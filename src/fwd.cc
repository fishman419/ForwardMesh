#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <utility>
#include <vector>

#include "logger.h"
#include "protocol.h"
#include "util.h"

typedef std::vector<std::pair<uint32_t, uint32_t>> ForwardAddress;

int SendFile(int fd, const char *fpath, const ForwardAddress &address) {
  int r_fd = 0;
  int ret = 0;
  struct stat s;
  uint64_t f_size;
  const char *filename;
  ForwardRequest req;
  ForwardResponse res;
  ForwardFile *fmeta = NULL;
  uint32_t fmeta_length;
  size_t fname_len = 0;
  uint32_t ttl = address.size() - 1;
  ForwardNode *fnodes = NULL;
  r_fd = open(fpath, O_RDONLY);
  if (r_fd < 0) {
    LOG_ERROR("open file error, %d", errno);
    ret = -1;
    goto out;
  }
  if (stat(fpath, &s)) {
    LOG_ERROR("stat error, fpath %s, %d", fpath, errno);
    ret = -1;
    goto out;
  }
  f_size = s.st_size;
  filename = strrchr(fpath, '/');
  if (!filename) {
    filename = fpath;
  } else {
    filename += 1;
  }
  fmeta_length = sizeof(ForwardFile) + strlen(filename) + 1;
  // ForwardRequest
  req.length = sizeof(req) + ttl * sizeof(ForwardNode) + fmeta_length + f_size;
  req.magic = kForwardMagic;
  req.version = kForwardVersion1;
  req.cmd = ForwardPush;
  req.ttl = ttl;
  req.id = 0;
  ret = SendSync(fd, &req, sizeof(req));
  if (ret) {
    LOG_ERROR("send forward req error");
    goto out;
  }
  // ForwardNode
  if (ttl) {
    fnodes = (ForwardNode *)malloc(ttl * sizeof(ForwardNode));
    for (uint32_t i = 1; i < address.size(); ++i) {
      fnodes[i - 1].ip = address[i].first;
      fnodes[i - 1].port = address[i].second;
      LOG_DEBUG("ip %u port %u", fnodes[i - 1].ip, fnodes[i - 1].port);
    }
    ret = SendSync(fd, fnodes, ttl * sizeof(ForwardNode));
    if (ret) {
      LOG_ERROR("send forward nodes error");
      goto out;
    }
  }

  fmeta = (ForwardFile *)malloc(fmeta_length);
  if (!fmeta) {
    LOG_ERROR("malloc error");
    ret = -1;
    goto out;
  }
  fname_len = strlen(filename);
  fmeta->length = fname_len + 1;
  strncpy((char *)fmeta->filename, filename, fname_len);
  fmeta->filename[fname_len] = '\0';
  ret = SendSync(fd, fmeta, fmeta_length);
  if (ret) {
    LOG_ERROR("send file meta error");
    goto out;
  }
  // data
  ret = ForwardSync(r_fd, fd, f_size);
  if (ret) {
    LOG_ERROR("send file data error");
    goto out;
  }
  // wait res
  ret = RecvSync(fd, &res, sizeof(res));
  if (ret) {
    LOG_ERROR("recv res error");
    goto out;
  }
  if (res.retcode == ForwardSuccess) {
    LOG_DEBUG("forward success");
  } else {
    LOG_ERROR("forward failed, retcode %d", res.retcode);
  }

out:
  if (r_fd > 0) {
    close(r_fd);
  }
  if (fnodes) {
    free(fnodes);
  }
  if (fmeta) {
    free(fmeta);
  }
  return ret;
}

int PullFile(int fd, const char *remote_path, const char *local_path,
             const ForwardAddress &address) {
  int ret = 0;
  int w_fd = 0;
  ForwardRequest req;
  ForwardResponse res;
  uint32_t ttl = address.size() - 1;
  size_t remote_fname_len = strlen(remote_path);
  uint32_t fmeta_length = sizeof(ForwardFile) + remote_fname_len + 1;
  uint32_t recv_fmeta_len = 0;
  uint32_t recv_fname_len = 0;
  uint32_t file_data_len = 0;
  uint32_t total_fmeta_len = 0;
  ForwardFile *recv_fmeta = NULL;

  ForwardNode *fnodes = NULL;
  ForwardFile *fmeta = (ForwardFile *)malloc(fmeta_length);
  if (!fmeta) {
    LOG_ERROR("malloc error");
    return -1;
  }

  req.length = sizeof(req) + ttl * sizeof(ForwardNode) + fmeta_length;
  req.magic = kForwardMagic;
  req.version = kForwardVersion1;
  req.cmd = ForwardPull;
  req.ttl = ttl;
  req.id = 0;

  ret = SendSync(fd, &req, sizeof(req));
  if (ret) {
    LOG_ERROR("send pull req error");
    goto out;
  }

  if (ttl) {
    fnodes = (ForwardNode *)malloc(ttl * sizeof(ForwardNode));
    for (uint32_t i = 1; i < address.size(); ++i) {
      fnodes[i - 1].ip = address[i].first;
      fnodes[i - 1].port = address[i].second;
      LOG_DEBUG("ip %u port %u", fnodes[i - 1].ip, fnodes[i - 1].port);
    }
    ret = SendSync(fd, fnodes, ttl * sizeof(ForwardNode));
    if (ret) {
      LOG_ERROR("send pull nodes error");
      goto out;
    }
  }

  fmeta->length = remote_fname_len + 1;
  memcpy(fmeta->filename, remote_path, remote_fname_len);
  fmeta->filename[remote_fname_len] = '\0';
  ret = SendSync(fd, fmeta, fmeta_length);
  if (ret) {
    LOG_ERROR("send pull fmeta error");
    goto out;
  }

  ret = RecvSync(fd, &res, sizeof(res));
  if (ret) {
    LOG_ERROR("recv pull response error");
    goto out;
  }

  if (res.cmd != ForwardPush) {
    LOG_ERROR("unexpected response cmd %d", res.cmd);
    ret = -1;
    goto out;
  }

  recv_fmeta_len = sizeof(ForwardFile);
  recv_fmeta = (ForwardFile *)malloc(recv_fmeta_len);
  if (!recv_fmeta) {
    LOG_ERROR("malloc error");
    ret = -1;
    goto out;
  }

  ret = RecvSync(fd, recv_fmeta, recv_fmeta_len);
  if (ret) {
    LOG_ERROR("recv file meta error");
    goto out;
  }

  recv_fname_len = recv_fmeta->length - 1;
  total_fmeta_len = sizeof(ForwardFile) + recv_fname_len + 1;
  recv_fmeta = (ForwardFile *)realloc(recv_fmeta, total_fmeta_len);
  ret = RecvSync(fd, recv_fmeta->filename, recv_fname_len + 1);
  if (ret) {
    LOG_ERROR("recv filename error");
    goto out;
  }

  file_data_len = res.length - sizeof(ForwardResponse) - total_fmeta_len;
  if (file_data_len < 0) {
    LOG_ERROR("invalid file data len %d", file_data_len);
    ret = -1;
    goto out;
  }

  w_fd = open(local_path, O_CREAT | O_TRUNC | O_RDWR, kDefaultFileMode);
  if (w_fd < 0) {
    LOG_ERROR("open file error, %d", errno);
    ret = -1;
    goto out;
  }

  ret = ForwardSync(fd, w_fd, file_data_len);
  close(w_fd);
  if (ret) {
    LOG_ERROR("forward file error");
    goto out;
  }

  ret = RecvSync(fd, &res, sizeof(res));
  if (ret) {
    LOG_ERROR("recv final response error");
    goto out;
  }

  if (res.retcode == ForwardSuccess) {
    LOG_INFO("pull file(%s) to %s success", remote_path, local_path);
  } else {
    LOG_ERROR("pull failed, retcode %d", res.retcode);
    ret = -1;
  }

out:
  if (fnodes) {
    free(fnodes);
  }
  if (fmeta) {
    free(fmeta);
  }
  if (recv_fmeta) {
    free(recv_fmeta);
  }
  return ret;
}

int ResolveAddress(char *raw_str, ForwardAddress *res) {
  std::string cxx_str(raw_str);
  size_t pos = 0;
  size_t next = 0;
  while (pos != std::string::npos && pos < cxx_str.length()) {
    next = cxx_str.find(',', pos);
    std::string addr;
    uint32_t port = kDefaultPort;
    if (next == std::string::npos) {
      addr = cxx_str.substr(pos, next);
      pos = std::string::npos;
    } else {
      addr = cxx_str.substr(pos, next - pos);
      pos = next + 1;
    }
    size_t port_pos = addr.find(':');
    if (port_pos != std::string::npos) {
      port = atoi(addr.substr(port_pos + 1, std::string::npos).c_str());
    }
    uint32_t ip;
    if (inet_pton(AF_INET, addr.substr(0, port_pos).c_str(), &ip) < 0) {
      LOG_ERROR("ip address is invalid, %s", addr.substr(0, port_pos).c_str());
      return -1;
    }
    res->emplace_back(ip, port);
  }
  return 0;
}

int main(int argc, char *argv[]) {
  int ret = LogInit("forward_client.log");
  if (ret) {
    return -1;
  }

  atexit(LogClose);

  int opt;
  ForwardAddress forward_address;
  char *fpath = NULL;
  char *remote_path = NULL;
  int pull_mode = 0;

  LOG_INFO("Forward client started");
  while ((opt = getopt(argc, argv, "a:f:g:h")) != -1) {
    switch (opt) {
      case 'a':
        if (ResolveAddress(optarg, &forward_address)) {
          LOG_ERROR("invalid address format");
          return -1;
        }
        if (forward_address.empty()) {
          LOG_ERROR("no address parsed");
          return -1;
        }
        break;
      case 'f':
        fpath = strdup(optarg);
        break;
      case 'g':
        pull_mode = 1;
        remote_path = strdup(optarg);
        break;
      case 'h':
        printf("Usage: %s -a ip:port,... [options]\n", argv[0]);
        printf("  -a addr : comma-separated list of ip:port forwarding chain\n");
        printf("  -f file : file to send (push mode)\n");
        printf("  -g file : remote file to get (pull mode)\n");
        printf("  -h      : show this help\n");
        printf("Examples:\n");
        printf("  Push: %s -a 127.0.0.1:40000 -f test.txt\n", argv[0]);
        printf("  Pull: %s -a 127.0.0.1:40000 -g /path/to/remote.txt\n",
               argv[0]);
        return 0;
      default:
        printf("Usage: %s -a ip:port,... [options]\n", argv[0]);
        printf("  -a addr : comma-separated list of ip:port forwarding chain\n");
        printf("  -f file : file to send (push mode)\n");
        printf("  -g file : remote file to get (pull mode)\n");
        printf("  -h      : show this help\n");
        return -1;
    }
  }

  if (forward_address.empty()) {
    LOG_ERROR("please specify address with -a");
    return -1;
  }

  if (!pull_mode && !fpath) {
    LOG_ERROR("please specify file with -f (push) or -g (pull)");
    return -1;
  }

  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    LOG_ERROR("socket error, %d", errno);
    return -1;
  }

  int optval = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))) {
    LOG_ERROR("setsockopt error, %d", errno);
    close(sockfd);
    return -1;
  }
  struct sockaddr_in dst_addr;
  memset(&dst_addr, 0, sizeof(dst_addr));
  dst_addr.sin_family = AF_INET;
  dst_addr.sin_port = htons(forward_address[0].second);
  dst_addr.sin_addr.s_addr = forward_address[0].first;

  if (connect(sockfd, (struct sockaddr *)&dst_addr, sizeof(dst_addr)) < 0) {
    LOG_ERROR("connect error, %d", errno);
    close(sockfd);
    return -1;
  }

  if (pull_mode) {
    if (PullFile(sockfd, remote_path, fpath ? fpath : "./downloaded_file",
                 forward_address)) {
      LOG_ERROR("pull file error");
      close(sockfd);
      return -1;
    }
  } else {
    struct stat s;
    if (stat(fpath, &s)) {
      LOG_ERROR("stat error, fpath %s, %d", fpath, errno);
      close(sockfd);
      return -1;
    }
    if (!S_ISREG(s.st_mode)) {
      LOG_ERROR("input file is not regular file, fpath %s, mode %d", fpath,
                s.st_mode);
      close(sockfd);
      return -1;
    }
    if (SendFile(sockfd, fpath, forward_address)) {
      LOG_ERROR("forward file error");
      close(sockfd);
      return -1;
    }
  }
  close(sockfd);
  return 0;
}