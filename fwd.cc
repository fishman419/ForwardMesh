#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "protocol.h"

int forward_file(int fd, const char *fpath) {
  int r_fd = 0;
  int read_len;
  int write_len;
  int ret = 0;
  struct stat s;
  uint64_t f_size;
  const char *filename;
  ForwardRequest req;
  ForwardFile *fmeta = nullptr;
  uint32_t fmeta_length;
  char data[16384];
  r_fd = open(fpath, 0, O_RDONLY);
  if (r_fd < 0) {
    printf("open file error, %d\n", errno);
    ret = -1;
    goto out;
  }
  if (stat(fpath, &s)) {
    printf("stat error, fpath %s, %d\n", fpath, errno);
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
  req.length = sizeof(req) + fmeta_length + f_size;
  req.magic = kForwardMagic;
  req.version = kForwardVersion1;
  req.cmd = ForwardPush;
  req.ttl = 0;
  req.id = 0;
  fmeta = (ForwardFile *)malloc(fmeta_length);
  fmeta->length = strlen(filename) + 1;
  strncpy((char *)fmeta->filename, filename, strlen(filename));
  write_len = write(fd, (const void *)&req, sizeof(req));
  if (write_len != sizeof(req)) {
    printf("write error, %d %d\n", write_len, errno);
    ret = -1;
    goto out;
  }
  write_len = write(fd, fmeta, fmeta_length);
  if (write_len != fmeta_length) {
    printf("write error, %d %d\n", write_len, errno);
    ret = -1;
    goto out;
  }
  while (f_size > 0) {
    read_len = read(r_fd, data, sizeof(data));
    if (read_len < 0) {
      printf("read error, %d\n", errno);
      ret = -1;
      goto out;
    }
    f_size -= read_len;
    write_len = write(fd, data, read_len);
    if (write_len != read_len) {
      printf("write error, %d %d\n", write_len, errno);
      ret = -1;
      goto out;
    }
  }

  printf("write success\n");
out:
  if (r_fd) {
    close(r_fd);
  }
  if (fmeta) {
    free(fmeta);
  }
  return ret;
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

  if (forward_file(sockfd, fpath)) {
    printf("forward file error\n");
    return -1;
  }
  close(sockfd);
  return 0;
}