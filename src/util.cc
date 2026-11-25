#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "logger.h"

int SendSync(int fd, const void *buffer, size_t len) {
  size_t offset = 0;
  while (len > 0) {
    ssize_t ret = write(fd, static_cast<const char *>(buffer) + offset, len);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      LOG_ERROR("[UTIL]write error, %d", errno);
      return -1;
    }
    if (ret == 0) {
      LOG_ERROR("[UTIL]write returned 0 with %zu bytes remaining", len);
      return -1;
    }
    offset += static_cast<size_t>(ret);
    len -= static_cast<size_t>(ret);
  }
  return 0;
}

int RecvSync(int fd, void *buffer, size_t len) {
  size_t offset = 0;
  while (len > 0) {
    ssize_t ret = read(fd, static_cast<char *>(buffer) + offset, len);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      LOG_ERROR("[UTIL]read error, %d", errno);
      return -1;
    }
    if (ret == 0) {
      LOG_ERROR("[UTIL]peer closed connection (%zu bytes remaining)", len);
      return -1;
    }
    offset += static_cast<size_t>(ret);
    len -= static_cast<size_t>(ret);
  }
  return 0;
}

int ForwardSync(int fd_src, int fd_dst, size_t len) {
  char buffer[kForwardBufSize];
  while (len > 0) {
    size_t read_len = len < kForwardBufSize ? len : kForwardBufSize;
    ssize_t ret = read(fd_src, buffer, read_len);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      LOG_ERROR("[UTIL]read error, %d", errno);
      return -1;
    }
    if (ret == 0) {
      LOG_ERROR("[UTIL]unexpected EOF while forwarding (%zu bytes remaining)",
                len);
      return -1;
    }
    if (SendSync(fd_dst, buffer, static_cast<size_t>(ret)) != 0) {
      return -1;
    }
    len -= static_cast<size_t>(ret);
  }
  return 0;
}

int SetSocketTimeout(int fd, int timeout_sec) {
  struct timeval tv;
  tv.tv_sec = timeout_sec;
  tv.tv_usec = 0;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
    LOG_WARNING("[UTIL]setsockopt SO_RCVTIMEO failed, %d", errno);
    return -1;
  }
  if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
    LOG_WARNING("[UTIL]setsockopt SO_SNDTIMEO failed, %d", errno);
    return -1;
  }
  return 0;
}

void MakeResponse(ForwardResponse *res, const ForwardRequest *req,
                  uint8_t retcode) {
  memset(res, 0, sizeof(*res));
  res->length = sizeof(ForwardResponse);
  res->magic = kForwardMagic;
  res->version = kForwardVersion1;
  res->cmd = req->cmd;
  res->ttl = req->ttl;
  res->retcode = retcode;
  res->id = req->id;
}

bool IsSafeRelativePath(const char *path) {
  if (!path || path[0] == '\0' || path[0] == '/') {
    return false;
  }
  const char *p = path;
  while (*p != '\0') {
    if (*p == '\\') {
      return false;
    }
    const char *slash = strchr(p, '/');
    size_t seg_len = slash ? static_cast<size_t>(slash - p) : strlen(p);
    if (seg_len == 0 || (seg_len == 1 && p[0] == '.') ||
        (seg_len == 2 && p[0] == '.' && p[1] == '.')) {
      return false;
    }
    if (!slash) {
      break;
    }
    p = slash + 1;
    if (*p == '\0') {
      // Trailing slash (directory path rather than file)
      return false;
    }
  }
  return true;
}

std::string ExtractBasename(const char *path) {
  if (!path) {
    return "";
  }
  const char *slash = strrchr(path, '/');
  const char *base = slash ? slash + 1 : path;
  if (base[0] == '\0' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
    return "";
  }
  return std::string(base);
}
