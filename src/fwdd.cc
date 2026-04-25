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

#include "logger.h"
#include "protocol.h"
#include "util.h"

#if defined(__APPLE__)
#define PLATFORM_MACOS 1
#elif defined(__linux__)
#define PLATFORM_LINUX 1
#endif

int ForwardNext(int fd, ForwardRequest *req, ForwardNode *nodes,
                ForwardFile *fmeta) {
  int ret;
  ForwardRequest next_req;
  ForwardResponse res;
  int next_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (next_fd < 0) {
    LOG_ERROR("socket error, %d", errno);
    ret = -1;
    res.retcode = ForwardInternalError;
    goto out_response;
  }
  struct sockaddr_in dst_addr;
  memset(&dst_addr, 0, sizeof(dst_addr));
  dst_addr.sin_family = AF_INET;
  dst_addr.sin_port = htons(nodes[0].port);
  dst_addr.sin_addr.s_addr = nodes[0].ip;

  if (connect(next_fd, (struct sockaddr *)&dst_addr, sizeof(dst_addr)) < 0) {
    LOG_ERROR("connect error, %d", errno);
    ret = -1;
    res.retcode = ForwardUnreachable;
    goto out_response;
  }
  memcpy(&next_req, req, sizeof(next_req));
  next_req.length -= sizeof(ForwardNode);
  next_req.ttl -= 1;
  ret = SendSync(next_fd, &next_req, sizeof(ForwardRequest));
  if (ret) {
    LOG_ERROR("send forward req error %d", errno);
    ret = -1;
    res.retcode = ForwardInterrupt;
    goto out_response;
  }
  if (next_req.ttl) {
    ret = SendSync(next_fd, nodes + 1, sizeof(ForwardNode) * next_req.ttl);
    if (ret) {
      LOG_ERROR("send forward nodes error %d", errno);
      ret = -1;
      res.retcode = ForwardInterrupt;
      goto out_response;
    }
  }
  ret = SendSync(next_fd, fmeta, sizeof(ForwardFile) + fmeta->length);
  if (ret) {
    LOG_ERROR("send forward file error");
    ret = -1;
    res.retcode = ForwardInterrupt;
    goto out_response;
  }

  ret = ForwardSync(fd, next_fd,
                     next_req.length - sizeof(ForwardRequest) -
                         sizeof(ForwardNode) * next_req.ttl -
                         sizeof(ForwardFile) - fmeta->length);
  if (ret) {
    LOG_ERROR("forward data error");
    ret = -1;
    res.retcode = ForwardInterrupt;
    goto out_response;
  }

  ret = RecvSync(next_fd, &res, sizeof(res));
  if (ret) {
    LOG_ERROR("recv response error");
    ret = -1;
    res.retcode = ForwardInterrupt;
  }
out_response:
  res.retcode = ForwardInternalError;
  res.length = sizeof(res);
  res.magic = kForwardMagic;
  res.version = kForwardVersion1;
  res.cmd = req->cmd;
  res.ttl = req->ttl;
  res.id = req->id;
  ret = SendSync(fd, &res, sizeof(res));
  if (ret) {
    LOG_ERROR("send response error");
    ret = -1;
  }
out:
  if (next_fd > 0) {
    close(next_fd);
  }
  return ret;
}

int StoreLocal(int fd, ForwardRequest *req, ForwardFile *fmeta) {
  ForwardResponse res;
  int ret = 0;
  int w_fd =
      open((const char *)fmeta->filename, O_CREAT | O_RDWR, kDefaultFileMode);
  if (w_fd < 0) {
    LOG_ERROR("open error, %d", errno);
    return -1;
  }
  ret = ForwardSync(fd, w_fd,
                     req->length - sizeof(ForwardRequest) -
                         sizeof(ForwardFile) - fmeta->length);
  if (ret) {
    LOG_ERROR("forward data error");
    ret = -1;
    goto out;
  }
  close(w_fd);
  res.length = sizeof(res);
  res.magic = kForwardMagic;
  res.version = kForwardVersion1;
  res.cmd = req->cmd;
  res.ttl = req->ttl;
  res.retcode = ForwardSuccess;
  res.id = req->id;
  ret = SendSync(fd, &res, sizeof(res));
  if (ret) {
    LOG_ERROR("send response error");
    ret = -1;
    goto out;
  }
  LOG_INFO("store file(%s) to local", fmeta->filename);
out:
  if (w_fd > 0) {
    close(w_fd);
  }
  return ret;
}

int ForwardLoop(int port) {
  int ret;
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    LOG_ERROR("socket error, %d", errno);
    return -1;
  }
  int optval = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))) {
    LOG_ERROR("setsockopt error, %d", errno);
    return -1;
  }
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);
  if (bind(sockfd, (struct sockaddr *)&address, sizeof(address))) {
    LOG_ERROR("bind error, %d", errno);
    return -1;
  }
  if (listen(sockfd, kDefaultBacklog)) {
    LOG_ERROR("listen error, %d", errno);
    return -1;
  }
  while (1) {
    struct sockaddr_in src_addr;
    socklen_t addr_len = sizeof(src_addr);
    char ip_str[64];
    ForwardRequest req;
    ForwardNode *fnodes = NULL;
    ForwardFile *fmeta = NULL;
    int fd = accept(sockfd, (struct sockaddr *)&src_addr, &addr_len);
    if (fd < 0) {
      LOG_ERROR("accept error, %d", errno);
      continue;
    }
    inet_ntop(AF_INET, &src_addr.sin_addr, ip_str, sizeof(ip_str));
    LOG_DEBUG("accept %d, ip: %s, port: %d", fd, ip_str,
              ntohs(src_addr.sin_port));
    ret = RecvSync(fd, &req, sizeof(uint32_t));
    if (ret) {
      LOG_ERROR("recv req size");
      goto cleanup_fd;
    }
    // ForwardRequest
    ret = RecvSync(fd, (char *)&req + sizeof(uint32_t),
                    sizeof(ForwardRequest) - sizeof(uint32_t));
    if (ret) {
      LOG_ERROR("recv req error");
      goto cleanup_fd;
    }
    LOG_DEBUG(
        "[header]length %d, magic %d, version %d, cmd %d, ttl %d, id "
        "%llu",
        req.length, req.magic, req.version, req.cmd, req.ttl, req.id);
    // ForwardNode
    if (req.ttl) {
      fnodes = (ForwardNode *)malloc(sizeof(ForwardNode) * req.ttl);
      if (!fnodes) {
        LOG_ERROR("malloc fnodes error");
        goto cleanup_fd;
      }
      ret = RecvSync(fd, fnodes, sizeof(ForwardNode) * req.ttl);
      if (ret) {
        LOG_ERROR("recv nodes error");
        goto cleanup_fnodes;
      }
      for (uint32_t i = 0; i < req.ttl; ++i) {
        LOG_DEBUG("[node%u]ip %u, port %u", i, fnodes[i].ip, fnodes[i].port);
      }
    }
    // ForwardFile
    uint32_t f_length;
    ret = RecvSync(fd, &f_length, sizeof(uint32_t));
    if (ret) {
      LOG_ERROR("recv file size error");
      goto cleanup_fnodes;
    }
    fmeta = (ForwardFile *)malloc(sizeof(ForwardFile) + f_length);
    if (!fmeta) {
      LOG_ERROR("malloc fmeta error");
      goto cleanup_fnodes;
    }
    fmeta->length = f_length;
    ret = RecvSync(fd, fmeta->filename, f_length);
    if (ret) {
      LOG_ERROR("recv file name error");
      goto cleanup_fmeta;
    }
    // Forward to next node
    if (req.ttl > 0) {
      ret = ForwardNext(fd, &req, fnodes, fmeta);
    } else {
      ret = StoreLocal(fd, &req, fmeta);
    }

  cleanup_fmeta:
    free(fmeta);
    fmeta = NULL;
  cleanup_fnodes:
    free(fnodes);
    fnodes = NULL;
  cleanup_fd:
    close(fd);
    continue;
  }
  return 0;
}

int main(int argc, char *argv[]) {
  int opt;
  int port = kDefaultPort;
  char *dir = NULL;

  while ((opt = getopt(argc, argv, "p:d:h")) != -1) {
    switch (opt) {
      case 'p':
        port = atoi(optarg);
        break;
      case 'd':
        dir = strdup(optarg);
        struct stat s;
        if (stat(dir, &s)) {
          LOG_ERROR("stat error, dir %s, %d", dir, errno);
          return -1;
        }
        if (!S_ISDIR(s.st_mode)) {
          LOG_ERROR("input dir is not directory, dir %s, mode %d", dir,
                    s.st_mode);
          return -1;
        }
        break;
      case 'h':
        printf("Usage: %s [-p port] [-d dir]\n", argv[0]);
        printf("  -p port  : specify port (default %d)\n", kDefaultPort);
        printf("  -d dir   : specify working directory\n");
        printf("  -h       : show this help\n");
        return 0;
      default:
        printf("Usage: %s [-p port] [-d dir]\n", argv[0]);
        printf("  -p port  : specify port (default %d)\n", kDefaultPort);
        printf("  -d dir   : specify working directory\n");
        printf("  -h       : show this help\n");
        return -1;
    }
  }
  if (!dir) {
    dir = strdup("/");
  }
  char log_path[PATH_MAX];
  snprintf(log_path, sizeof(log_path), "%s/forward_server.log", dir);
  int ret = LogInit(log_path);
  if (ret) {
    return -1;
  }
  atexit(LogClose);

  LOG_INFO("Forward daemon started, working directory: %s", dir);
  pid_t pid = fork();
  if (pid < 0) {
    LOG_ERROR("fork error, %d", errno);
    return -1;
  }
  // parent
  if (pid > 0) {
    // printf("parent exit, child pid: %d\n", pid);
    return 0;
  }
  if (setsid() < 0) {
    LOG_ERROR("setsid error, %d", errno);
    return -1;
  }
  signal(SIGCHLD, SIG_IGN);
  signal(SIGHUP, SIG_IGN);
  pid = fork();
  if (pid < 0) {
    LOG_ERROR("fork error, %d", errno);
    return -1;
  }
  if (pid > 0) {
    return 0;
  }
  umask(0);
  chdir(dir);
  free(dir);
  return ForwardLoop(port);
}
