#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "protocol.h"

int forward_loop() {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    printf("socket error, %d\n", errno);
    return -1;
  }
  int optval = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &optval,
                 sizeof(optval))) {
    printf("setsockopt error, %d\n", errno);
    return -1;
  }
  struct sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(kDefaultPort);
  if (bind(sockfd, (struct sockaddr *)&address, sizeof(address))) {
    printf("bind error, %d\n", errno);
    return -1;
  }
  if (listen(sockfd, 5)) {
    printf("listener error, %d\n", errno);
    return -1;
  }
  while (1) {
    struct sockaddr_in src_addr;
    socklen_t addr_len;
    char ip_str[64];
    int fd = accept(sockfd, (struct sockaddr *)&src_addr, &addr_len);
    if (fd < 0) {
      printf("accept error, %d\n", errno);
      return -1;
    }
    inet_ntop(AF_INET, &src_addr.sin_addr, ip_str, sizeof(ip_str));
    printf("accept %d, ip: %s, port: %d\n", fd, ip_str,
           ntohs(src_addr.sin_port));
    char buffer[4096];
    int len = read(fd, buffer, sizeof(uint32_t));
    if (len != sizeof(uint32_t)) {
      printf("read error, %d %d\n", len, errno);
      return -1;
    }
    uint32_t data_len = *(uint32_t *)buffer;
    uint32_t left_len = data_len - sizeof(uint32_t);
    len = read(fd, buffer + sizeof(uint32_t) / sizeof(char), left_len);
    if (len != sizeof(uint32_t)) {
      printf("read error, %d %d\n", len, errno);
      return -1;
    }
    ForwardRequest *req = (ForwardRequest *)buffer;
    printf("length %d, magic %d, version %d, cmd %d, ttl %d, id %d\n",
           req->length, req->magic, req->version, req->cmd, req->ttl, req->id);
    printf("data %s\n", req->data);
    close(fd);
  }
  return 0;
}

int main(int argc, char *argv[]) {
  pid_t pid;
  pid = fork();
  if (pid < 0) {
    printf("fork error, %d\n", errno);
    return -1;
  }
  // parent
  if (pid > 0) {
    printf("parent exit, child pid: %d\n", pid);
    return 0;
  }
  if (setsid() < 0) {
    printf("setsid error, %d\n", errno);
    return -1;
  }
  signal(SIGCHLD, SIG_IGN);
  signal(SIGHUP, SIG_IGN);
  pid = fork();
  if (pid < 0) {
    printf("fork error, %d\n", errno);
    return -1;
  }
  if (pid > 0) {
    printf("parent exit, child pid: %d\n", pid);
    return 0;
  }
  umask(0);
  chdir("/");
  printf("deamon start\n");
  return forward_loop();
}