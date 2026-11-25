#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <vector>

#include "logger.h"

namespace {
struct Crc32Table {
  uint32_t entries[256];
  Crc32Table() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
      }
      entries[i] = c;
    }
  }
};

const Crc32Table kCrcTable;
}  // namespace

uint64_t HostToNet64(uint64_t val) {
  uint32_t high = htonl(static_cast<uint32_t>(val >> 32));
  uint32_t low = htonl(static_cast<uint32_t>(val & 0xFFFFFFFFULL));
  uint16_t probe = 1;
  if (*reinterpret_cast<const uint8_t *>(&probe) == 1) {
    return (static_cast<uint64_t>(low) << 32) | high;
  }
  return (static_cast<uint64_t>(high) << 32) | low;
}

uint64_t NetToHost64(uint64_t val) { return HostToNet64(val); }

uint32_t Crc32Update(uint32_t crc, const void *data, size_t len) {
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  uint32_t c = ~crc;
  for (size_t i = 0; i < len; ++i) {
    c = kCrcTable.entries[(c ^ bytes[i]) & 0xFFU] ^ (c >> 8);
  }
  return ~c;
}

int ComputeFileCrc32(int fd, uint64_t len, uint32_t *out_crc) {
  if (lseek(fd, 0, SEEK_SET) < 0) {
    LOG_ERROR("[UTIL]lseek start failed, %d", errno);
    return -1;
  }
  std::vector<char> buf(kForwardBufSize);
  uint32_t crc = 0;
  uint64_t remaining = len;
  while (remaining > 0) {
    size_t chunk = remaining < kForwardBufSize ? static_cast<size_t>(remaining)
                                               : kForwardBufSize;
    ssize_t ret = read(fd, buf.data(), chunk);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      LOG_ERROR("[UTIL]read file for crc32 failed, %d", errno);
      return -1;
    }
    if (ret == 0) {
      LOG_ERROR("[UTIL]unexpected EOF computing crc32 (%llu remaining)",
                static_cast<unsigned long long>(remaining));
      return -1;
    }
    crc = Crc32Update(crc, buf.data(), static_cast<size_t>(ret));
    remaining -= static_cast<uint64_t>(ret);
  }
  if (lseek(fd, 0, SEEK_SET) < 0) {
    LOG_ERROR("[UTIL]lseek rewind failed, %d", errno);
    return -1;
  }
  *out_crc = crc;
  return 0;
}

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

int RelayStream(int fd_src, int fd_dst, int fd_tee, uint64_t len,
                uint32_t *out_crc) {
  std::vector<char> buffer(kForwardBufSize);
  uint32_t crc = 0;
  while (len > 0) {
    size_t read_len =
        len < kForwardBufSize ? static_cast<size_t>(len) : kForwardBufSize;
    ssize_t ret = read(fd_src, buffer.data(), read_len);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      LOG_ERROR("[UTIL]read error, %d", errno);
      return -1;
    }
    if (ret == 0) {
      LOG_ERROR("[UTIL]unexpected EOF while relaying (%llu bytes remaining)",
                static_cast<unsigned long long>(len));
      return -1;
    }
    size_t chunk_bytes = static_cast<size_t>(ret);
    if (out_crc) {
      crc = Crc32Update(crc, buffer.data(), chunk_bytes);
    }
    if (SendSync(fd_dst, buffer.data(), chunk_bytes) != 0) {
      return -1;
    }
    if (fd_tee >= 0 && SendSync(fd_tee, buffer.data(), chunk_bytes) != 0) {
      return -1;
    }
    len -= static_cast<uint64_t>(chunk_bytes);
  }
  if (out_crc) {
    *out_crc = crc;
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

int SendRequestHeader(int fd, const ForwardRequest &host_req) {
  ForwardRequest wire_req;
  memset(&wire_req, 0, sizeof(wire_req));
  wire_req.length = HostToNet64(host_req.length);
  wire_req.magic = htonl(host_req.magic);
  wire_req.version = host_req.version;
  wire_req.cmd = host_req.cmd;
  wire_req.ttl = host_req.ttl;
  wire_req.flags = host_req.flags;
  wire_req.id = HostToNet64(host_req.id);
  wire_req.crc32 = htonl(host_req.crc32);
  return SendSync(fd, &wire_req, sizeof(wire_req));
}

int RecvRequestHeader(int fd, ForwardRequest *host_req) {
  ForwardRequest wire_req;
  if (RecvSync(fd, &wire_req, sizeof(wire_req)) != 0) {
    return -1;
  }
  host_req->length = NetToHost64(wire_req.length);
  host_req->magic = ntohl(wire_req.magic);
  host_req->version = wire_req.version;
  host_req->cmd = wire_req.cmd;
  host_req->ttl = wire_req.ttl;
  host_req->flags = wire_req.flags;
  host_req->id = NetToHost64(wire_req.id);
  host_req->crc32 = ntohl(wire_req.crc32);
  return 0;
}

int SendResponseHeader(int fd, const ForwardResponse &host_res) {
  ForwardResponse wire_res;
  memset(&wire_res, 0, sizeof(wire_res));
  wire_res.length = HostToNet64(host_res.length);
  wire_res.magic = htonl(host_res.magic);
  wire_res.version = host_res.version;
  wire_res.cmd = host_res.cmd;
  wire_res.ttl = host_res.ttl;
  wire_res.retcode = host_res.retcode;
  wire_res.id = HostToNet64(host_res.id);
  wire_res.crc32 = htonl(host_res.crc32);
  return SendSync(fd, &wire_res, sizeof(wire_res));
}

int RecvResponseHeader(int fd, ForwardResponse *host_res) {
  ForwardResponse wire_res;
  if (RecvSync(fd, &wire_res, sizeof(wire_res)) != 0) {
    return -1;
  }
  host_res->length = NetToHost64(wire_res.length);
  host_res->magic = ntohl(wire_res.magic);
  host_res->version = wire_res.version;
  host_res->cmd = wire_res.cmd;
  host_res->ttl = wire_res.ttl;
  host_res->retcode = wire_res.retcode;
  host_res->id = NetToHost64(wire_res.id);
  host_res->crc32 = ntohl(wire_res.crc32);
  return 0;
}

int SendNodes(int fd, const ForwardNode *host_nodes, size_t count) {
  if (count == 0) {
    return 0;
  }
  std::vector<ForwardNode> wire_nodes(count);
  for (size_t i = 0; i < count; ++i) {
    wire_nodes[i].ip = host_nodes[i].ip;
    wire_nodes[i].port = htonl(host_nodes[i].port);
  }
  return SendSync(fd, wire_nodes.data(), count * sizeof(ForwardNode));
}

int RecvNodes(int fd, ForwardNode *host_nodes, size_t count) {
  if (count == 0) {
    return 0;
  }
  if (RecvSync(fd, host_nodes, count * sizeof(ForwardNode)) != 0) {
    return -1;
  }
  for (size_t i = 0; i < count; ++i) {
    host_nodes[i].port = ntohl(host_nodes[i].port);
  }
  return 0;
}

int SendFileMeta(int fd, const char *filename, uint32_t fname_with_nul_len) {
  uint32_t wire_len = htonl(fname_with_nul_len);
  if (SendSync(fd, &wire_len, sizeof(wire_len)) != 0) {
    return -1;
  }
  return SendSync(fd, filename, fname_with_nul_len);
}

int RecvFileMeta(int fd, ForwardFile **out_fmeta) {
  uint32_t wire_len = 0;
  if (RecvSync(fd, &wire_len, sizeof(wire_len)) != 0) {
    return -1;
  }
  uint32_t f_length = ntohl(wire_len);
  if (f_length == 0 || f_length > kMaxFilenameLen) {
    LOG_ERROR("[UTIL]invalid file meta length %u", f_length);
    return -2;
  }
  ForwardFile *fmeta =
      static_cast<ForwardFile *>(malloc(sizeof(ForwardFile) + f_length + 1));
  if (!fmeta) {
    LOG_ERROR("[UTIL]malloc fmeta failed");
    return -3;
  }
  fmeta->length = f_length;
  if (RecvSync(fd, fmeta->filename, f_length) != 0) {
    free(fmeta);
    return -1;
  }
  fmeta->filename[f_length - 1] = '\0';
  *out_fmeta = fmeta;
  return 0;
}

void MakeResponse(ForwardResponse *res, const ForwardRequest *req,
                  uint8_t retcode) {
  memset(res, 0, sizeof(*res));
  res->length = sizeof(ForwardResponse);
  res->magic = kForwardMagic;
  res->version = kForwardVersion;
  res->cmd = req->cmd;
  res->ttl = req->ttl;
  res->retcode = retcode;
  res->id = req->id;
  res->crc32 = 0;
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
