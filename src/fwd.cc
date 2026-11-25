#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <utility>
#include <vector>

#include "logger.h"
#include "protocol.h"
#include "util.h"

typedef std::vector<std::pair<uint32_t, uint32_t>> ForwardAddress;

namespace {
uint64_t GenerateRequestId() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  uint64_t ts = static_cast<uint64_t>(tv.tv_sec) * 1000000ULL +
                static_cast<uint64_t>(tv.tv_usec);
  return (ts << 16) ^ static_cast<uint64_t>(getpid());
}

int SendFile(int fd, const char *fpath, const ForwardAddress &address,
             uint8_t flags) {
  int r_fd = -1;
  int ret = 0;
  struct stat s;
  uint64_t f_size = 0;
  uint32_t file_crc32 = 0;
  std::string base_name;
  ForwardRequest req;
  ForwardResponse res;
  std::vector<ForwardNode> fnodes;
  uint32_t fname_with_nul = 0;
  uint64_t fmeta_length = 0;
  uint32_t ttl = static_cast<uint32_t>(address.size() - 1);

  r_fd = open(fpath, O_RDONLY);
  if (r_fd < 0) {
    LOG_ERROR("open file error, %d", errno);
    return -1;
  }
  if (fstat(r_fd, &s) != 0) {
    LOG_ERROR("stat error, fpath %s, %d", fpath, errno);
    close(r_fd);
    return -1;
  }
  f_size = static_cast<uint64_t>(s.st_size);
  base_name = ExtractBasename(fpath);
  if (base_name.empty() || base_name.size() + 1 > kMaxFilenameLen) {
    LOG_ERROR("invalid filename: %s", fpath);
    close(r_fd);
    return -1;
  }

  if (ComputeFileCrc32(r_fd, f_size, &file_crc32) != 0) {
    close(r_fd);
    return -1;
  }

  fname_with_nul = static_cast<uint32_t>(base_name.size() + 1);
  fmeta_length = sizeof(ForwardFile) + fname_with_nul;

  memset(&req, 0, sizeof(req));
  req.length = sizeof(req) + static_cast<uint64_t>(ttl) * sizeof(ForwardNode) +
               fmeta_length + f_size;
  req.magic = kForwardMagic;
  req.version = kForwardVersion;
  req.cmd = ForwardPush;
  req.ttl = static_cast<uint8_t>(ttl);
  req.flags = flags;
  req.id = GenerateRequestId();
  req.crc32 = file_crc32;

  if (ttl > 0) {
    fnodes.resize(ttl);
    for (size_t i = 1; i < address.size(); ++i) {
      fnodes[i - 1].ip = address[i].first;
      fnodes[i - 1].port = address[i].second;
      LOG_DEBUG("hop[%zu] ip=%u port=%u", i - 1, fnodes[i - 1].ip,
                fnodes[i - 1].port);
    }
  }

  if (SendRequestHeader(fd, req) != 0 ||
      SendNodes(fd, fnodes.data(), fnodes.size()) != 0 ||
      SendFileMeta(fd, base_name.c_str(), fname_with_nul) != 0) {
    LOG_ERROR("send push header/meta error");
    close(r_fd);
    return -1;
  }

  ret = RelayStream(r_fd, fd, -1, f_size, NULL);
  close(r_fd);
  if (ret != 0) {
    LOG_ERROR("send file data error");
    return -1;
  }

  if (RecvResponseHeader(fd, &res) != 0) {
    LOG_ERROR("recv response error");
    return -1;
  }
  if (res.magic != kForwardMagic || res.version != kForwardVersion ||
      res.retcode != ForwardSuccess) {
    LOG_ERROR("forward failed, magic=0x%x ver=%u retcode=%u", res.magic,
              res.version, res.retcode);
    return -1;
  }

  LOG_INFO("push file(%s) success (%llu bytes, crc32=0x%08x, id=0x%llx)", fpath,
           static_cast<unsigned long long>(f_size), file_crc32,
           static_cast<unsigned long long>(req.id));
  return 0;
}

int PullFile(int fd, const char *remote_path, const char *local_path,
             const ForwardAddress &address, uint8_t flags) {
  size_t remote_fname_len = strlen(remote_path);
  if (!IsSafeRelativePath(remote_path) ||
      remote_fname_len + 1 > kMaxFilenameLen) {
    LOG_ERROR("invalid remote relative path: %s", remote_path);
    return -1;
  }

  uint32_t ttl = static_cast<uint32_t>(address.size() - 1);
  uint32_t fname_with_nul = static_cast<uint32_t>(remote_fname_len + 1);
  uint64_t fmeta_length = sizeof(ForwardFile) + fname_with_nul;

  ForwardRequest req;
  memset(&req, 0, sizeof(req));
  req.length = sizeof(req) + static_cast<uint64_t>(ttl) * sizeof(ForwardNode) +
               fmeta_length;
  req.magic = kForwardMagic;
  req.version = kForwardVersion;
  req.cmd = ForwardPull;
  req.ttl = static_cast<uint8_t>(ttl);
  req.flags = flags;
  req.id = GenerateRequestId();
  req.crc32 = 0;

  std::vector<ForwardNode> fnodes;
  if (ttl > 0) {
    fnodes.resize(ttl);
    for (size_t i = 1; i < address.size(); ++i) {
      fnodes[i - 1].ip = address[i].first;
      fnodes[i - 1].port = address[i].second;
      LOG_DEBUG("hop[%zu] ip=%u port=%u", i - 1, fnodes[i - 1].ip,
                fnodes[i - 1].port);
    }
  }

  if (SendRequestHeader(fd, req) != 0 ||
      SendNodes(fd, fnodes.data(), fnodes.size()) != 0 ||
      SendFileMeta(fd, remote_path, fname_with_nul) != 0) {
    LOG_ERROR("send pull request error");
    return -1;
  }

  ForwardResponse res;
  if (RecvResponseHeader(fd, &res) != 0) {
    LOG_ERROR("recv pull response error");
    return -1;
  }

  if (res.magic != kForwardMagic || res.version != kForwardVersion ||
      res.retcode != ForwardSuccess) {
    LOG_ERROR("pull rejected by server, magic=0x%x ver=%u retcode=%u",
              res.magic, res.version, res.retcode);
    return -1;
  }
  if (res.cmd != ForwardPush) {
    LOG_ERROR("unexpected response cmd %u", res.cmd);
    return -1;
  }

  ForwardFile *recv_fmeta = NULL;
  if (RecvFileMeta(fd, &recv_fmeta) != 0) {
    LOG_ERROR("recv pull file meta error");
    return -1;
  }

  uint64_t total_fmeta_len = sizeof(ForwardFile) + recv_fmeta->length;
  free(recv_fmeta);
  if (res.length < sizeof(ForwardResponse) + total_fmeta_len) {
    LOG_ERROR("invalid pull response packet length %llu",
              static_cast<unsigned long long>(res.length));
    return -1;
  }

  uint64_t file_data_len =
      res.length - sizeof(ForwardResponse) - total_fmeta_len;
  uint32_t expected_crc32 = res.crc32;

  std::string tmp_tmpl = std::string(local_path) + ".part.XXXXXX";
  std::vector<char> tmp_buf(tmp_tmpl.begin(), tmp_tmpl.end());
  tmp_buf.push_back('\0');
  int tmp_fd = mkstemp(tmp_buf.data());
  if (tmp_fd < 0) {
    LOG_ERROR("mkstemp(%s) error, %d", tmp_buf.data(), errno);
    return -1;
  }
  fchmod(tmp_fd, kDefaultFileMode);

  uint32_t actual_crc32 = 0;
  int stream_rc = RelayStream(fd, tmp_fd, -1, file_data_len, &actual_crc32);
  close(tmp_fd);
  if (stream_rc != 0) {
    LOG_ERROR("receive file data error");
    unlink(tmp_buf.data());
    return -1;
  }

  ForwardResponse final_res;
  if (RecvResponseHeader(fd, &final_res) != 0) {
    LOG_ERROR("recv final response error");
    unlink(tmp_buf.data());
    return -1;
  }

  if (actual_crc32 != expected_crc32) {
    LOG_ERROR("pull crc32 mismatch: expected 0x%08x, actual 0x%08x",
              expected_crc32, actual_crc32);
    unlink(tmp_buf.data());
    return -1;
  }

  if (final_res.magic != kForwardMagic ||
      final_res.version != kForwardVersion ||
      final_res.retcode != ForwardSuccess) {
    LOG_ERROR("pull failed at final ack, retcode %u", final_res.retcode);
    unlink(tmp_buf.data());
    return -1;
  }

  if (rename(tmp_buf.data(), local_path) != 0) {
    LOG_ERROR("rename(%s -> %s) error, %d", tmp_buf.data(), local_path, errno);
    unlink(tmp_buf.data());
    return -1;
  }

  LOG_INFO("pull file(%s) to %s success (%llu bytes, crc32=0x%08x, id=0x%llx)",
           remote_path, local_path,
           static_cast<unsigned long long>(file_data_len), actual_crc32,
           static_cast<unsigned long long>(req.id));
  return 0;
}

int ResolveAddress(const char *raw_str, ForwardAddress *res) {
  std::string cxx_str(raw_str);
  if (cxx_str.empty()) {
    return -1;
  }
  size_t pos = 0;
  while (pos < cxx_str.length()) {
    size_t next = cxx_str.find(',', pos);
    std::string addr;
    if (next == std::string::npos) {
      addr = cxx_str.substr(pos);
      pos = cxx_str.length();
    } else {
      if (next == pos || next + 1 == cxx_str.length()) {
        LOG_ERROR("empty hop in address chain: %s", raw_str);
        return -1;
      }
      addr = cxx_str.substr(pos, next - pos);
      pos = next + 1;
    }

    int port = static_cast<int>(kDefaultPort);
    size_t port_pos = addr.find(':');
    std::string ip_part = addr;
    if (port_pos != std::string::npos) {
      ip_part = addr.substr(0, port_pos);
      std::string port_part = addr.substr(port_pos + 1);
      char *endptr = NULL;
      long parsed_port = strtol(port_part.c_str(), &endptr, 10);
      if (port_part.empty() || *endptr != '\0' || parsed_port <= 0 ||
          parsed_port > 65535) {
        LOG_ERROR("port is invalid in address: %s", addr.c_str());
        return -1;
      }
      port = static_cast<int>(parsed_port);
    }

    uint32_t ip = 0;
    if (inet_pton(AF_INET, ip_part.c_str(), &ip) != 1) {
      LOG_ERROR("ip address is invalid: %s", ip_part.c_str());
      return -1;
    }
    res->emplace_back(ip, static_cast<uint32_t>(port));
  }

  if (res->empty() || res->size() - 1 > kMaxForwardTTL) {
    LOG_ERROR("address chain length %zu invalid (max hops %u)", res->size(),
              kMaxForwardTTL);
    return -1;
  }
  return 0;
}

void PrintUsage(const char *prog) {
  printf("Usage: %s -a ip:port,... [options]\n", prog);
  printf("  -a addr : comma-separated list of ip:port forwarding chain\n");
  printf("  -f file : file to send (push) or local output path (pull)\n");
  printf("  -g file : remote relative file path to get (pull mode)\n");
  printf(
      "  -s      : cache file copies on intermediate hops (default: zero-disk "
      "streaming)\n");
  printf("  -h      : show this help\n");
  printf("Examples:\n");
  printf("  Push (stream): %s -a 127.0.0.1:40000,127.0.0.1:40001 -f test.txt\n",
         prog);
  printf(
      "  Push (cache) : %s -s -a 127.0.0.1:40000,127.0.0.1:40001 -f test.txt\n",
      prog);
  printf("  Pull         : %s -a 127.0.0.1:40000 -g remote.txt -f local.txt\n",
         prog);
}
}  // namespace

int main(int argc, char *argv[]) {
  signal(SIGPIPE, SIG_IGN);

  if (LogInit("forward_client.log") != 0) {
    return 1;
  }
  atexit(LogClose);

  int opt;
  ForwardAddress forward_address;
  std::string fpath;
  std::string remote_path;
  bool pull_mode = false;
  uint8_t flags = kFlagNone;

  LOG_INFO("Forward client started");
  while ((opt = getopt(argc, argv, "a:f:g:sh")) != -1) {
    switch (opt) {
      case 'a':
        forward_address.clear();
        if (ResolveAddress(optarg, &forward_address) != 0) {
          LOG_ERROR("invalid address format: %s", optarg);
          return 1;
        }
        break;
      case 'f':
        fpath = optarg;
        break;
      case 'g':
        pull_mode = true;
        remote_path = optarg;
        break;
      case 's':
        flags |= kFlagStoreOnHop;
        break;
      case 'h':
        PrintUsage(argv[0]);
        return 0;
      default:
        PrintUsage(argv[0]);
        return 1;
    }
  }

  if (forward_address.empty()) {
    LOG_ERROR("please specify address with -a");
    return 1;
  }

  if (!pull_mode && fpath.empty()) {
    LOG_ERROR("please specify file with -f (push) or -g (pull)");
    return 1;
  }

  if (!pull_mode) {
    struct stat s;
    if (stat(fpath.c_str(), &s) != 0) {
      LOG_ERROR("stat error, fpath %s, %d", fpath.c_str(), errno);
      return 1;
    }
    if (!S_ISREG(s.st_mode)) {
      LOG_ERROR("input file is not regular file, fpath %s, mode %d",
                fpath.c_str(), s.st_mode);
      return 1;
    }
  }

  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    LOG_ERROR("socket error, %d", errno);
    return 1;
  }
  SetSocketTimeout(sockfd, kDefaultSocketTimeoutSec);

  struct sockaddr_in dst_addr;
  memset(&dst_addr, 0, sizeof(dst_addr));
  dst_addr.sin_family = AF_INET;
  dst_addr.sin_port = htons(static_cast<uint16_t>(forward_address[0].second));
  dst_addr.sin_addr.s_addr = forward_address[0].first;

  if (connect(sockfd, reinterpret_cast<struct sockaddr *>(&dst_addr),
              sizeof(dst_addr)) < 0) {
    LOG_ERROR("connect error, %d", errno);
    close(sockfd);
    return 1;
  }

  int rc = 0;
  if (pull_mode) {
    const char *out_path = fpath.empty() ? "./downloaded_file" : fpath.c_str();
    if (PullFile(sockfd, remote_path.c_str(), out_path, forward_address,
                 flags) != 0) {
      LOG_ERROR("pull file error");
      rc = 1;
    }
  } else {
    if (SendFile(sockfd, fpath.c_str(), forward_address, flags) != 0) {
      LOG_ERROR("forward file error");
      rc = 1;
    }
  }

  close(sockfd);
  return rc;
}
