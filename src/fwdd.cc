#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "logger.h"
#include "protocol.h"
#include "util.h"

namespace {
std::queue<int> g_task_queue;
std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
bool g_shutdown = false;
bool g_always_store_on_hop = false;
volatile sig_atomic_t g_stop_requested = 0;

void SignalHandler(int /*signo*/) { g_stop_requested = 1; }

int ConnectNextNode(const ForwardNode &node) {
  int next_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (next_fd < 0) {
    LOG_ERROR("socket error, %d", errno);
    return -1;
  }
  SetSocketTimeout(next_fd, kDefaultSocketTimeoutSec);

  struct sockaddr_in dst_addr;
  memset(&dst_addr, 0, sizeof(dst_addr));
  dst_addr.sin_family = AF_INET;
  dst_addr.sin_port = htons(static_cast<uint16_t>(node.port));
  dst_addr.sin_addr.s_addr = node.ip;

  if (connect(next_fd, reinterpret_cast<struct sockaddr *>(&dst_addr),
              sizeof(dst_addr)) < 0) {
    LOG_ERROR("connect error, %d", errno);
    close(next_fd);
    return -2;
  }
  return next_fd;
}

// Relays data_len bytes from src_fd to dst_fd in cut-through streaming fashion.
// When store_copy is true, tees the stream into a temporary file in a single
// pass and atomically renames it onto the local basename only if CRC32 matches.
int StreamOrTeeForward(int src_fd, int dst_fd, const ForwardFile *fmeta,
                       uint64_t data_len, uint32_t expected_crc32,
                       bool store_copy, uint8_t *out_err) {
  *out_err = ForwardSuccess;
  int tmp_fd = -1;
  char tmp_path[] = ".fwd_tee_XXXXXX";
  std::string local_name;

  if (store_copy) {
    local_name =
        ExtractBasename(reinterpret_cast<const char *>(fmeta->filename));
    if (local_name.empty()) {
      LOG_ERROR("invalid filename for intermediate hop cache");
      *out_err = ForwardInvalidRequest;
      return -1;
    }
    tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
      LOG_ERROR("mkstemp error, %d", errno);
      *out_err = ForwardInternalError;
      return -1;
    }
    fchmod(tmp_fd, kDefaultFileMode);
  }

  uint32_t actual_crc32 = 0;
  int relay_rc = RelayStream(src_fd, dst_fd, tmp_fd, data_len, &actual_crc32);
  if (tmp_fd >= 0) {
    close(tmp_fd);
  }

  if (relay_rc != 0) {
    if (store_copy) {
      unlink(tmp_path);
    }
    *out_err = ForwardInterrupt;
    return -1;
  }

  if (actual_crc32 != expected_crc32) {
    LOG_ERROR("hop crc32 mismatch: expected 0x%08x, actual 0x%08x",
              expected_crc32, actual_crc32);
    if (store_copy) {
      unlink(tmp_path);
    }
    *out_err = ForwardChecksumMismatch;
    return -1;
  }

  if (store_copy) {
    if (rename(tmp_path, local_name.c_str()) != 0) {
      LOG_ERROR("rename temp to %s error, %d", local_name.c_str(), errno);
      unlink(tmp_path);
      *out_err = ForwardInternalError;
      return -1;
    }
    LOG_INFO("tee-cached and streamed %llu bytes (%s, crc32=0x%08x)",
             static_cast<unsigned long long>(data_len), local_name.c_str(),
             actual_crc32);
  } else {
    LOG_INFO("cut-through streamed %llu bytes (crc32=0x%08x)",
             static_cast<unsigned long long>(data_len), actual_crc32);
  }
  return 0;
}

int ForwardNext(int fd, const ForwardRequest *req, const ForwardNode *nodes,
                const ForwardFile *fmeta) {
  ForwardResponse res;
  uint8_t retcode = ForwardSuccess;
  int ret = 0;

  uint64_t header_overhead =
      sizeof(ForwardRequest) +
      static_cast<uint64_t>(req->ttl) * sizeof(ForwardNode) +
      sizeof(ForwardFile) + fmeta->length;
  if (req->length < header_overhead) {
    LOG_ERROR("invalid push packet length %llu < %llu",
              static_cast<unsigned long long>(req->length),
              static_cast<unsigned long long>(header_overhead));
    MakeResponse(&res, req, ForwardInvalidRequest);
    SendResponseHeader(fd, res);
    return -1;
  }
  uint64_t data_len = req->length - header_overhead;

  int next_fd = ConnectNextNode(nodes[0]);
  if (next_fd < 0) {
    retcode = (next_fd == -2) ? ForwardUnreachable : ForwardInternalError;
    ret = -1;
    goto out_response;
  }

  {
    ForwardRequest next_req = *req;
    next_req.length -= sizeof(ForwardNode);
    next_req.ttl -= 1;

    if (SendRequestHeader(next_fd, next_req) != 0 ||
        SendNodes(next_fd, nodes + 1, next_req.ttl) != 0 ||
        SendFileMeta(next_fd, reinterpret_cast<const char *>(fmeta->filename),
                     fmeta->length) != 0) {
      LOG_ERROR("send push header/meta to next hop error");
      retcode = ForwardInterrupt;
      ret = -1;
      goto out_response;
    }
  }

  {
    bool store_copy =
        ((req->flags & kFlagStoreOnHop) != 0) || g_always_store_on_hop;
    uint8_t stream_err = ForwardSuccess;
    int sret = StreamOrTeeForward(fd, next_fd, fmeta, data_len, req->crc32,
                                  store_copy, &stream_err);

    // If relay completed on the wire (even with CRC mismatch), read downstream
    // response; if wire relay broke mid-stream, bail immediately.
    if (sret != 0 && stream_err == ForwardInterrupt) {
      retcode = ForwardInterrupt;
      ret = -1;
      goto out_response;
    }

    if (RecvResponseHeader(next_fd, &res) != 0) {
      LOG_ERROR("recv downstream push response error");
      retcode = (stream_err != ForwardSuccess) ? stream_err : ForwardInterrupt;
      ret = -1;
    } else if (res.magic != kForwardMagic || res.version != kForwardVersion) {
      retcode = ForwardInternalError;
      ret = -1;
    } else {
      retcode = (res.retcode != ForwardSuccess) ? res.retcode : stream_err;
      if (retcode != ForwardSuccess) {
        ret = -1;
      }
    }
  }

out_response:
  MakeResponse(&res, req, retcode);
  if (SendResponseHeader(fd, res) != 0) {
    LOG_ERROR("send response error");
    ret = -1;
  }
  if (next_fd >= 0) {
    close(next_fd);
  }
  return ret;
}

int PullForward(int fd, const ForwardRequest *req, const ForwardNode *nodes,
                const ForwardFile *fmeta) {
  ForwardResponse res;
  int next_fd = ConnectNextNode(nodes[0]);
  if (next_fd < 0) {
    uint8_t err = (next_fd == -2) ? ForwardUnreachable : ForwardInternalError;
    MakeResponse(&res, req, err);
    SendResponseHeader(fd, res);
    return -1;
  }

  ForwardRequest next_req = *req;
  next_req.length -= sizeof(ForwardNode);
  next_req.ttl -= 1;

  if (SendRequestHeader(next_fd, next_req) != 0 ||
      SendNodes(next_fd, nodes + 1, next_req.ttl) != 0 ||
      SendFileMeta(next_fd, reinterpret_cast<const char *>(fmeta->filename),
                   fmeta->length) != 0) {
    LOG_ERROR("send pull request to next node failed");
    MakeResponse(&res, req, ForwardInterrupt);
    SendResponseHeader(fd, res);
    close(next_fd);
    return -1;
  }

  ForwardResponse pull_res;
  if (RecvResponseHeader(next_fd, &pull_res) != 0) {
    LOG_ERROR("recv pull response error");
    MakeResponse(&res, req, ForwardInterrupt);
    SendResponseHeader(fd, res);
    close(next_fd);
    return -1;
  }

  if (pull_res.magic != kForwardMagic || pull_res.version != kForwardVersion ||
      pull_res.retcode != ForwardSuccess || pull_res.cmd != ForwardPush) {
    uint8_t err =
        (pull_res.magic == kForwardMagic && pull_res.retcode != ForwardSuccess)
            ? pull_res.retcode
            : ForwardInternalError;
    MakeResponse(&res, req, err);
    SendResponseHeader(fd, res);
    close(next_fd);
    return -1;
  }

  ForwardFile *pull_fmeta_buf = NULL;
  if (RecvFileMeta(next_fd, &pull_fmeta_buf) != 0) {
    LOG_ERROR("recv pull fmeta error");
    MakeResponse(&res, req, ForwardInterrupt);
    SendResponseHeader(fd, res);
    close(next_fd);
    return -1;
  }

  uint64_t fmeta_total_len = sizeof(ForwardFile) + pull_fmeta_buf->length;
  if (pull_res.length < sizeof(ForwardResponse) + fmeta_total_len) {
    LOG_ERROR("invalid pull_res length %llu",
              static_cast<unsigned long long>(pull_res.length));
    free(pull_fmeta_buf);
    MakeResponse(&res, req, ForwardInvalidRequest);
    SendResponseHeader(fd, res);
    close(next_fd);
    return -1;
  }

  // Forward pull data header + file metadata upstream before streaming payload.
  if (SendResponseHeader(fd, pull_res) != 0 ||
      SendFileMeta(fd, reinterpret_cast<const char *>(pull_fmeta_buf->filename),
                   pull_fmeta_buf->length) != 0) {
    LOG_ERROR("send pull header/fmeta upstream error");
    free(pull_fmeta_buf);
    close(next_fd);
    return -1;
  }

  uint64_t f_length =
      pull_res.length - sizeof(ForwardResponse) - fmeta_total_len;
  bool store_copy =
      ((req->flags & kFlagStoreOnHop) != 0) || g_always_store_on_hop;
  uint8_t stream_err = ForwardSuccess;
  int sf_ret = StreamOrTeeForward(next_fd, fd, pull_fmeta_buf, f_length,
                                  pull_res.crc32, store_copy, &stream_err);
  free(pull_fmeta_buf);
  if (sf_ret != 0 && stream_err == ForwardInterrupt) {
    close(next_fd);
    return -1;
  }

  ForwardResponse final_res;
  uint8_t final_code = stream_err;
  if (RecvResponseHeader(next_fd, &final_res) != 0) {
    LOG_ERROR("recv final pull response from next node error");
    if (final_code == ForwardSuccess) {
      final_code = ForwardInterrupt;
    }
  } else if (final_res.magic != kForwardMagic ||
             final_res.version != kForwardVersion) {
    final_code = ForwardInternalError;
  } else if (final_res.retcode != ForwardSuccess) {
    final_code = final_res.retcode;
  }

  close(next_fd);
  MakeResponse(&res, req, final_code);
  if (SendResponseHeader(fd, res) != 0) {
    LOG_ERROR("send final pull response error");
    return -1;
  }
  return final_code == ForwardSuccess ? 0 : -1;
}

int StoreLocal(int fd, const ForwardRequest *req, const ForwardFile *fmeta) {
  ForwardResponse res;
  uint64_t header_overhead =
      sizeof(ForwardRequest) + sizeof(ForwardFile) + fmeta->length;
  if (req->length < header_overhead) {
    LOG_ERROR("invalid StoreLocal length %llu",
              static_cast<unsigned long long>(req->length));
    MakeResponse(&res, req, ForwardInvalidRequest);
    SendResponseHeader(fd, res);
    return -1;
  }
  uint64_t data_len = req->length - header_overhead;

  std::string local_name =
      ExtractBasename(reinterpret_cast<const char *>(fmeta->filename));
  if (local_name.empty()) {
    LOG_ERROR("invalid local filename");
    MakeResponse(&res, req, ForwardInvalidRequest);
    SendResponseHeader(fd, res);
    return -1;
  }

  char tmp_path[] = ".fwd_store_XXXXXX";
  int tmp_fd = mkstemp(tmp_path);
  if (tmp_fd < 0) {
    LOG_ERROR("mkstemp error, %d", errno);
    MakeResponse(&res, req, ForwardInternalError);
    SendResponseHeader(fd, res);
    return -1;
  }
  fchmod(tmp_fd, kDefaultFileMode);

  uint32_t actual_crc32 = 0;
  if (RelayStream(fd, tmp_fd, -1, data_len, &actual_crc32) != 0) {
    LOG_ERROR("receive data error");
    close(tmp_fd);
    unlink(tmp_path);
    MakeResponse(&res, req, ForwardInterrupt);
    SendResponseHeader(fd, res);
    return -1;
  }
  close(tmp_fd);

  if (actual_crc32 != req->crc32) {
    LOG_ERROR("StoreLocal crc32 mismatch for %s: expected 0x%08x, got 0x%08x",
              local_name.c_str(), req->crc32, actual_crc32);
    unlink(tmp_path);
    MakeResponse(&res, req, ForwardChecksumMismatch);
    SendResponseHeader(fd, res);
    return -1;
  }

  if (rename(tmp_path, local_name.c_str()) != 0) {
    LOG_ERROR("rename temp to %s error, %d", local_name.c_str(), errno);
    unlink(tmp_path);
    MakeResponse(&res, req, ForwardInternalError);
    SendResponseHeader(fd, res);
    return -1;
  }

  MakeResponse(&res, req, ForwardSuccess);
  if (SendResponseHeader(fd, res) != 0) {
    LOG_ERROR("send response error");
    return -1;
  }
  LOG_INFO("store file(%s) to local (%llu bytes, crc32=0x%08x)",
           local_name.c_str(), static_cast<unsigned long long>(data_len),
           actual_crc32);
  return 0;
}

int PullFileToClient(int fd, const ForwardRequest *req,
                     const ForwardFile *fmeta) {
  ForwardResponse res;
  const char *fname = reinterpret_cast<const char *>(fmeta->filename);
  int r_fd = open(fname, O_RDONLY);
  if (r_fd < 0) {
    LOG_ERROR("open file(%s) error, %d", fname, errno);
    uint8_t code =
        (errno == ENOENT) ? ForwardFileNotFound : ForwardInternalError;
    MakeResponse(&res, req, code);
    SendResponseHeader(fd, res);
    return -1;
  }

  struct stat s;
  if (fstat(r_fd, &s) < 0 || !S_ISREG(s.st_mode)) {
    LOG_ERROR("stat/regular file check failed for %s", fname);
    close(r_fd);
    MakeResponse(&res, req, ForwardInternalError);
    SendResponseHeader(fd, res);
    return -1;
  }

  uint64_t f_size = static_cast<uint64_t>(s.st_size);
  uint32_t file_crc32 = 0;
  if (ComputeFileCrc32(r_fd, f_size, &file_crc32) != 0) {
    close(r_fd);
    MakeResponse(&res, req, ForwardInternalError);
    SendResponseHeader(fd, res);
    return -1;
  }

  uint32_t fname_with_nul = static_cast<uint32_t>(strlen(fname) + 1);
  uint64_t fmeta_length = sizeof(ForwardFile) + fname_with_nul;

  ForwardResponse resp_res;
  memset(&resp_res, 0, sizeof(resp_res));
  resp_res.length = sizeof(ForwardResponse) + fmeta_length + f_size;
  resp_res.magic = kForwardMagic;
  resp_res.version = kForwardVersion;
  resp_res.cmd = ForwardPush;
  resp_res.ttl = 0;
  resp_res.retcode = ForwardSuccess;
  resp_res.id = req->id;
  resp_res.crc32 = file_crc32;

  if (SendResponseHeader(fd, resp_res) != 0 ||
      SendFileMeta(fd, fname, fname_with_nul) != 0) {
    LOG_ERROR("send pull response header/meta error");
    close(r_fd);
    return -1;
  }

  int ret = RelayStream(r_fd, fd, -1, f_size, NULL);
  close(r_fd);

  if (ret != 0) {
    LOG_ERROR("stream file data error");
    return -1;
  }

  MakeResponse(&res, req, ForwardSuccess);
  if (SendResponseHeader(fd, res) != 0) {
    LOG_ERROR("send final pull response error");
    return -1;
  }
  LOG_INFO("pull file(%s) to client success (%llu bytes, crc32=0x%08x)", fname,
           static_cast<unsigned long long>(f_size), file_crc32);
  return 0;
}

void HandleConnection(int fd) {
  SetSocketTimeout(fd, kDefaultSocketTimeoutSec);

  ForwardRequest req;
  ForwardResponse res;
  ForwardNode *fnodes = NULL;
  ForwardFile *fmeta = NULL;

  if (RecvRequestHeader(fd, &req) != 0) {
    LOG_ERROR("recv req header error");
    close(fd);
    return;
  }

  LOG_DEBUG(
      "[header]len=%llu magic=0x%x ver=%u cmd=%u ttl=%u flags=0x%x "
      "id=0x%llx crc32=0x%08x",
      static_cast<unsigned long long>(req.length), req.magic, req.version,
      req.cmd, req.ttl, req.flags, static_cast<unsigned long long>(req.id),
      req.crc32);

  if (req.magic != kForwardMagic || req.version != kForwardVersion ||
      (req.cmd != ForwardPull && req.cmd != ForwardPush) ||
      req.ttl > kMaxForwardTTL) {
    LOG_ERROR("invalid request header: magic=0x%x version=%u cmd=%u ttl=%u",
              req.magic, req.version, req.cmd, req.ttl);
    MakeResponse(&res, &req, ForwardInvalidRequest);
    SendResponseHeader(fd, res);
    close(fd);
    return;
  }

  uint64_t min_len = sizeof(ForwardRequest) +
                     static_cast<uint64_t>(req.ttl) * sizeof(ForwardNode) +
                     sizeof(ForwardFile) + 1;
  if (req.length < min_len) {
    LOG_ERROR("packet length %llu smaller than minimum %llu",
              static_cast<unsigned long long>(req.length),
              static_cast<unsigned long long>(min_len));
    MakeResponse(&res, &req, ForwardInvalidRequest);
    SendResponseHeader(fd, res);
    close(fd);
    return;
  }

  if (req.ttl > 0) {
    fnodes = static_cast<ForwardNode *>(malloc(sizeof(ForwardNode) * req.ttl));
    if (!fnodes) {
      LOG_ERROR("malloc fnodes error");
      MakeResponse(&res, &req, ForwardInternalError);
      SendResponseHeader(fd, res);
      close(fd);
      return;
    }
    if (RecvNodes(fd, fnodes, req.ttl) != 0) {
      LOG_ERROR("recv nodes error");
      free(fnodes);
      close(fd);
      return;
    }
    for (uint32_t i = 0; i < req.ttl; ++i) {
      LOG_DEBUG("[node%u]ip %u, port %u", i, fnodes[i].ip, fnodes[i].port);
    }
  }

  int fm_rc = RecvFileMeta(fd, &fmeta);
  if (fm_rc != 0) {
    if (fm_rc == -2 || fm_rc == -3) {
      MakeResponse(&res, &req,
                   fm_rc == -2 ? ForwardInvalidRequest : ForwardInternalError);
      SendResponseHeader(fd, res);
    }
    free(fnodes);
    close(fd);
    return;
  }

  uint64_t header_with_fname =
      sizeof(ForwardRequest) +
      static_cast<uint64_t>(req.ttl) * sizeof(ForwardNode) +
      sizeof(ForwardFile) + fmeta->length;
  if (req.length < header_with_fname) {
    LOG_ERROR("req.length %llu smaller than header+meta %llu",
              static_cast<unsigned long long>(req.length),
              static_cast<unsigned long long>(header_with_fname));
    MakeResponse(&res, &req, ForwardInvalidRequest);
    SendResponseHeader(fd, res);
    free(fmeta);
    free(fnodes);
    close(fd);
    return;
  }

  if (!IsSafeRelativePath(reinterpret_cast<const char *>(fmeta->filename))) {
    LOG_ERROR("rejected unsafe path: %s",
              reinterpret_cast<const char *>(fmeta->filename));
    MakeResponse(&res, &req, ForwardInvalidRequest);
    SendResponseHeader(fd, res);
    free(fmeta);
    free(fnodes);
    close(fd);
    return;
  }

  if (req.cmd == ForwardPull) {
    if (req.ttl > 0) {
      PullForward(fd, &req, fnodes, fmeta);
    } else {
      PullFileToClient(fd, &req, fmeta);
    }
  } else {
    if (req.ttl > 0) {
      ForwardNext(fd, &req, fnodes, fmeta);
    } else {
      StoreLocal(fd, &req, fmeta);
    }
  }

  free(fmeta);
  free(fnodes);
  close(fd);
}

void WorkerThread() {
  while (true) {
    int fd = -1;
    {
      std::unique_lock<std::mutex> lock(g_queue_mutex);
      g_queue_cv.wait(lock, [] { return !g_task_queue.empty() || g_shutdown; });
      if (g_shutdown && g_task_queue.empty()) {
        return;
      }
      fd = g_task_queue.front();
      g_task_queue.pop();
    }
    HandleConnection(fd);
  }
}

int CreateListenSocket(int port) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    LOG_ERROR("socket error, %d", errno);
    return -1;
  }
  int optval = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) !=
      0) {
    LOG_ERROR("setsockopt error, %d", errno);
    close(sockfd);
    return -1;
  }
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(static_cast<uint16_t>(port));
  if (bind(sockfd, reinterpret_cast<struct sockaddr *>(&address),
           sizeof(address)) != 0) {
    LOG_ERROR("bind port %d error, %d", port, errno);
    close(sockfd);
    return -1;
  }
  if (listen(sockfd, kDefaultBacklog) != 0) {
    LOG_ERROR("listen error, %d", errno);
    close(sockfd);
    return -1;
  }
  return sockfd;
}

int ForwardLoop(int sockfd, int num_threads) {
  std::vector<std::thread> workers;
  workers.reserve(num_threads);
  for (int i = 0; i < num_threads; ++i) {
    workers.emplace_back(WorkerThread);
  }

  LOG_INFO("Forward daemon listening with %d worker threads", num_threads);

  struct pollfd pfd;
  pfd.fd = sockfd;
  pfd.events = POLLIN;

  while (!g_stop_requested) {
    pfd.revents = 0;
    int pret = poll(&pfd, 1, 200);
    if (pret < 0) {
      if (errno == EINTR) {
        continue;
      }
      LOG_ERROR("poll error, %d", errno);
      break;
    }
    if (pret == 0 || !(pfd.revents & POLLIN)) {
      continue;
    }

    struct sockaddr_in src_addr;
    socklen_t addr_len = sizeof(src_addr);
    int fd = accept(sockfd, reinterpret_cast<struct sockaddr *>(&src_addr),
                    &addr_len);
    if (fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      LOG_ERROR("accept error, %d", errno);
      continue;
    }

    char ip_str[64] = {0};
    inet_ntop(AF_INET, &src_addr.sin_addr, ip_str, sizeof(ip_str));
    LOG_DEBUG("accept %d, ip: %s, port: %d", fd, ip_str,
              ntohs(src_addr.sin_port));
    {
      std::lock_guard<std::mutex> lock(g_queue_mutex);
      g_task_queue.push(fd);
    }
    g_queue_cv.notify_one();
  }

  LOG_INFO("Shutting down forward daemon...");
  close(sockfd);
  {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_shutdown = true;
  }
  g_queue_cv.notify_all();
  for (auto &t : workers) {
    t.join();
  }
  LOG_INFO("Forward daemon stopped cleanly");
  return 0;
}

void PrintUsage(const char *prog) {
  printf(
      "Usage: %s [-p port] [-d dir] [-t threads] [-P pidfile] [-F] [-S] [-h]\n",
      prog);
  printf("  -p port     : specify port (default %u)\n", kDefaultPort);
  printf("  -d dir      : specify working directory (default .)\n");
  printf("  -t threads  : number of worker threads (default 4)\n");
  printf("  -P pidfile  : write process PID to pidfile\n");
  printf("  -F          : run in foreground (do not daemonize)\n");
  printf("  -S          : always cache relayed files on intermediate hops\n");
  printf("  -h          : show this help\n");
}

bool WritePidFile(const std::string &pidfile) {
  FILE *fp = fopen(pidfile.c_str(), "w");
  if (!fp) {
    LOG_ERROR("failed to open pidfile %s, %d", pidfile.c_str(), errno);
    return false;
  }
  fprintf(fp, "%d\n", static_cast<int>(getpid()));
  fclose(fp);
  return true;
}
}  // namespace

int main(int argc, char *argv[]) {
  int opt;
  int port = static_cast<int>(kDefaultPort);
  std::string dir = ".";
  std::string pidfile;
  int num_threads = 4;
  bool foreground = false;

  while ((opt = getopt(argc, argv, "p:d:t:P:FSh")) != -1) {
    switch (opt) {
      case 'p':
        port = atoi(optarg);
        if (port <= 0 || port > 65535) {
          fprintf(stderr, "Invalid port: %s\n", optarg);
          return 1;
        }
        break;
      case 'd':
        dir = optarg;
        break;
      case 't':
        num_threads = atoi(optarg);
        if (num_threads <= 0 || num_threads > 256) {
          fprintf(stderr, "Invalid thread count: %s\n", optarg);
          return 1;
        }
        break;
      case 'P':
        pidfile = optarg;
        break;
      case 'F':
        foreground = true;
        break;
      case 'S':
        g_always_store_on_hop = true;
        break;
      case 'h':
        PrintUsage(argv[0]);
        return 0;
      default:
        PrintUsage(argv[0]);
        return 1;
    }
  }

  struct stat s;
  if (stat(dir.c_str(), &s) != 0 || !S_ISDIR(s.st_mode)) {
    fprintf(stderr, "Invalid working directory: %s\n", dir.c_str());
    return 1;
  }

  char resolved_dir[PATH_MAX];
  if (!realpath(dir.c_str(), resolved_dir)) {
    fprintf(stderr, "realpath failed for %s: %s\n", dir.c_str(),
            strerror(errno));
    return 1;
  }
  dir = resolved_dir;

  if (!pidfile.empty() && pidfile[0] != '/') {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
      pidfile = std::string(cwd) + "/" + pidfile;
    }
  }

  std::string log_path = dir + "/forward_server.log";
  if (LogInit(log_path.c_str()) != 0) {
    return 1;
  }
  atexit(LogClose);

  signal(SIGPIPE, SIG_IGN);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SignalHandler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  int listen_fd = CreateListenSocket(port);
  if (listen_fd < 0) {
    return 1;
  }

  if (!foreground) {
    pid_t pid = fork();
    if (pid < 0) {
      LOG_ERROR("fork error, %d", errno);
      close(listen_fd);
      return 1;
    }
    if (pid > 0) {
      close(listen_fd);
      return 0;
    }
    if (setsid() < 0) {
      LOG_ERROR("setsid error, %d", errno);
      close(listen_fd);
      return 1;
    }
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    pid = fork();
    if (pid < 0) {
      LOG_ERROR("fork error, %d", errno);
      close(listen_fd);
      return 1;
    }
    if (pid > 0) {
      close(listen_fd);
      return 0;
    }
    umask(0022);
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO) {
        close(devnull);
      }
    }
  }

  if (chdir(dir.c_str()) != 0) {
    LOG_ERROR("chdir(%s) error, %d", dir.c_str(), errno);
    close(listen_fd);
    return 1;
  }

  if (!pidfile.empty() && !WritePidFile(pidfile)) {
    close(listen_fd);
    return 1;
  }

  LOG_INFO(
      "Forward daemon started (pid=%d, port=%d, dir=%s, foreground=%d, "
      "store_on_hop=%d)",
      static_cast<int>(getpid()), port, dir.c_str(), foreground ? 1 : 0,
      g_always_store_on_hop ? 1 : 0);

  int rc = ForwardLoop(listen_fd, num_threads);
  if (!pidfile.empty()) {
    unlink(pidfile.c_str());
  }
  return rc;
}
