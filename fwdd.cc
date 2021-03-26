#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "protocol.h"

int recv_sync(int fd, void *buffer, int len) {
  int ret;
  int offset = 0;
  while (len > 0) {
    ret = read(fd, (char *)buffer + offset, len);
    if (ret < 0) {
      printf("[FWDD]read error, %d\n", errno);
      return -1;
    }
    offset += ret;
    len -= ret;
  }
  return 0;
}

int forward_sync(int fd_src, int fd_dst, int len) {
  char buffer[16384];
  int ret;
  int write_len;
  while (len > 0) {
    ret = read(fd_src, buffer, 16384);
    if (ret < 0) {
      printf("[FWDD]read error, %d\n", errno);
      return -1;
    }
    write_len = write(fd_dst, buffer, ret);
    if (write_len != ret) {
      printf("[FWDD]write error, %d %d\n", write_len, errno);
      return -1;
    }
    len -= ret;
  }
  return 0;
}

int forward_next(int fd, ForwardRequest *req, ForwardNode *nodes,
                 ForwardFile *fmeta) {
  int ret;
  int next_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (next_fd < 0) {
    printf("[FWDD]socket error, %d\n", errno);
    return -1;
  }
  struct sockaddr_in dst_addr;
  dst_addr.sin_family = AF_INET;
  dst_addr.sin_port = htons(nodes[0].port);
  dst_addr.sin_addr.s_addr = nodes[0].ip;

  if (connect(next_fd, (struct sockaddr *)&dst_addr, sizeof(dst_addr)) < 0) {
    printf("[FWDD]connect error, %d\n", errno);
    return -1;
  }
  req->length -= sizeof(ForwardNode);
  req->ttl -= 1;
  ret = write(next_fd, req, sizeof(ForwardRequest));
  if (ret != sizeof(ForwardRequest)) {
    printf("[FWDD]write error %d\n", errno);
    return -1;
  }
  if (req->ttl) {
    ret = write(next_fd, nodes + 1, sizeof(ForwardNode) * req->ttl);
    if (ret != sizeof(ForwardNode) * req->ttl) {
      printf("[FWDD]write error %d\n", errno);
      return -1;
    }
  }
  ret = write(next_fd, fmeta, sizeof(ForwardFile) + fmeta->length);
  if (ret != sizeof(ForwardFile) + fmeta->length) {
    printf("[FWDD]write error %d\n", errno);
    return -1;
  }

  ret = forward_sync(fd, next_fd, req->length - sizeof(ForwardRequest) -
                                      sizeof(ForwardNode) * req->ttl -
                                      sizeof(ForwardFile) - fmeta->length);
  if (ret) {
    printf("[FWDD]forward data error\n");
    return -1;
  }

  printf("[FWDD]forward data success\n");
  close(next_fd);
  return 0;
}

int forward_loop(int port) {
  int ret;
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    printf("[FWDD]socket error, %d\n", errno);
    return -1;
  }
  int optval = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &optval,
                 sizeof(optval))) {
    printf("[FWDD]setsockopt error, %d\n", errno);
    return -1;
  }
  struct sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(port);
  if (bind(sockfd, (struct sockaddr *)&address, sizeof(address))) {
    printf("[FWDD]bind error, %d\n", errno);
    return -1;
  }
  if (listen(sockfd, 5)) {
    printf("[FWDD]listener error, %d\n", errno);
    return -1;
  }
  while (1) {
    struct sockaddr_in src_addr;
    socklen_t addr_len;
    char ip_str[64];
    ForwardRequest req;
    ForwardNode *fnodes = NULL;
    int fd = accept(sockfd, (struct sockaddr *)&src_addr, &addr_len);
    if (fd < 0) {
      printf("[FWDD]accept error, %d\n", errno);
      return -1;
    }
    inet_ntop(AF_INET, &src_addr.sin_addr, ip_str, sizeof(ip_str));
    printf("[FWDD]accept %d, ip: %s, port: %d\n", fd, ip_str,
           ntohs(src_addr.sin_port));
    ret = recv_sync(fd, &req, sizeof(uint32_t));
    if (ret) {
      printf("[FWDD]recv req size\n");
      return -1;
    }
    // ForwardRequest
    ret = recv_sync(fd, (char *)&req + sizeof(uint32_t),
                    sizeof(ForwardRequest) - sizeof(uint32_t));
    if (ret) {
      printf("[FWDD]recv req error\n");
      return -1;
    }
    printf(
        "[FWDD][header]length %d, magic %d, version %d, cmd %d, ttl %d, id "
        "%lu\n",
        req.length, req.magic, req.version, req.cmd, req.ttl, req.id);
    // ForwardNode
    if (req.ttl) {
      fnodes = (ForwardNode *)malloc(sizeof(ForwardNode) * req.ttl);
      ret = recv_sync(fd, fnodes, sizeof(ForwardNode) * req.ttl);
      if (ret) {
        printf("[FWDD]recv nodes error\n");
        return -1;
      }
      for (uint32_t i = 0; i < req.ttl; ++i) {
        printf("[FWDD][node%u]ip %u, port %u\n", i, fnodes[i].ip,
               fnodes[i].port);
      }
    }
    // ForwardFile
    uint32_t f_length;
    ret = recv_sync(fd, &f_length, sizeof(uint32_t));
    if (ret) {
      printf("[FWDD]recv file size error\n");
      return -1;
    }
    ForwardFile *fmeta = (ForwardFile *)malloc(sizeof(ForwardFile) + f_length);
    fmeta->length = f_length;
    ret = recv_sync(fd, fmeta->filename, f_length);
    if (ret) {
      printf("[FWDD]recv file name error\n");
      return -1;
    }
    if (req.ttl > 0) {
      forward_next(fd, &req, fnodes, fmeta);
      free(fnodes);
    } else {
      // data
      int w_fd = open((const char *)fmeta->filename, O_CREAT | O_RDWR, 0644);
      if (w_fd < 0) {
        printf("[FWDD]open error, %d\n", errno);
        return -1;
      }
      ret = forward_sync(fd, w_fd, req.length - sizeof(ForwardRequest) -
                                       sizeof(ForwardNode) * req.ttl -
                                       sizeof(ForwardFile) - f_length);
      if (ret) {
        printf("[FWDD]forward data error\n");
        return -1;
      }
      printf("[FWDD]data write to %s success\n", fmeta->filename);
      close(w_fd);
    }
    free(fmeta);
    close(fd);
  }
  return 0;
}

int main(int argc, char *argv[]) {
  int opt;
  int port = kDefaultPort;
  char *dir = NULL;
  while ((opt = getopt(argc, argv, "p:d:")) != -1) {
    switch (opt) {
      case 'p':
        port = atoi(optarg);
        break;
      case 'd':
        dir = strdup(optarg);
        struct stat s;
        if (stat(dir, &s)) {
          printf("[FWDD]stat error, dir %s, %d\n", dir, errno);
          return -1;
        }
        if (!S_ISDIR(s.st_mode)) {
          printf("[FWDD]input dir is not directory, dir %s, mode %d\n", dir,
                 s.st_mode);
          return -1;
        }
        break;
      default:
        printf("[FWDD]Usage: %s [-p port] [-d dir]\n", argv[0]);
        return -1;
    }
  }
  pid_t pid;
  pid = fork();
  if (pid < 0) {
    printf("[FWDD]fork error, %d\n", errno);
    return -1;
  }
  // parent
  if (pid > 0) {
    // printf("[FWDD]parent exit, child pid: %d\n", pid);
    return 0;
  }
  if (setsid() < 0) {
    printf("[FWDD]setsid error, %d\n", errno);
    return -1;
  }
  signal(SIGCHLD, SIG_IGN);
  signal(SIGHUP, SIG_IGN);
  pid = fork();
  if (pid < 0) {
    printf("[FWDD]fork error, %d\n", errno);
    return -1;
  }
  if (pid > 0) {
    // printf("[FWDD]parent exit, child pid: %d\n", pid);
    return 0;
  }
  umask(0);
  if (dir) {
    chdir(dir);
    free(dir);
  } else {
    chdir("/");
  }
  printf("[FWDD]forward daemon start\n");
  return forward_loop(port);
}