#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include "logger.h"

int SendSync(int fd, void *buffer, int len) {
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

int RecvSync(int fd, void *buffer, int len) {
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

int ForwardSync(int fd_src, int fd_dst, int len) {
  char buffer[16384];
  int ret;
  while (len > 0) {
    ret = read(fd_src, buffer, 16384);
    if (ret < 0) {
      LOG_ERROR("[UTIL]read error, %d", errno);
      return -1;
    }
    if (ret == 0) {
      break;
    }
    int write_len = write(fd_dst, buffer, ret);
    if (write_len < 0) {
      LOG_ERROR("[UTIL]write error, %d", errno);
      return -1;
    }
    len -= write_len;
  }
  return 0;
}