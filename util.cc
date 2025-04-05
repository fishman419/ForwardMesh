#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include "logger.h"

int send_sync(int fd, void *buffer, int len) {
  int ret;
  int offset = 0;
  while (len > 0) {
    ret = write(fd, (char *)buffer + offset, len);
    if (ret < 0) {
      LOG_ERROR("[UTIL]write error, %d", errno);
      return -1;
    }
    offset += ret;
    len -= ret;
  }
  return 0;
}

int recv_sync(int fd, void *buffer, int len) {
  int ret;
  int offset = 0;
  while (len > 0) {
    ret = read(fd, (char *)buffer + offset, len);
    if (ret < 0) {
      LOG_ERROR("[UTIL]read error, %d", errno);
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
      LOG_ERROR("[UTIL]read error, %d", errno);
      return -1;
    }
    write_len = write(fd_dst, buffer, ret);
    if (ret < 0) {
      LOG_ERROR("[UTIL]write error, %d %d", write_len, errno);
      return -1;
    }
    len -= ret;
  }
  return 0;
}