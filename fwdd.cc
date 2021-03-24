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

int forward_loop(int port) {
  char buffer[16384];
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
    int fd = accept(sockfd, (struct sockaddr *)&src_addr, &addr_len);
    if (fd < 0) {
      printf("[FWDD]accept error, %d\n", errno);
      return -1;
    }
    inet_ntop(AF_INET, &src_addr.sin_addr, ip_str, sizeof(ip_str));
    printf("[FWDD]accept %d, ip: %s, port: %d\n", fd, ip_str,
           ntohs(src_addr.sin_port));
    uint32_t offset = 0;
    int len = read(fd, buffer, sizeof(uint32_t));
    if (len != sizeof(uint32_t)) {
      printf("[FWDD]read error, %d %d\n", len, errno);
      return -1;
    }
    offset += sizeof(uint32_t) / sizeof(char);
    uint32_t data_len = *(uint32_t *)buffer;
    uint32_t left_len = sizeof(ForwardRequest) - sizeof(uint32_t);
    ForwardRequest req;
    // ForwardRequest
    while (left_len != 0) {
      len = read(fd, buffer + offset, left_len);
      if (len < 0) {
        printf("[FWDD]read error, %d\n", errno);
        return -1;
      }
      offset += len;
      left_len -= len;
    }
    memcpy(&req, buffer, sizeof(ForwardRequest));
    printf(
        "[FWDD][header]length %d, magic %d, version %d, cmd %d, ttl %d, id "
        "%d\n",
        req.length, req.magic, req.version, req.cmd, req.ttl, req.id);
    // ForwardNode
    if (req.ttl) {
      left_len = sizeof(ForwardNode) * req.ttl;
      offset = 0;
      ForwardNode *fnodes = (ForwardNode *)malloc(left_len);
      while (left_len != 0) {
        len = read(fd, (uint8_t *)fnodes + offset, left_len);
        if (len < 0) {
          printf("[FWDD]read error, %d\n", errno);
          return -1;
        }
        offset += len;
        left_len -= len;
      }
      for (uint32_t i = 0; i < req.ttl; ++i) {
        printf("[FWDD][node%u]ip %u, port %u\n", i, fnodes[i].ip,
               fnodes[i].port);
      }
      free(fnodes);
    }
    // ForwardFile
    len = read(fd, buffer, sizeof(uint32_t));
    if (len != sizeof(uint32_t)) {
      printf("[FWDD]read error, %d %d\n", len, errno);
      return -1;
    }
    uint32_t f_length = *(uint32_t *)buffer;
    ForwardFile *fmeta = (ForwardFile *)malloc(sizeof(ForwardFile) + f_length);
    fmeta->length = f_length;
    left_len = f_length;
    offset = 0;
    while (left_len != 0) {
      len = read(fd, fmeta->filename + offset, left_len);
      if (len < 0) {
        printf("[FWDD]read error, %d\n", errno);
        return -1;
      }
      offset += len;
      left_len -= len;
    }
    printf("[FWDD][filemeta]length: %d, filename: %s\n", fmeta->length,
           fmeta->filename);
    // data
    int w_fd = open((const char *)fmeta->filename, O_CREAT | O_RDWR, 0644);
    if (w_fd < 0) {
      printf("[FWDD]open error, %d\n", errno);
      return -1;
    }
    left_len = data_len - sizeof(ForwardRequest) -
               sizeof(ForwardNode) * req.ttl - sizeof(ForwardFile) - f_length;
    printf("[FWDD]data length: %d\n", left_len);
    while (left_len > 0) {
      len = read(fd, buffer, 16384);
      if (len < 0) {
        printf("[FWDD]read error, %d\n", errno);
        return -1;
      }
      printf("[FWDD]read length %d\n", len);
      left_len -= len;
      int write_len = write(w_fd, buffer, len);
      if (write_len != len) {
        printf("[FWDD]write error, %d %d\n", write_len, errno);
        return -1;
      }
      printf("[FWDD]write length %d\n", write_len);
    }
    free(fmeta);
    close(w_fd);
    printf("[FWDD]data write success\n");
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
    printf("[FWDD]parent exit, child pid: %d\n", pid);
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
    printf("[FWDD]parent exit, child pid: %d\n", pid);
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