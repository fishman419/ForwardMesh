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

int main(int argc, char *argv[]) {
  int opt;
  char *ip = nullptr;
  int port = kDefaultPort;
  while ((opt = getopt(argc, argv, "i:p:")) != -1) {
    switch (opt) {
      case 'i':
        ip = strdup(optarg);
        break;
      case 'p':
        port = atoi(optarg);
        break;
      default:
        printf("Usage: %s <-i ip> [-p port]\n", argv[0]);
        return -1;
    }
  }
  if (!ip) {
    printf("please input ip\n");
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
  ForwardRequest req;
  char data[16] = "abcde";
  req.length = sizeof(req) + sizeof(data);
  req.magic = kForwardMagic;
  req.version = kForwardVersion1;
  req.cmd = ForwardPush;
  req.ttl = kMaxForwardTTL;
  req.id = 0;
  int len = write(sockfd, (const void *)&req, sizeof(req));
  if (len != sizeof(req)) {
    printf("write error, %d %d\n", len, errno);
    return -1;
  }
  len = write(sockfd, data, sizeof(data));
  if (len != sizeof(data)) {
    printf("write error, %d %d\n", len, errno);
    return -1;
  }
  printf("write success\n");
  close(sockfd);
  return 0;
}