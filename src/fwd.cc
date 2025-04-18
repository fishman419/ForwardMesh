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

int forward_file(int fd, const char *fpath, const ForwardAddress &address) {
  int r_fd = 0;
  int ret = 0;
  struct stat s;
  uint64_t f_size;
  const char *filename;
  ForwardRequest req;
  ForwardResponse res;
  ForwardFile *fmeta = NULL;
  uint32_t fmeta_length;
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
  ret = send_sync(fd, &req, sizeof(req));
  if (ret) {
    printf("send forward req error\n");
    goto out;
  }
  // ForwardNode
  if (ttl) {
    fnodes = (ForwardNode *)malloc(ttl * sizeof(ForwardNode));
    for (uint32_t i = 1; i < address.size(); ++i) {
      fnodes[i - 1].ip = address[i].first;
      fnodes[i - 1].port = address[i].second;
      printf("ip %u port %u\n", fnodes[i - 1].ip, fnodes[i - 1].port);
    }
    ret = send_sync(fd, fnodes, ttl * sizeof(ForwardNode));
    if (ret) {
      printf("send forward nodes error\n");
      goto out;
    }
  }

  fmeta = (ForwardFile *)malloc(fmeta_length);
  if (!fmeta) {
    printf("malloc error\n");
    ret = -1;
    goto out;
  }
  fmeta->length = strlen(filename) + 1;
  strncpy((char *)fmeta->filename, filename, strlen(filename));
  fmeta->filename[strlen(filename)] = '\0';
  ret = send_sync(fd, fmeta, fmeta_length);
  if (ret) {
    printf("send file meta error\n");
    goto out;
  }
  // data
  ret = forward_sync(r_fd, fd, f_size);
  if (ret) {
    printf("send file data error\n");
    goto out;
  }
  // wait res
  ret = recv_sync(fd, &res, sizeof(res));
  if (ret) {
    printf("recv res error\n");
    goto out;
  }
  if (res.retcode == ForwardSuccess) {
    LOG_INFO("forward success");
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

int resolve_address(char *raw_str, ForwardAddress *res) {
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
  int ret = log_init("forward_client.log");
  if (ret) {
    return -1;
  }

  atexit(log_close);

  int opt;
  ForwardAddress forward_address;
  char *fpath = NULL;

  LOG_INFO("Forward client started");
  while ((opt = getopt(argc, argv, "a:f:")) != -1) {
    switch (opt) {
      case 'a':
        if (resolve_address(optarg, &forward_address)) {
          printf("invalid address format\n");
          return -1;
        }
        if (forward_address.empty()) {
          printf("no address parsed\n");
          return -1;
        }
        break;
      case 'f':
        fpath = strdup(optarg);
        struct stat s;
        if (stat(fpath, &s)) {
          printf("stat error, fpath %s, %d\n", fpath, errno);
          return -1;
        }
        if (!S_ISREG(s.st_mode)) {
          printf("input file is not regular file, fpath %s, mode %d\n", fpath,
                 s.st_mode);
          return -1;
        }
        break;
      default:
        printf("Usage: %s <-a ip[:port],ip[:port],...ip[:port]> <-f file>\n",
               argv[0]);
        printf("Example: %s -a 127.0.0.1:40000,127.0.0.1:40001 -f testfile\n",
               argv[0]);
        return -1;
    }
  }
  if (!fpath) {
    printf("please input file\n");
    return -1;
  }
  // 在main函数中修改socket创建部分
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    LOG_ERROR("socket error, %d", errno);
    return -1;
  }

  // 添加socket选项确保可移植性
  int optval = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))) {
    printf("setsockopt error, %d\n", errno);
    close(sockfd);
    return -1;
  }
  struct sockaddr_in dst_addr;
  dst_addr.sin_family = AF_INET;
  dst_addr.sin_port = htons(forward_address[0].second);
  dst_addr.sin_addr.s_addr = forward_address[0].first;

  if (connect(sockfd, (struct sockaddr *)&dst_addr, sizeof(dst_addr)) < 0) {
    printf("connect error, %d\n", errno);
    return -1;
  }

  if (forward_file(sockfd, fpath, forward_address)) {
    printf("forward file error\n");
    return -1;
  }
  close(sockfd);
  return 0;
}