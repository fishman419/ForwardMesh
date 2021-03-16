#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "protocol.h"

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("no dst ip\n");
    return -1;
  }
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    printf("socket error, %d\n", errno);
    return -1;
  }
  struct sockaddr_in dst_addr;
  dst_addr.sin_family = AF_INET;
  dst_addr.sin_port = htons(kDefaultPort);

  if (inet_pton(AF_INET, argv[1], &dst_addr.sin_addr) < 0) {
    printf("address is invalid, %s\n", argv[1]);
    return -1;
  }

  if (connect(sockfd, (struct sockaddr *)&dst_addr, sizeof(dst_addr)) < 0) {
    printf("connect error, %d\n", errno);
    return -1;
  }
  printf("connect success\n");
  return 0;
}