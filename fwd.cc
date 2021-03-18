#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "protocol.h"

int forward_file(int fd, const char *filename) {
  ForwardRequest req;
  char data[16] = "abcde";
  uint32_t ff_length = sizeof(ForwardFile) + strlen(filename) + 1;
  req.length = sizeof(req) + ff_length + sizeof(data);
  req.magic = kForwardMagic;
  req.version = kForwardVersion1;
  req.cmd = ForwardPush;
  req.ttl = 0;
  req.id = 0;
  ForwardFile *fmeta = (ForwardFile *)malloc(ff_length);
  fmeta->length = strlen(filename) + 1;
  strncpy((char *)fmeta->filename, filename, strlen(filename));
  int len = write(fd, (const void *)&req, sizeof(req));
  if (len != sizeof(req)) {
    printf("write error, %d %d\n", len, errno);
    free(fmeta);
    return -1;
  }
  len = write(fd, fmeta, ff_length);
  if (len != ff_length) {
    printf("write error, %d %d\n", len, errno);
    free(fmeta);
    return -1;
  }
  len = write(fd, data, sizeof(data));
  if (len != sizeof(data)) {
    printf("write error, %d %d\n", len, errno);
    free(fmeta);
    return -1;
  }
  free(fmeta);
  printf("write success\n");
  return 0;
}

int main(int argc, char *argv[]) {
  int opt;
  char *ip = nullptr;
  char *fpath = nullptr;
  int port = kDefaultPort;
  while ((opt = getopt(argc, argv, "i:p:f:")) != -1) {
    switch (opt) {
      case 'i':
        ip = strdup(optarg);
        break;
      case 'p':
        port = atoi(optarg);
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
        printf("Usage: %s <-i ip> <-f file> [-p port]\n", argv[0]);
        return -1;
    }
  }
  if (!ip) {
    printf("please input ip\n");
    return -1;
  }
  if (!fpath) {
    printf("please input file\n");
    return -1;
  }
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    printf("socket error, %d\n", errno);
    return -1;
  }
  struct sockaddr_in dst_addr;
  dst_addr.sin_family = AF_INET;
  dst_addr.sin_port = htons(port);

  if (inet_pton(AF_INET, ip, &dst_addr.sin_addr) < 0) {
    printf("address is invalid, %s\n", ip);
    return -1;
  }
  free(ip);

  if (connect(sockfd, (struct sockaddr *)&dst_addr, sizeof(dst_addr)) < 0) {
    printf("connect error, %d\n", errno);
    return -1;
  }

  char *filename = strrchr(fpath, '/');
  if (!filename) {
    filename = fpath;
  } else {
    filename += 1;
  }
  if (forward_file(sockfd, filename)) {
    printf("forward file error\n");
    return -1;
  }
  close(sockfd);
  return 0;
}