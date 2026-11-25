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

// Receives data_len bytes from src_fd into a temporary file in the working
// directory, forwards the same bytes to dst_fd, and atomically renames the
// temporary file onto the local basename.
int StoreAndForward(int src_fd, int dst_fd, const ForwardFile *fmeta,
                    uint32_t data_len) {
  std::string local_name =
      ExtractBasename(reinterpret_cast<const char *>(fmeta->filename));
  if (local_name.empty()) {
    LOG_ERROR("invalid filename for local store");
    return -1;
  }

  char tmp_path[] = ".fwd_forward_XXXXXX";
  int tmp_fd = mkstemp(tmp_path);
  if (tmp_fd < 0) {
    LOG_ERROR("mkstemp error, %d", errno);
    return -1;
  }
  fchmod(tmp_fd, kDefaultFileMode);

  if (ForwardSync(src_fd, tmp_fd, data_len) != 0) {
    LOG_ERROR("receive to temp error");
    close(tmp_fd);
    unlink(tmp_path);
    return -1;
  }

  if (lseek(tmp_fd, 0, SEEK_SET) < 0) {
    LOG_ERROR("lseek temp error, %d", errno);
    close(tmp_fd);
    unlink(tmp_path);
    return -1;
  }

  if (ForwardSync(tmp_fd, dst_fd, data_len) != 0) {
    LOG_ERROR("forward from temp error");
    close(tmp_fd);
    unlink(tmp_path);
    return -1;
  }

  close(tmp_fd);
  if (rename(tmp_path, local_name.c_str()) != 0) {
    LOG_ERROR("rename temp to %s error, %d", local_name.c_str(), errno);
    unlink(tmp_path);
    return -1;
  }

  LOG_INFO("stored and forwarded %u bytes (%s)", data_len, local_name.c_str());
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
    LOG_ERROR("invalid push packet length %u < %llu", req->length,
              static_cast<unsigned long long>(header_overhead));
    MakeResponse(&res, req, ForwardInvalidRequest);
    SendSync(fd, &res, sizeof(res));
    return -1;
  }
  uint32_t data_len = static_cast<uint32_t>(req->length - header_overhead);

  int next_fd = ConnectNextNode(nodes[0]);
  if (next_fd < 0) {
    retcode = (next_fd == -2) ? ForwardUnreachable : ForwardInternalError;
    ret = -1;
    goto out_response;
  }

  ForwardRequest next_req;
  memcpy(&next_req, req, sizeof(next_req));
  next_req.length -= sizeof(ForwardNode);
  next_req.ttl -= 1;

  if (SendSync(next_fd, &next_req, sizeof(ForwardRequest)) != 0) {
    LOG_ERROR("send forward req error");
    retcode = ForwardInterrupt;
    ret = -1;
    goto out_response;
  }
  if (next_req.ttl > 0) {
    if (SendSync(next_fd, nodes + 1, sizeof(ForwardNode) * next_req.ttl) != 0) {
      LOG_ERROR("send forward nodes error");
      retcode = ForwardInterrupt;
      ret = -1;
      goto out_response;
    }
  }
  if (SendSync(next_fd, fmeta, sizeof(ForwardFile) + fmeta->length) != 0) {
    LOG_ERROR("send forward file error");
    retcode = ForwardInterrupt;
    ret = -1;
    goto out_response;
  }

  if (StoreAndForward(fd, next_fd, fmeta, data_len) != 0) {
    LOG_ERROR("store and forward error");
    retcode = ForwardInterrupt;
    ret = -1;
    goto out_response;
  }

  if (RecvSync(next_fd, &res, sizeof(res)) != 0) {
    LOG_ERROR("recv response error");
    retcode = ForwardInterrupt;
    ret = -1;
  } else {
    retcode = res.retcode;
    if (retcode != ForwardSuccess) {
      ret = -1;
    }
  }

out_response:
  MakeResponse(&res, req, retcode);
  if (SendSync(fd, &res, sizeof(res)) != 0) {
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
    SendSync(fd, &res, sizeof(res));
    return -1;
  }

  ForwardRequest next_req;
  memcpy(&next_req, req, sizeof(next_req));
  next_req.length -= sizeof(ForwardNode);
  next_req.ttl -= 1;

  if (SendSync(next_fd, &next_req, sizeof(next_req)) != 0 ||
      (next_req.ttl > 0 &&
       SendSync(next_fd, nodes + 1, sizeof(ForwardNode) * next_req.ttl) != 0) ||
      SendSync(next_fd, fmeta, sizeof(ForwardFile) + fmeta->length) != 0) {
    LOG_ERROR("send pull request to next node failed");
    MakeResponse(&res, req, ForwardInterrupt);
    SendSync(fd, &res, sizeof(res));
    close(next_fd);
    return -1;
  }

  ForwardResponse pull_res;
  if (RecvSync(next_fd, &pull_res, sizeof(pull_res)) != 0) {
    LOG_ERROR("recv pull response error");
    MakeResponse(&res, req, ForwardInterrupt);
    SendSync(fd, &res, sizeof(res));
    close(next_fd);
    return -1;
  }

  if (pull_res.magic != kForwardMagic || pull_res.retcode != ForwardSuccess ||
      pull_res.cmd != ForwardPush) {
    uint8_t err =
        (pull_res.magic == kForwardMagic && pull_res.retcode != ForwardSuccess)
            ? pull_res.retcode
            : ForwardInternalError;
    MakeResponse(&res, req, err);
    SendSync(fd, &res, sizeof(res));
    close(next_fd);
    return -1;
  }

  ForwardFile pull_fmeta;
  if (RecvSync(next_fd, &pull_fmeta, sizeof(pull_fmeta)) != 0 ||
      pull_fmeta.length == 0 || pull_fmeta.length > kMaxFilenameLen) {
    LOG_ERROR("recv or validate pull fmeta error");
    MakeResponse(&res, req, ForwardInterrupt);
    SendSync(fd, &res, sizeof(res));
    close(next_fd);
    return -1;
  }

  uint32_t fmeta_total_len = sizeof(ForwardFile) + pull_fmeta.length;
  if (pull_res.length < sizeof(ForwardResponse) + fmeta_total_len) {
    LOG_ERROR("invalid pull_res length %u", pull_res.length);
    MakeResponse(&res, req, ForwardInvalidRequest);
    SendSync(fd, &res, sizeof(res));
    close(next_fd);
    return -1;
  }

  ForwardFile *pull_fmeta_buf =
      static_cast<ForwardFile *>(malloc(fmeta_total_len + 1));
  if (!pull_fmeta_buf) {
    LOG_ERROR("malloc error");
    MakeResponse(&res, req, ForwardInternalError);
    SendSync(fd, &res, sizeof(res));
    close(next_fd);
    return -1;
  }
  pull_fmeta_buf->length = pull_fmeta.length;
  if (RecvSync(next_fd, pull_fmeta_buf->filename, pull_fmeta.length) != 0) {
    LOG_ERROR("recv filename error");
    free(pull_fmeta_buf);
    MakeResponse(&res, req, ForwardInterrupt);
    SendSync(fd, &res, sizeof(res));
    close(next_fd);
    return -1;
  }
  pull_fmeta_buf->filename[pull_fmeta.length - 1] = '\0';

  // Begin streaming push header + metadata + file data back to upstream caller.
  if (SendSync(fd, &pull_res, sizeof(pull_res)) != 0 ||
      SendSync(fd, pull_fmeta_buf, fmeta_total_len) != 0) {
    LOG_ERROR("send pull header/fmeta upstream error");
    free(pull_fmeta_buf);
    close(next_fd);
    return -1;
  }

  uint32_t f_length =
      pull_res.length - sizeof(ForwardResponse) - fmeta_total_len;
  int sf_ret = StoreAndForward(next_fd, fd, pull_fmeta_buf, f_length);
  free(pull_fmeta_buf);
  if (sf_ret != 0) {
    LOG_ERROR("pull store and forward error");
    close(next_fd);
    return -1;
  }

  ForwardResponse final_res;
  uint8_t final_code = ForwardSuccess;
  if (RecvSync(next_fd, &final_res, sizeof(final_res)) != 0) {
    LOG_ERROR("recv final pull response from next node error");
    final_code = ForwardInterrupt;
  } else if (final_res.magic != kForwardMagic) {
    final_code = ForwardInternalError;
  } else {
    final_code = final_res.retcode;
  }

  close(next_fd);
  MakeResponse(&res, req, final_code);
  if (SendSync(fd, &res, sizeof(res)) != 0) {
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
    LOG_ERROR("invalid StoreLocal length %u", req->length);
    MakeResponse(&res, req, ForwardInvalidRequest);
    SendSync(fd, &res, sizeof(res));
    return -1;
  }
  uint32_t data_len = static_cast<uint32_t>(req->length - header_overhead);

  std::string local_name =
      ExtractBasename(reinterpret_cast<const char *>(fmeta->filename));
  if (local_name.empty()) {
    LOG_ERROR("invalid local filename");
    MakeResponse(&res, req, ForwardInvalidRequest);
    SendSync(fd, &res, sizeof(res));
    return -1;
  }

  char tmp_path[] = ".fwd_store_XXXXXX";
  int tmp_fd = mkstemp(tmp_path);
  if (tmp_fd < 0) {
    LOG_ERROR("mkstemp error, %d", errno);
    MakeResponse(&res, req, ForwardInternalError);
    SendSync(fd, &res, sizeof(res));
    return -1;
  }
  fchmod(tmp_fd, kDefaultFileMode);

  if (ForwardSync(fd, tmp_fd, data_len) != 0) {
    LOG_ERROR("forward data error");
    close(tmp_fd);
    unlink(tmp_path);
    MakeResponse(&res, req, ForwardInterrupt);
    SendSync(fd, &res, sizeof(res));
    return -1;
  }
  close(tmp_fd);

  if (rename(tmp_path, local_name.c_str()) != 0) {
    LOG_ERROR("rename temp to %s error, %d", local_name.c_str(), errno);
    unlink(tmp_path);
    MakeResponse(&res, req, ForwardInternalError);
    SendSync(fd, &res, sizeof(res));
    return -1;
  }

  MakeResponse(&res, req, ForwardSuccess);
  if (SendSync(fd, &res, sizeof(res)) != 0) {
    LOG_ERROR("send response error");
    return -1;
  }
  LOG_INFO("store file(%s) to local (%u bytes)", local_name.c_str(), data_len);
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
    SendSync(fd, &res, sizeof(res));
    return -1;
  }

  struct stat s;
  if (fstat(r_fd, &s) < 0 || !S_ISREG(s.st_mode)) {
    LOG_ERROR("stat/regular file check failed for %s", fname);
    close(r_fd);
    MakeResponse(&res, req, ForwardInternalError);
    SendSync(fd, &res, sizeof(res));
    return -1;
  }

  uint64_t f_size = static_cast<uint64_t>(s.st_size);
  size_t fname_len = strlen(fname);
  uint32_t fmeta_length =
      static_cast<uint32_t>(sizeof(ForwardFile) + fname_len + 1);
  uint64_t total_resp_len = sizeof(ForwardResponse) + fmeta_length + f_size;
  if (total_resp_len > UINT32_MAX) {
    LOG_ERROR("file too large for v1 protocol: %llu bytes",
              static_cast<unsigned long long>(f_size));
    close(r_fd);
    MakeResponse(&res, req, ForwardInternalError);
    SendSync(fd, &res, sizeof(res));
    return -1;
  }

  ForwardFile *resp_fmeta = static_cast<ForwardFile *>(malloc(fmeta_length));
  if (!resp_fmeta) {
    LOG_ERROR("malloc error");
    close(r_fd);
    MakeResponse(&res, req, ForwardInternalError);
    SendSync(fd, &res, sizeof(res));
    return -1;
  }

  ForwardResponse resp_res;
  memset(&resp_res, 0, sizeof(resp_res));
  resp_res.length = static_cast<uint32_t>(total_resp_len);
  resp_res.magic = kForwardMagic;
  resp_res.version = kForwardVersion1;
  resp_res.cmd = ForwardPush;
  resp_res.ttl = 0;
  resp_res.retcode = ForwardSuccess;
  resp_res.id = req->id;

  resp_fmeta->length = static_cast<uint32_t>(fname_len + 1);
  memcpy(resp_fmeta->filename, fname, fname_len);
  resp_fmeta->filename[fname_len] = '\0';

  if (SendSync(fd, &resp_res, sizeof(resp_res)) != 0 ||
      SendSync(fd, resp_fmeta, fmeta_length) != 0) {
    LOG_ERROR("send pull response header/meta error");
    free(resp_fmeta);
    close(r_fd);
    return -1;
  }
  free(resp_fmeta);

  int ret = ForwardSync(r_fd, fd, static_cast<size_t>(f_size));
  close(r_fd);

  if (ret != 0) {
    LOG_ERROR("forward file data error");
    return -1;
  }

  MakeResponse(&res, req, ForwardSuccess);
  if (SendSync(fd, &res, sizeof(res)) != 0) {
    LOG_ERROR("send final pull response error");
    return -1;
  }
  LOG_INFO("pull file(%s) to client success", fname);
  return 0;
}

void HandleConnection(int fd) {
  SetSocketTimeout(fd, kDefaultSocketTimeoutSec);

  ForwardRequest req;
  ForwardResponse res;
  ForwardNode *fnodes = NULL;
  ForwardFile *fmeta = NULL;

  if (RecvSync(fd, &req, sizeof(ForwardRequest)) != 0) {
    LOG_ERROR("recv req header error");
    close(fd);
    return;
  }

  LOG_DEBUG(
      "[header]length %u, magic 0x%x, version %u, cmd %u, ttl %u, id %llu",
      req.length, req.magic, req.version, req.cmd, req.ttl,
      static_cast<unsigned long long>(req.id));

  if (req.magic != kForwardMagic || req.version != kForwardVersion1 ||
      (req.cmd != ForwardPull && req.cmd != ForwardPush) ||
      req.ttl > kMaxForwardTTL) {
    LOG_ERROR("invalid request header: magic=0x%x version=%u cmd=%u ttl=%u",
              req.magic, req.version, req.cmd, req.ttl);
    MakeResponse(&res, &req, ForwardInvalidRequest);
    SendSync(fd, &res, sizeof(res));
    close(fd);
    return;
  }

  uint64_t min_len = sizeof(ForwardRequest) +
                     static_cast<uint64_t>(req.ttl) * sizeof(ForwardNode) +
                     sizeof(ForwardFile) + 1;
  if (req.length < min_len) {
    LOG_ERROR("packet length %u smaller than minimum %llu", req.length,
              static_cast<unsigned long long>(min_len));
    MakeResponse(&res, &req, ForwardInvalidRequest);
    SendSync(fd, &res, sizeof(res));
    close(fd);
    return;
  }

  if (req.ttl > 0) {
    fnodes = static_cast<ForwardNode *>(malloc(sizeof(ForwardNode) * req.ttl));
    if (!fnodes) {
      LOG_ERROR("malloc fnodes error");
      MakeResponse(&res, &req, ForwardInternalError);
      SendSync(fd, &res, sizeof(res));
      close(fd);
      return;
    }
    if (RecvSync(fd, fnodes, sizeof(ForwardNode) * req.ttl) != 0) {
      LOG_ERROR("recv nodes error");
      free(fnodes);
      close(fd);
      return;
    }
    for (uint32_t i = 0; i < req.ttl; ++i) {
      LOG_DEBUG("[node%u]ip %u, port %u", i, fnodes[i].ip, fnodes[i].port);
    }
  }

  uint32_t f_length = 0;
  if (RecvSync(fd, &f_length, sizeof(uint32_t)) != 0) {
    LOG_ERROR("recv file meta length error");
    free(fnodes);
    close(fd);
    return;
  }

  uint64_t header_with_fname =
      sizeof(ForwardRequest) +
      static_cast<uint64_t>(req.ttl) * sizeof(ForwardNode) +
      sizeof(ForwardFile) + f_length;
  if (f_length == 0 || f_length > kMaxFilenameLen ||
      req.length < header_with_fname) {
    LOG_ERROR("invalid filename length %u (req.length %u)", f_length,
              req.length);
    MakeResponse(&res, &req, ForwardInvalidRequest);
    SendSync(fd, &res, sizeof(res));
    free(fnodes);
    close(fd);
    return;
  }

  fmeta =
      static_cast<ForwardFile *>(malloc(sizeof(ForwardFile) + f_length + 1));
  if (!fmeta) {
    LOG_ERROR("malloc fmeta error");
    MakeResponse(&res, &req, ForwardInternalError);
    SendSync(fd, &res, sizeof(res));
    free(fnodes);
    close(fd);
    return;
  }
  fmeta->length = f_length;
  if (RecvSync(fd, fmeta->filename, f_length) != 0) {
    LOG_ERROR("recv file name error");
    free(fmeta);
    free(fnodes);
    close(fd);
    return;
  }
  fmeta->filename[f_length - 1] = '\0';

  if (!IsSafeRelativePath(reinterpret_cast<const char *>(fmeta->filename))) {
    LOG_ERROR("rejected unsafe path: %s",
              reinterpret_cast<const char *>(fmeta->filename));
    MakeResponse(&res, &req, ForwardInvalidRequest);
    SendSync(fd, &res, sizeof(res));
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
  printf("Usage: %s [-p port] [-d dir] [-t threads] [-P pidfile] [-F] [-h]\n",
         prog);
  printf("  -p port     : specify port (default %u)\n", kDefaultPort);
  printf("  -d dir      : specify working directory (default .)\n");
  printf("  -t threads  : number of worker threads (default 4)\n");
  printf("  -P pidfile  : write process PID to pidfile\n");
  printf("  -F          : run in foreground (do not daemonize)\n");
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

  while ((opt = getopt(argc, argv, "p:d:t:P:Fh")) != -1) {
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

  LOG_INFO("Forward daemon started (pid=%d, port=%d, dir=%s, foreground=%d)",
           static_cast<int>(getpid()), port, dir.c_str(), foreground ? 1 : 0);

  int rc = ForwardLoop(listen_fd, num_threads);
  if (!pidfile.empty()) {
    unlink(pidfile.c_str());
  }
  return rc;
}
