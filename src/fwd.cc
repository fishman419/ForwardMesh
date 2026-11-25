#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
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

namespace {
int SendFile(int fd, const char *fpath, const ForwardAddress &address) {
  int r_fd = -1;
  int ret = 0;
  struct stat s;
  uint64_t f_size = 0;
  std::string base_name;
  ForwardRequest req;
  ForwardResponse res;
  ForwardFile *fmeta = NULL;
  ForwardNode *fnodes = NULL;
  uint32_t fmeta_length = 0;
  size_t fname_len = 0;
  uint32_t ttl = static_cast<uint32_t>(address.size() - 1);
  uint64_t total_len = 0;

  r_fd = open(fpath, O_RDONLY);
  if (r_fd < 0) {
    LOG_ERROR("open file error, %d", errno);
    ret = -1;
    goto out;
  }
  if (fstat(r_fd, &s) != 0) {
    LOG_ERROR("stat error, fpath %s, %d", fpath, errno);
    ret = -1;
    goto out;
  }
  f_size = static_cast<uint64_t>(s.st_size);
  base_name = ExtractBasename(fpath);
  fname_len = base_name.size();
  if (fname_len == 0 || fname_len + 1 > kMaxFilenameLen) {
    LOG_ERROR("invalid filename: %s", fpath);
    ret = -1;
    goto out;
  }

  fmeta_length = static_cast<uint32_t>(sizeof(ForwardFile) + fname_len + 1);
  total_len = sizeof(req) + static_cast<uint64_t>(ttl) * sizeof(ForwardNode) +
              fmeta_length + f_size;
  if (total_len > UINT32_MAX) {
    LOG_ERROR("file too large for v1 protocol: %llu bytes",
              static_cast<unsigned long long>(f_size));
    ret = -1;
    goto out;
  }

  memset(&req, 0, sizeof(req));
  req.length = static_cast<uint32_t>(total_len);
  req.magic = kForwardMagic;
  req.version = kForwardVersion1;
  req.cmd = ForwardPush;
  req.ttl = static_cast<uint8_t>(ttl);
  req.id = 0;

  ret = SendSync(fd, &req, sizeof(req));
  if (ret) {
    LOG_ERROR("send forward req error");
    goto out;
  }

  if (ttl > 0) {
    fnodes = static_cast<ForwardNode *>(malloc(ttl * sizeof(ForwardNode)));
    if (!fnodes) {
      LOG_ERROR("malloc fnodes error");
      ret = -1;
      goto out;
    }
    for (size_t i = 1; i < address.size(); ++i) {
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

  fmeta = static_cast<ForwardFile *>(malloc(fmeta_length));
  if (!fmeta) {
    LOG_ERROR("malloc error");
    ret = -1;
    goto out;
  }
  fmeta->length = static_cast<uint32_t>(fname_len + 1);
  memcpy(fmeta->filename, base_name.c_str(), fname_len + 1);
  ret = SendSync(fd, fmeta, fmeta_length);
  if (ret) {
    LOG_ERROR("send file meta error");
    goto out;
  }

  ret = ForwardSync(r_fd, fd, static_cast<size_t>(f_size));
  if (ret) {
    LOG_ERROR("send file data error");
    goto out;
  }

  ret = RecvSync(fd, &res, sizeof(res));
  if (ret) {
    LOG_ERROR("recv res error");
    goto out;
  }
  if (res.magic != kForwardMagic || res.retcode != ForwardSuccess) {
    LOG_ERROR("forward failed, magic=0x%x retcode=%u", res.magic, res.retcode);
    ret = -1;
  } else {
    LOG_INFO("push file(%s) success (%llu bytes)", fpath,
             static_cast<unsigned long long>(f_size));
  }

out:
  if (r_fd >= 0) {
    close(r_fd);
  }
  free(fnodes);
  free(fmeta);
  return ret;
}

int PullFile(int fd, const char *remote_path, const char *local_path,
             const ForwardAddress &address) {
  int ret = 0;
  int w_fd = -1;
  ForwardRequest req;
  ForwardResponse res;
  uint32_t ttl = static_cast<uint32_t>(address.size() - 1);
  size_t remote_fname_len = strlen(remote_path);
  uint32_t fmeta_length = 0;
  uint32_t recv_fname_len = 0;
  uint32_t file_data_len = 0;
  uint32_t total_fmeta_len = 0;
  ForwardFile recv_fmeta_hdr;
  ForwardFile *recv_fmeta = NULL;
  ForwardNode *fnodes = NULL;
  ForwardFile *fmeta = NULL;

  if (!IsSafeRelativePath(remote_path) ||
      remote_fname_len + 1 > kMaxFilenameLen) {
    LOG_ERROR("invalid remote relative path: %s", remote_path);
    return -1;
  }

  fmeta_length =
      static_cast<uint32_t>(sizeof(ForwardFile) + remote_fname_len + 1);
  fmeta = static_cast<ForwardFile *>(malloc(fmeta_length));
  if (!fmeta) {
    LOG_ERROR("malloc error");
    return -1;
  }

  memset(&req, 0, sizeof(req));
  req.length = static_cast<uint32_t>(sizeof(req) + ttl * sizeof(ForwardNode) +
                                     fmeta_length);
  req.magic = kForwardMagic;
  req.version = kForwardVersion1;
  req.cmd = ForwardPull;
  req.ttl = static_cast<uint8_t>(ttl);
  req.id = 0;

  ret = SendSync(fd, &req, sizeof(req));
  if (ret) {
    LOG_ERROR("send pull req error");
    goto out;
  }

  if (ttl > 0) {
    fnodes = static_cast<ForwardNode *>(malloc(ttl * sizeof(ForwardNode)));
    if (!fnodes) {
      LOG_ERROR("malloc fnodes error");
      ret = -1;
      goto out;
    }
    for (size_t i = 1; i < address.size(); ++i) {
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

  fmeta->length = static_cast<uint32_t>(remote_fname_len + 1);
  memcpy(fmeta->filename, remote_path, remote_fname_len + 1);
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

  if (res.magic != kForwardMagic || res.retcode != ForwardSuccess) {
    LOG_ERROR("pull rejected by server, magic=0x%x retcode=%u", res.magic,
              res.retcode);
    ret = -1;
    goto out;
  }

  if (res.cmd != ForwardPush) {
    LOG_ERROR("unexpected response cmd %u", res.cmd);
    ret = -1;
    goto out;
  }

  ret = RecvSync(fd, &recv_fmeta_hdr, sizeof(recv_fmeta_hdr));
  if (ret) {
    LOG_ERROR("recv file meta error");
    goto out;
  }

  if (recv_fmeta_hdr.length == 0 || recv_fmeta_hdr.length > kMaxFilenameLen) {
    LOG_ERROR("invalid recv file meta length %u", recv_fmeta_hdr.length);
    ret = -1;
    goto out;
  }

  recv_fname_len = recv_fmeta_hdr.length - 1;
  total_fmeta_len = sizeof(ForwardFile) + recv_fmeta_hdr.length;
  if (res.length < sizeof(ForwardResponse) + total_fmeta_len) {
    LOG_ERROR("invalid pull response packet length %u", res.length);
    ret = -1;
    goto out;
  }

  recv_fmeta = static_cast<ForwardFile *>(malloc(total_fmeta_len + 1));
  if (!recv_fmeta) {
    LOG_ERROR("malloc error");
    ret = -1;
    goto out;
  }
  recv_fmeta->length = recv_fmeta_hdr.length;
  ret = RecvSync(fd, recv_fmeta->filename, recv_fmeta_hdr.length);
  if (ret) {
    LOG_ERROR("recv filename error");
    goto out;
  }
  recv_fmeta->filename[recv_fname_len] = '\0';

  file_data_len = res.length - sizeof(ForwardResponse) - total_fmeta_len;
  w_fd = open(local_path, O_CREAT | O_TRUNC | O_WRONLY, kDefaultFileMode);
  if (w_fd < 0) {
    LOG_ERROR("open file(%s) error, %d", local_path, errno);
    ret = -1;
    goto out;
  }

  ret = ForwardSync(fd, w_fd, file_data_len);
  close(w_fd);
  w_fd = -1;
  if (ret) {
    LOG_ERROR("receive file data error");
    unlink(local_path);
    goto out;
  }

  ret = RecvSync(fd, &res, sizeof(res));
  if (ret) {
    LOG_ERROR("recv final response error");
    unlink(local_path);
    goto out;
  }

  if (res.magic == kForwardMagic && res.retcode == ForwardSuccess) {
    LOG_INFO("pull file(%s) to %s success (%u bytes)", remote_path, local_path,
             file_data_len);
  } else {
    LOG_ERROR("pull failed at final ack, retcode %u", res.retcode);
    unlink(local_path);
    ret = -1;
  }

out:
  if (w_fd >= 0) {
    close(w_fd);
  }
  free(fnodes);
  free(fmeta);
  free(recv_fmeta);
  return ret;
}

int ResolveAddress(const char *raw_str, ForwardAddress *res) {
  std::string cxx_str(raw_str);
  if (cxx_str.empty()) {
    return -1;
  }
  size_t pos = 0;
  while (pos < cxx_str.length()) {
    size_t next = cxx_str.find(',', pos);
    std::string addr;
    if (next == std::string::npos) {
      addr = cxx_str.substr(pos);
      pos = cxx_str.length();
    } else {
      if (next == pos || next + 1 == cxx_str.length()) {
        LOG_ERROR("empty hop in address chain: %s", raw_str);
        return -1;
      }
      addr = cxx_str.substr(pos, next - pos);
      pos = next + 1;
    }

    int port = static_cast<int>(kDefaultPort);
    size_t port_pos = addr.find(':');
    std::string ip_part = addr;
    if (port_pos != std::string::npos) {
      ip_part = addr.substr(0, port_pos);
      std::string port_part = addr.substr(port_pos + 1);
      char *endptr = NULL;
      long parsed_port = strtol(port_part.c_str(), &endptr, 10);
      if (port_part.empty() || *endptr != '\0' || parsed_port <= 0 ||
          parsed_port > 65535) {
        LOG_ERROR("port is invalid in address: %s", addr.c_str());
        return -1;
      }
      port = static_cast<int>(parsed_port);
    }

    uint32_t ip = 0;
    if (inet_pton(AF_INET, ip_part.c_str(), &ip) != 1) {
      LOG_ERROR("ip address is invalid: %s", ip_part.c_str());
      return -1;
    }
    res->emplace_back(ip, static_cast<uint32_t>(port));
  }

  if (res->empty() || res->size() - 1 > kMaxForwardTTL) {
    LOG_ERROR("address chain length %zu invalid (max hops %u)", res->size(),
              kMaxForwardTTL);
    return -1;
  }
  return 0;
}

void PrintUsage(const char *prog) {
  printf("Usage: %s -a ip:port,... [options]\n", prog);
  printf("  -a addr : comma-separated list of ip:port forwarding chain\n");
  printf("  -f file : file to send (push) or local output path (pull)\n");
  printf("  -g file : remote relative file path to get (pull mode)\n");
  printf("  -h      : show this help\n");
  printf("Examples:\n");
  printf("  Push: %s -a 127.0.0.1:40000 -f test.txt\n", prog);
  printf("  Pull: %s -a 127.0.0.1:40000 -g remote.txt -f local.txt\n", prog);
}
}  // namespace

int main(int argc, char *argv[]) {
  signal(SIGPIPE, SIG_IGN);

  if (LogInit("forward_client.log") != 0) {
    return 1;
  }
  atexit(LogClose);

  int opt;
  ForwardAddress forward_address;
  std::string fpath;
  std::string remote_path;
  bool pull_mode = false;

  LOG_INFO("Forward client started");
  while ((opt = getopt(argc, argv, "a:f:g:h")) != -1) {
    switch (opt) {
      case 'a':
        forward_address.clear();
        if (ResolveAddress(optarg, &forward_address) != 0) {
          LOG_ERROR("invalid address format: %s", optarg);
          return 1;
        }
        break;
      case 'f':
        fpath = optarg;
        break;
      case 'g':
        pull_mode = true;
        remote_path = optarg;
        break;
      case 'h':
        PrintUsage(argv[0]);
        return 0;
      default:
        PrintUsage(argv[0]);
        return 1;
    }
  }

  if (forward_address.empty()) {
    LOG_ERROR("please specify address with -a");
    return 1;
  }

  if (!pull_mode && fpath.empty()) {
    LOG_ERROR("please specify file with -f (push) or -g (pull)");
    return 1;
  }

  if (!pull_mode) {
    struct stat s;
    if (stat(fpath.c_str(), &s) != 0) {
      LOG_ERROR("stat error, fpath %s, %d", fpath.c_str(), errno);
      return 1;
    }
    if (!S_ISREG(s.st_mode)) {
      LOG_ERROR("input file is not regular file, fpath %s, mode %d",
                fpath.c_str(), s.st_mode);
      return 1;
    }
  }

  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    LOG_ERROR("socket error, %d", errno);
    return 1;
  }
  SetSocketTimeout(sockfd, kDefaultSocketTimeoutSec);

  struct sockaddr_in dst_addr;
  memset(&dst_addr, 0, sizeof(dst_addr));
  dst_addr.sin_family = AF_INET;
  dst_addr.sin_port = htons(static_cast<uint16_t>(forward_address[0].second));
  dst_addr.sin_addr.s_addr = forward_address[0].first;

  if (connect(sockfd, reinterpret_cast<struct sockaddr *>(&dst_addr),
              sizeof(dst_addr)) < 0) {
    LOG_ERROR("connect error, %d", errno);
    close(sockfd);
    return 1;
  }

  int rc = 0;
  if (pull_mode) {
    const char *out_path = fpath.empty() ? "./downloaded_file" : fpath.c_str();
    if (PullFile(sockfd, remote_path.c_str(), out_path, forward_address) != 0) {
      LOG_ERROR("pull file error");
      rc = 1;
    }
  } else {
    if (SendFile(sockfd, fpath.c_str(), forward_address) != 0) {
      LOG_ERROR("forward file error");
      rc = 1;
    }
  }

  close(sockfd);
  return rc;
}
