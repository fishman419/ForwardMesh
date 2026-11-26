#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
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

bool IsPeerDeniedByEnv(uint32_t ip, uint32_t port) {
  const char *deny_env = getenv("FWDD_DENY_PEERS");
  if (!deny_env || deny_env[0] == '\0') {
    return false;
  }
  ForwardNode target{ip, port};
  std::string target_str = FormatNodeAddr(target);
  std::string list(deny_env);
  size_t pos = 0;
  while (pos < list.size()) {
    size_t comma = list.find(',', pos);
    std::string token = (comma == std::string::npos)
                            ? list.substr(pos)
                            : list.substr(pos, comma - pos);
    if (token == target_str) {
      return true;
    }
    if (comma == std::string::npos) {
      break;
    }
    pos = comma + 1;
  }
  return false;
}

void AppendU32(std::vector<uint8_t> *buf, uint32_t host_val) {
  uint32_t net = htonl(host_val);
  const uint8_t *p = reinterpret_cast<const uint8_t *>(&net);
  buf->insert(buf->end(), p, p + sizeof(net));
}

void AppendRawU32(std::vector<uint8_t> *buf, uint32_t raw_nbo) {
  const uint8_t *p = reinterpret_cast<const uint8_t *>(&raw_nbo);
  buf->insert(buf->end(), p, p + sizeof(raw_nbo));
}

void AppendU16(std::vector<uint8_t> *buf, uint16_t host_val) {
  uint16_t net = htons(host_val);
  const uint8_t *p = reinterpret_cast<const uint8_t *>(&net);
  buf->insert(buf->end(), p, p + sizeof(net));
}
}  // namespace

uint64_t SteadyNowMs() {
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

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

int ConnectWithTimeout(uint32_t ip, uint32_t port, int timeout_ms,
                       uint16_t *out_rtt_ms) {
  if (IsPeerDeniedByEnv(ip, port)) {
    LOG_DEBUG("[UTIL]peer %s denied by FWDD_DENY_PEERS",
              FormatNodeAddr({ip, port}).c_str());
    return -2;
  }

  uint64_t start_ms = SteadyNowMs();
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    LOG_ERROR("[UTIL]socket error, %d", errno);
    return -1;
  }

  int orig_flags = fcntl(fd, F_GETFL, 0);
  if (orig_flags < 0 || fcntl(fd, F_SETFL, orig_flags | O_NONBLOCK) < 0) {
    LOG_ERROR("[UTIL]fcntl O_NONBLOCK error, %d", errno);
    close(fd);
    return -1;
  }

  struct sockaddr_in dst_addr;
  memset(&dst_addr, 0, sizeof(dst_addr));
  dst_addr.sin_family = AF_INET;
  dst_addr.sin_port = htons(static_cast<uint16_t>(port));
  dst_addr.sin_addr.s_addr = ip;

  int rc = connect(fd, reinterpret_cast<struct sockaddr *>(&dst_addr),
                   sizeof(dst_addr));
  if (rc < 0) {
    if (errno != EINPROGRESS) {
      LOG_DEBUG("[UTIL]connect immediate failure to %s, errno=%d",
                FormatNodeAddr({ip, port}).c_str(), errno);
      close(fd);
      return -2;
    }

    uint64_t deadline = start_ms + static_cast<uint64_t>(timeout_ms);
    while (true) {
      uint64_t now = SteadyNowMs();
      if (now >= deadline) {
        LOG_DEBUG("[UTIL]connect timeout to %s",
                  FormatNodeAddr({ip, port}).c_str());
        close(fd);
        return -2;
      }
      struct pollfd pfd;
      pfd.fd = fd;
      pfd.events = POLLOUT;
      pfd.revents = 0;
      int pret = poll(&pfd, 1, static_cast<int>(deadline - now));
      if (pret < 0) {
        if (errno == EINTR) {
          continue;
        }
        close(fd);
        return -2;
      }
      if (pret == 0) {
        close(fd);
        return -2;
      }
      int so_err = 0;
      socklen_t slen = sizeof(so_err);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &slen) < 0 ||
          so_err != 0) {
        LOG_DEBUG("[UTIL]connect SO_ERROR=%d to %s", so_err,
                  FormatNodeAddr({ip, port}).c_str());
        close(fd);
        return -2;
      }
      break;
    }
  }

  if (fcntl(fd, F_SETFL, orig_flags) < 0) {
    close(fd);
    return -1;
  }
  SetSocketTimeout(fd, kDefaultSocketTimeoutSec);

  uint64_t elapsed = SteadyNowMs() - start_ms;
  uint16_t rtt = static_cast<uint16_t>(
      std::max<uint64_t>(1, std::min<uint64_t>(65535, elapsed)));
  if (out_rtt_ms) {
    *out_rtt_ms = rtt;
  }
  return fd;
}

std::string FormatNodeAddr(const ForwardNode &node) {
  char ip_buf[INET_ADDRSTRLEN] = {0};
  if (!inet_ntop(AF_INET, &node.ip, ip_buf, sizeof(ip_buf))) {
    return "0.0.0.0:0";
  }
  return std::string(ip_buf) + ":" + std::to_string(node.port);
}

bool ParseNodeAddr(const std::string &text, ForwardNode *out) {
  if (text.empty() || !out) {
    return false;
  }
  size_t colon = text.find(':');
  std::string ip_part = text;
  uint32_t port = kDefaultPort;
  if (colon != std::string::npos) {
    ip_part = text.substr(0, colon);
    std::string port_part = text.substr(colon + 1);
    if (port_part.empty()) {
      return false;
    }
    char *endptr = NULL;
    long parsed = strtol(port_part.c_str(), &endptr, 10);
    if (*endptr != '\0' || parsed <= 0 || parsed > 65535) {
      return false;
    }
    port = static_cast<uint32_t>(parsed);
  }
  uint32_t ip = 0;
  if (inet_pton(AF_INET, ip_part.c_str(), &ip) != 1) {
    return false;
  }
  out->ip = ip;
  out->port = port;
  return true;
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

void EncodeMeshSyncPayload(const std::vector<MeshSyncEntry> &entries,
                           std::vector<uint8_t> *out_buf) {
  out_buf->clear();
  uint32_t count =
      static_cast<uint32_t>(std::min<size_t>(entries.size(), kMaxMeshRoutes));
  AppendU32(out_buf, count);
  for (uint32_t i = 0; i < count; ++i) {
    const MeshSyncEntry &e = entries[i];
    uint8_t plen =
        static_cast<uint8_t>(std::min<size_t>(e.path.size(), kMaxForwardTTL));
    AppendRawU32(out_buf, e.target.ip);
    AppendU32(out_buf, e.target.port);
    AppendU16(out_buf, e.rtt_ms);
    out_buf->push_back(plen);
    out_buf->push_back(e.alive ? 1 : 0);
    for (uint8_t j = 0; j < plen; ++j) {
      AppendRawU32(out_buf, e.path[j].ip);
      AppendU32(out_buf, e.path[j].port);
    }
  }
}

int DecodeMeshSyncPayload(const uint8_t *buf, size_t len,
                          std::vector<MeshSyncEntry> *out_entries) {
  if (!buf || !out_entries || len < sizeof(uint32_t)) {
    return -1;
  }
  out_entries->clear();
  uint32_t wire_count = 0;
  memcpy(&wire_count, buf, sizeof(uint32_t));
  uint32_t count = ntohl(wire_count);
  if (count > kMaxMeshRoutes) {
    return -1;
  }

  size_t off = sizeof(uint32_t);
  out_entries->reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (len - off < 12) {
      return -1;
    }
    MeshSyncEntry e;
    uint32_t wire_port = 0;
    uint16_t wire_rtt = 0;
    memcpy(&e.target.ip, buf + off, 4);
    memcpy(&wire_port, buf + off + 4, 4);
    memcpy(&wire_rtt, buf + off + 8, 2);
    uint8_t plen = buf[off + 10];
    e.alive = buf[off + 11];
    off += 12;

    e.target.port = ntohl(wire_port);
    e.rtt_ms = ntohs(wire_rtt);
    if (e.target.port == 0 || e.target.port > 65535 || plen == 0 ||
        plen > kMaxForwardTTL) {
      return -1;
    }
    size_t path_bytes = static_cast<size_t>(plen) * sizeof(ForwardNode);
    if (len - off < path_bytes) {
      return -1;
    }
    e.path.resize(plen);
    for (uint8_t j = 0; j < plen; ++j) {
      uint32_t hop_port = 0;
      memcpy(&e.path[j].ip, buf + off, 4);
      memcpy(&hop_port, buf + off + 4, 4);
      e.path[j].port = ntohl(hop_port);
      if (e.path[j].port == 0 || e.path[j].port > 65535) {
        return -1;
      }
      off += sizeof(ForwardNode);
    }
    out_entries->push_back(e);
  }

  return (off == len) ? 0 : -1;
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
