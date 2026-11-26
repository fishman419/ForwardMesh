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

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "logger.h"
#include "mesh.h"
#include "protocol.h"
#include "util.h"

namespace {
const size_t kMaxElasticWorkers = 64;
const uint64_t kMaxSyncPayloadBytes = 512 * 1024;

std::queue<int> g_task_queue;
std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
size_t g_idle_workers = 0;
bool g_shutdown = false;
bool g_always_store_on_hop = false;
volatile sig_atomic_t g_stop_requested = 0;

MeshRouter g_mesh_router;
std::mutex g_hb_mutex;
std::condition_variable g_hb_cv;
bool g_hb_stop = false;

void SignalHandler(int /*signo*/) { g_stop_requested = 1; }

bool SameNode(const ForwardNode &a, const ForwardNode &b) {
  return a.ip == b.ip && a.port == b.port;
}

void DrainSmallPayload(int fd, uint64_t len) {
  if (len == 0 || len > kForwardBufSize) {
    return;
  }
  std::vector<char> discard(static_cast<size_t>(len));
  RecvSync(fd, discard.data(), discard.size());
}

bool SyncNeighborOnce(const ForwardNode &peer) {
  uint16_t rtt_ms = 1;
  int fd =
      ConnectWithTimeout(peer.ip, peer.port, kProbeConnectTimeoutMs, &rtt_ms);
  if (fd < 0) {
    g_mesh_router.RecordConnectFailure(peer);
    return false;
  }
  SetSocketTimeout(fd, 2);
  g_mesh_router.RegisterSelfFromSocket(fd);
  if (g_mesh_router.IsSelf(peer)) {
    close(fd);
    return false;
  }

  ForwardRequest req;
  memset(&req, 0, sizeof(req));
  req.length = sizeof(ForwardRequest);
  req.magic = kForwardMagic;
  req.version = kForwardVersion;
  req.cmd = ForwardMeshSync;
  req.ttl = 0;
  req.flags = 0;
  req.id = 0;
  req.crc32 = 0;

  if (SendRequestHeader(fd, req) != 0) {
    close(fd);
    g_mesh_router.RecordConnectFailure(peer);
    return false;
  }

  ForwardResponse res;
  if (RecvResponseHeader(fd, &res) != 0) {
    close(fd);
    g_mesh_router.RecordConnectFailure(peer);
    return false;
  }
  if (res.magic != kForwardMagic || res.version != kForwardVersion ||
      res.cmd != ForwardMeshSync || res.retcode != ForwardSuccess ||
      res.length < sizeof(ForwardResponse) ||
      (res.length - sizeof(ForwardResponse)) > kMaxSyncPayloadBytes) {
    close(fd);
    return false;
  }

  size_t payload_len =
      static_cast<size_t>(res.length - sizeof(ForwardResponse));
  std::vector<uint8_t> payload(payload_len);
  if (payload_len > 0 && RecvSync(fd, payload.data(), payload_len) != 0) {
    close(fd);
    g_mesh_router.RecordConnectFailure(peer);
    return false;
  }
  close(fd);

  uint32_t actual_crc = Crc32Update(0, payload.data(), payload.size());
  if (actual_crc != res.crc32) {
    LOG_WARNING("[MESH]sync payload crc32 mismatch from %s",
                FormatNodeAddr(peer).c_str());
    return false;
  }

  std::vector<MeshSyncEntry> entries;
  if (DecodeMeshSyncPayload(payload.data(), payload.size(), &entries) != 0) {
    LOG_WARNING("[MESH]invalid sync payload from %s",
                FormatNodeAddr(peer).c_str());
    return false;
  }

  g_mesh_router.ApplyNeighborSync(peer, rtt_ms, entries);
  return true;
}

void SyncAllNeighborsOnce() {
  std::vector<ForwardNode> peers = g_mesh_router.GetDirectNeighbors();
  for (const auto &peer : peers) {
    if (g_stop_requested) {
      break;
    }
    SyncNeighborOnce(peer);
  }
}

void MeshHeartbeatThread(uint32_t interval_ms) {
  SyncAllNeighborsOnce();
  std::unique_lock<std::mutex> lock(g_hb_mutex);
  while (!g_hb_stop && !g_stop_requested) {
    g_hb_cv.wait_for(lock, std::chrono::milliseconds(interval_ms),
                     [] { return g_hb_stop || g_stop_requested != 0; });
    if (g_hb_stop || g_stop_requested) {
      break;
    }
    lock.unlock();
    SyncAllNeighborsOnce();
    lock.lock();
  }
}

int HandleMeshSync(int fd, const ForwardRequest *req) {
  ForwardResponse res;
  std::vector<MeshSyncEntry> snap = g_mesh_router.ExportSyncSnapshot();
  std::vector<uint8_t> payload;
  EncodeMeshSyncPayload(snap, &payload);
  uint32_t crc = Crc32Update(0, payload.data(), payload.size());

  MakeResponse(&res, req, ForwardSuccess);
  res.length = sizeof(ForwardResponse) + payload.size();
  res.crc32 = crc;

  if (SendResponseHeader(fd, res) != 0 ||
      (!payload.empty() && SendSync(fd, payload.data(), payload.size()) != 0)) {
    LOG_ERROR("[MESH]send sync response error");
    return -1;
  }
  return 0;
}

int EstablishNextHop(const ForwardRequest *req,
                     const std::vector<ForwardNode> &req_nodes,
                     HopPlan *out_plan, uint16_t *out_rtt_ms) {
  bool strict_mode = (req->flags & kFlagStrictRoute) != 0;
  uint8_t hops_traversed = GetHopsTraversed(req->flags);
  int timeout_ms =
      strict_mode ? kStrictConnectTimeoutMs : kDefaultConnectTimeoutMs;
  bool should_sync_on_miss = false;

  std::vector<HopPlan> plans = g_mesh_router.BuildHopPlans(
      req_nodes, hops_traversed, strict_mode, &should_sync_on_miss);

  for (const auto &p : plans) {
    uint16_t rtt = 1;
    int next_fd =
        ConnectWithTimeout(p.first_hop.ip, p.first_hop.port, timeout_ms, &rtt);
    if (next_fd >= 0) {
      g_mesh_router.RegisterSelfFromSocket(next_fd);
      if (g_mesh_router.IsSelf(p.first_hop)) {
        close(next_fd);
        continue;
      }
      *out_plan = p;
      *out_rtt_ms = rtt;
      return next_fd;
    }
    g_mesh_router.RecordConnectFailure(p.first_hop);
  }

  if (!strict_mode && should_sync_on_miss && !req_nodes.empty()) {
    LOG_DEBUG("[MESH]cache miss for %s, triggering on-demand neighbor sync",
              FormatNodeAddr(req_nodes[0]).c_str());
    SyncAllNeighborsOnce();
    std::vector<HopPlan> retry_plans =
        g_mesh_router.BuildHopPlans(req_nodes, hops_traversed, false, NULL);
    for (const auto &p : retry_plans) {
      if (SameNode(p.first_hop, req_nodes[0]) &&
          p.remaining_tail.size() + 1 == req_nodes.size()) {
        continue;
      }
      uint16_t rtt = 1;
      int next_fd = ConnectWithTimeout(p.first_hop.ip, p.first_hop.port,
                                       timeout_ms, &rtt);
      if (next_fd >= 0) {
        g_mesh_router.RegisterSelfFromSocket(next_fd);
        if (g_mesh_router.IsSelf(p.first_hop)) {
          close(next_fd);
          continue;
        }
        *out_plan = p;
        *out_rtt_ms = rtt;
        return next_fd;
      }
      g_mesh_router.RecordConnectFailure(p.first_hop);
    }
  }

  return -2;
}

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

  std::vector<ForwardNode> req_nodes(nodes, nodes + req->ttl);
  HopPlan chosen_plan;
  uint16_t connect_rtt_ms = 1;
  int next_fd = EstablishNextHop(req, req_nodes, &chosen_plan, &connect_rtt_ms);
  if (next_fd < 0) {
    DrainSmallPayload(fd, data_len);
    retcode = (next_fd == -2) ? ForwardUnreachable : ForwardInternalError;
    ret = -1;
    goto out_response;
  }

  {
    uint8_t new_ttl = static_cast<uint8_t>(chosen_plan.remaining_tail.size());
    uint8_t next_hops = static_cast<uint8_t>(GetHopsTraversed(req->flags) + 1);
    ForwardRequest next_req = *req;
    next_req.ttl = new_ttl;
    next_req.flags = MakePacketFlags(req->flags, next_hops);
    next_req.length = sizeof(ForwardRequest) +
                      static_cast<uint64_t>(new_ttl) * sizeof(ForwardNode) +
                      sizeof(ForwardFile) + fmeta->length + data_len;

    if (SendRequestHeader(next_fd, next_req) != 0 ||
        SendNodes(next_fd, chosen_plan.remaining_tail.data(), new_ttl) != 0 ||
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

    if (sret != 0 && stream_err == ForwardInterrupt) {
      retcode = ForwardInterrupt;
      ret = -1;
      goto out_response;
    }

    if (RecvResponseHeader(next_fd, &res) != 0) {
      LOG_ERROR("recv downstream push response error");
      retcode = (stream_err != ForwardSuccess)
                    ? stream_err
                    : static_cast<uint8_t>(ForwardInterrupt);
      ret = -1;
    } else if (res.magic != kForwardMagic || res.version != kForwardVersion) {
      retcode = ForwardInternalError;
      ret = -1;
    } else {
      retcode = (res.retcode != ForwardSuccess) ? res.retcode : stream_err;
      if (retcode == ForwardSuccess) {
        g_mesh_router.RecordTransferSuccess(
            chosen_plan.first_hop, connect_rtt_ms, chosen_plan.remaining_tail,
            (req->flags & kFlagStrictRoute) != 0);
      } else {
        if (retcode == ForwardUnreachable && !req_nodes.empty()) {
          g_mesh_router.RecordDownstreamUnreachable(chosen_plan.first_hop,
                                                    req_nodes.back());
        }
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
  std::vector<ForwardNode> req_nodes(nodes, nodes + req->ttl);
  HopPlan chosen_plan;
  uint16_t connect_rtt_ms = 1;
  int next_fd = EstablishNextHop(req, req_nodes, &chosen_plan, &connect_rtt_ms);
  if (next_fd < 0) {
    uint8_t err = (next_fd == -2) ? ForwardUnreachable : ForwardInternalError;
    MakeResponse(&res, req, err);
    SendResponseHeader(fd, res);
    return -1;
  }

  uint8_t new_ttl = static_cast<uint8_t>(chosen_plan.remaining_tail.size());
  uint8_t next_hops = static_cast<uint8_t>(GetHopsTraversed(req->flags) + 1);
  ForwardRequest next_req = *req;
  next_req.ttl = new_ttl;
  next_req.flags = MakePacketFlags(req->flags, next_hops);
  next_req.length = sizeof(ForwardRequest) +
                    static_cast<uint64_t>(new_ttl) * sizeof(ForwardNode) +
                    sizeof(ForwardFile) + fmeta->length;

  if (SendRequestHeader(next_fd, next_req) != 0 ||
      SendNodes(next_fd, chosen_plan.remaining_tail.data(), new_ttl) != 0 ||
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
            : static_cast<uint8_t>(ForwardInternalError);
    if (err == ForwardUnreachable && !req_nodes.empty()) {
      g_mesh_router.RecordDownstreamUnreachable(chosen_plan.first_hop,
                                                req_nodes.back());
    }
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
  if (final_code == ForwardSuccess) {
    g_mesh_router.RecordTransferSuccess(chosen_plan.first_hop, connect_rtt_ms,
                                        chosen_plan.remaining_tail,
                                        (req->flags & kFlagStrictRoute) != 0);
  }

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
  g_mesh_router.RegisterSelfFromSocket(fd);

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
      "hops=%u id=0x%llx crc32=0x%08x",
      static_cast<unsigned long long>(req.length), req.magic, req.version,
      req.cmd, req.ttl, req.flags, GetHopsTraversed(req.flags),
      static_cast<unsigned long long>(req.id), req.crc32);

  if (req.magic != kForwardMagic || req.version != kForwardVersion ||
      (req.cmd != ForwardPull && req.cmd != ForwardPush &&
       req.cmd != ForwardMeshSync) ||
      req.ttl > kMaxForwardTTL ||
      GetHopsTraversed(req.flags) > kMaxForwardTTL) {
    LOG_ERROR("invalid request header: magic=0x%x version=%u cmd=%u ttl=%u",
              req.magic, req.version, req.cmd, req.ttl);
    MakeResponse(&res, &req, ForwardInvalidRequest);
    SendResponseHeader(fd, res);
    close(fd);
    return;
  }

  if (req.cmd == ForwardMeshSync) {
    if (req.ttl != 0 || req.length != sizeof(ForwardRequest)) {
      MakeResponse(&res, &req, ForwardInvalidRequest);
      SendResponseHeader(fd, res);
      close(fd);
      return;
    }
    HandleMeshSync(fd, &req);
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
      g_idle_workers += 1;
      g_queue_cv.wait(lock, [] { return !g_task_queue.empty() || g_shutdown; });
      g_idle_workers -= 1;
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

int ForwardLoop(int sockfd, int num_threads, uint32_t heartbeat_ms) {
  std::vector<std::thread> workers;
  size_t max_workers =
      std::max<size_t>(static_cast<size_t>(num_threads), kMaxElasticWorkers);
  workers.reserve(max_workers);
  for (int i = 0; i < num_threads; ++i) {
    workers.emplace_back(WorkerThread);
  }

  std::thread hb_thread;
  if (heartbeat_ms > 0) {
    hb_thread = std::thread(MeshHeartbeatThread, heartbeat_ms);
  }

  LOG_INFO(
      "Forward daemon listening with %d base worker threads (heartbeat=%ums)",
      num_threads, heartbeat_ms);

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
      if (g_idle_workers == 0 && workers.size() < max_workers) {
        workers.emplace_back(WorkerThread);
      }
    }
    g_queue_cv.notify_one();
  }

  LOG_INFO("Shutting down forward daemon...");
  close(sockfd);
  {
    std::lock_guard<std::mutex> hb_lock(g_hb_mutex);
    g_hb_stop = true;
  }
  g_hb_cv.notify_all();
  if (hb_thread.joinable()) {
    hb_thread.join();
  }

  {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_shutdown = true;
  }
  g_queue_cv.notify_all();
  for (auto &t : workers) {
    t.join();
  }
  g_mesh_router.SaveToFile();
  LOG_INFO("Forward daemon stopped cleanly");
  return 0;
}

void PrintUsage(const char *prog) {
  printf(
      "Usage: %s [-p port] [-d dir] [-t threads] [-P pidfile] [-F] [-S] "
      "[-I heartbeat_ms] [-h]\n",
      prog);
  printf("  -p port         : specify port (default %u)\n", kDefaultPort);
  printf("  -d dir          : specify working directory (default .)\n");
  printf("  -t threads      : number of worker threads (default 4)\n");
  printf("  -P pidfile      : write process PID to pidfile\n");
  printf("  -F              : run in foreground (do not daemonize)\n");
  printf(
      "  -S              : always cache relayed files on intermediate hops\n");
  printf(
      "  -I heartbeat_ms : neighbor probe & route sync interval in ms "
      "(default %u, 0 to disable)\n",
      kDefaultHeartbeatMs);
  printf("  -h              : show this help\n");
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
  uint32_t heartbeat_ms = kDefaultHeartbeatMs;
  bool foreground = false;

  while ((opt = getopt(argc, argv, "p:d:t:P:I:FSh")) != -1) {
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
      case 'I': {
        char *endptr = NULL;
        long parsed_hb = strtol(optarg, &endptr, 10);
        if (optarg[0] == '\0' || *endptr != '\0' || parsed_hb < 0 ||
            parsed_hb > 3600000) {
          fprintf(stderr, "Invalid heartbeat interval ms: %s\n", optarg);
          return 1;
        }
        heartbeat_ms = static_cast<uint32_t>(parsed_hb);
        break;
      }
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

  g_mesh_router.Init(static_cast<uint32_t>(port), dir + "/mesh_routes.dat",
                     heartbeat_ms);
  g_mesh_router.LoadFromFile();

  if (!pidfile.empty() && !WritePidFile(pidfile)) {
    close(listen_fd);
    return 1;
  }

  LOG_INFO(
      "Forward daemon started (pid=%d, port=%d, dir=%s, foreground=%d, "
      "store_on_hop=%d, heartbeat_ms=%u)",
      static_cast<int>(getpid()), port, dir.c_str(), foreground ? 1 : 0,
      g_always_store_on_hop ? 1 : 0, heartbeat_ms);

  int rc = ForwardLoop(listen_fd, num_threads, heartbeat_ms);
  if (!pidfile.empty()) {
    unlink(pidfile.c_str());
  }
  return rc;
}
