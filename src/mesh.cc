#include "mesh.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <sstream>

#include "logger.h"

namespace {
bool SameNode(const ForwardNode &a, const ForwardNode &b) {
  return a.ip == b.ip && a.port == b.port;
}

uint16_t SaturatingAddRtt(uint16_t a, uint16_t b) {
  uint32_t sum = static_cast<uint32_t>(a) + static_cast<uint32_t>(b);
  return static_cast<uint16_t>(
      std::min<uint32_t>(65535, std::max<uint32_t>(1, sum)));
}

bool SamePath(const std::vector<ForwardNode> &a,
              const std::vector<ForwardNode> &b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (!SameNode(a[i], b[i])) {
      return false;
    }
  }
  return true;
}

bool PathContainsNode(const std::vector<ForwardNode> &path,
                      const ForwardNode &target) {
  for (const auto &n : path) {
    if (SameNode(n, target)) {
      return true;
    }
  }
  return false;
}
}  // namespace

MeshRouter::MeshRouter() : listen_port_(kDefaultPort), heartbeat_ms_(3000) {
  self_ips_.insert(htonl(INADDR_LOOPBACK));
}

void MeshRouter::Init(uint32_t listen_port, const std::string &persist_path,
                      uint32_t heartbeat_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  listen_port_ = listen_port;
  persist_path_ = persist_path;
  heartbeat_ms_ = heartbeat_ms;
  self_ips_.clear();
  self_ips_.insert(htonl(INADDR_LOOPBACK));
  neighbors_.clear();
  routes_.clear();
}

uint64_t MeshRouter::NodeKey(const ForwardNode &node) {
  return (static_cast<uint64_t>(node.ip) << 32) |
         static_cast<uint64_t>(node.port);
}

void MeshRouter::RegisterSelfIp(uint32_t ip_nbo) {
  if (ip_nbo == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  self_ips_.insert(ip_nbo);
}

void MeshRouter::RegisterSelfFromSocket(int fd) {
  struct sockaddr_in local_addr;
  socklen_t alen = sizeof(local_addr);
  memset(&local_addr, 0, sizeof(local_addr));
  if (getsockname(fd, reinterpret_cast<struct sockaddr *>(&local_addr),
                  &alen) == 0 &&
      local_addr.sin_family == AF_INET && local_addr.sin_addr.s_addr != 0) {
    RegisterSelfIp(local_addr.sin_addr.s_addr);
  }
}

bool MeshRouter::IsSelf(const ForwardNode &node) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return IsSelfLocked(node);
}

bool MeshRouter::IsSelfLocked(const ForwardNode &node) const {
  return node.port == listen_port_ && self_ips_.count(node.ip) > 0;
}

bool MeshRouter::IsValidPathLocked(const std::vector<ForwardNode> &path) const {
  if (path.empty() || path.size() > kMaxForwardTTL) {
    return false;
  }
  std::set<uint64_t> seen;
  for (const auto &n : path) {
    if (n.port == 0 || n.port > 65535 || IsSelfLocked(n)) {
      return false;
    }
    uint64_t k = NodeKey(n);
    if (!seen.insert(k).second) {
      return false;
    }
  }
  return true;
}

bool MeshRouter::IsNeighborAliveLocked(const ForwardNode &hop) const {
  auto it = neighbors_.find(NodeKey(hop));
  return it != neighbors_.end() && it->second.alive;
}

bool MeshRouter::IsCandidateHealthyLocked(const RouteCandidate &cand) const {
  return cand.healthy && IsNeighborAliveLocked(cand.next_hop);
}

std::vector<ForwardNode> MeshRouter::ConcatAndDedupePath(
    const std::vector<ForwardNode> &prefix,
    const std::vector<ForwardNode> &suffix) {
  std::vector<ForwardNode> out;
  out.reserve(prefix.size() + suffix.size());
  for (const auto &n : prefix) {
    auto it = std::find_if(out.begin(), out.end(), [&](const ForwardNode &x) {
      return SameNode(x, n);
    });
    if (it != out.end()) {
      out.erase(it + 1, out.end());
    } else {
      out.push_back(n);
    }
  }
  for (const auto &n : suffix) {
    auto it = std::find_if(out.begin(), out.end(), [&](const ForwardNode &x) {
      return SameNode(x, n);
    });
    if (it != out.end()) {
      out.erase(it + 1, out.end());
    } else {
      out.push_back(n);
    }
  }
  return out;
}

std::vector<ForwardNode> MeshRouter::GetDirectNeighbors() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ForwardNode> res;
  res.reserve(neighbors_.size());
  for (const auto &kv : neighbors_) {
    res.push_back(kv.second.node);
  }
  return res;
}

bool MeshRouter::UpsertCandidateLocked(uint64_t target_key,
                                       const RouteCandidate &cand,
                                       bool allow_overwrite_authoritative) {
  auto &vec = routes_[target_key];
  for (auto &existing : vec) {
    if (SameNode(existing.next_hop, cand.next_hop)) {
      if (!allow_overwrite_authoritative && !existing.provisional &&
          cand.provisional) {
        return false;
      }
      bool topo_changed = !SamePath(existing.path, cand.path) ||
                          existing.provisional != cand.provisional;
      existing = cand;
      return topo_changed;
    }
  }
  vec.push_back(cand);
  return true;
}

void MeshRouter::RecordTransferSuccess(
    const ForwardNode &first_hop, uint16_t connect_rtt_ms,
    const std::vector<ForwardNode> &downstream_tail, bool authoritative) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (IsSelfLocked(first_hop) || first_hop.port == 0 ||
      first_hop.port > 65535) {
    return;
  }

  uint64_t now = SteadyNowMs();
  uint16_t rtt = std::max<uint16_t>(1, connect_rtt_ms);
  uint64_t hkey = NodeKey(first_hop);

  bool topo_changed = false;
  auto nit = neighbors_.find(hkey);
  if (nit == neighbors_.end()) {
    neighbors_[hkey] = NeighborState{first_hop, true, rtt, 0, now};
    topo_changed = true;
  } else {
    nit->second.alive = true;
    nit->second.rtt_ms = rtt;
    nit->second.fail_count = 0;
    nit->second.last_ok_ms = now;
  }

  RouteCandidate direct_cand{first_hop, {first_hop}, rtt, true, false, now};
  if (UpsertCandidateLocked(hkey, direct_cand, true)) {
    topo_changed = true;
  }

  std::vector<ForwardNode> full =
      ConcatAndDedupePath({first_hop}, downstream_tail);
  for (size_t i = 1; i < full.size(); ++i) {
    std::vector<ForwardNode> sub(full.begin(), full.begin() + i + 1);
    if (!IsValidPathLocked(sub)) {
      break;
    }
    RouteCandidate sub_cand{
        first_hop,
        sub,
        SaturatingAddRtt(rtt, static_cast<uint16_t>(i)),
        true,
        !authoritative,
        now,
    };
    if (UpsertCandidateLocked(NodeKey(sub.back()), sub_cand, authoritative)) {
      topo_changed = true;
    }
  }

  if (topo_changed && !persist_path_.empty()) {
    SaveToFileLocked();
  }
}

void MeshRouter::RecordConnectFailure(const ForwardNode &hop) {
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t hkey = NodeKey(hop);
  auto nit = neighbors_.find(hkey);
  if (nit != neighbors_.end()) {
    nit->second.alive = false;
    nit->second.fail_count += 1;
  }
  auto rit = routes_.find(hkey);
  if (rit != routes_.end()) {
    for (auto &c : rit->second) {
      if (SameNode(c.next_hop, hop)) {
        c.healthy = false;
      }
    }
  }
}

void MeshRouter::RecordDownstreamUnreachable(const ForwardNode &first_hop,
                                             const ForwardNode &final_target) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto rit = routes_.find(NodeKey(final_target));
  if (rit == routes_.end()) {
    return;
  }
  for (auto &c : rit->second) {
    if (SameNode(c.next_hop, first_hop)) {
      c.healthy = false;
    }
  }
}

void MeshRouter::ApplyNeighborSync(const ForwardNode &neighbor,
                                   uint16_t neighbor_rtt_ms,
                                   const std::vector<MeshSyncEntry> &entries) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (IsSelfLocked(neighbor) || neighbor.port == 0 || neighbor.port > 65535) {
    return;
  }

  uint64_t now = SteadyNowMs();
  uint16_t nrtt = std::max<uint16_t>(1, neighbor_rtt_ms);
  uint64_t nkey = NodeKey(neighbor);
  bool topo_changed = false;

  auto nit = neighbors_.find(nkey);
  if (nit == neighbors_.end()) {
    neighbors_[nkey] = NeighborState{neighbor, true, nrtt, 0, now};
    topo_changed = true;
  } else {
    nit->second.alive = true;
    nit->second.rtt_ms = nrtt;
    nit->second.fail_count = 0;
    nit->second.last_ok_ms = now;
  }

  RouteCandidate direct_cand{neighbor, {neighbor}, nrtt, true, false, now};
  if (UpsertCandidateLocked(nkey, direct_cand, true)) {
    topo_changed = true;
  }

  std::set<uint64_t> healthy_targets;
  std::set<uint64_t> looped_targets;
  healthy_targets.insert(nkey);

  for (const auto &e : entries) {
    uint64_t tkey = NodeKey(e.target);
    if (IsSelfLocked(e.target) || tkey == nkey) {
      continue;
    }
    if (e.path.empty() || !SameNode(e.path.back(), e.target)) {
      continue;
    }

    bool contains_self = false;
    for (const auto &hop : e.path) {
      if (IsSelfLocked(hop)) {
        contains_self = true;
        break;
      }
    }
    if (contains_self) {
      looped_targets.insert(tkey);
      continue;
    }

    std::vector<ForwardNode> full_path;
    full_path.reserve(1 + e.path.size());
    full_path.push_back(neighbor);
    full_path.insert(full_path.end(), e.path.begin(), e.path.end());
    if (!IsValidPathLocked(full_path)) {
      continue;
    }

    if (e.alive == 0) {
      continue;
    }

    RouteCandidate cand{
        neighbor,
        full_path,
        SaturatingAddRtt(nrtt, std::max<uint16_t>(1, e.rtt_ms)),
        true,
        false,
        now,
    };
    if (UpsertCandidateLocked(tkey, cand, true)) {
      topo_changed = true;
    }
    healthy_targets.insert(tkey);
  }

  uint64_t expire_ms =
      static_cast<uint64_t>(std::max<uint32_t>(heartbeat_ms_, 1000)) * 10ULL;

  for (auto it = routes_.begin(); it != routes_.end();) {
    uint64_t tkey = it->first;
    if (tkey != nkey) {
      auto &vec = it->second;
      for (auto cit = vec.begin(); cit != vec.end();) {
        if (!SameNode(cit->next_hop, neighbor)) {
          ++cit;
          continue;
        }
        if (looped_targets.count(tkey) > 0) {
          cit = vec.erase(cit);
          topo_changed = true;
          continue;
        }
        if (healthy_targets.count(tkey) == 0) {
          cit->healthy = false;
          if (now > cit->updated_ms && (now - cit->updated_ms) > expire_ms) {
            cit = vec.erase(cit);
            topo_changed = true;
            continue;
          }
        }
        ++cit;
      }
    }
    if (it->second.empty()) {
      it = routes_.erase(it);
    } else {
      ++it;
    }
  }

  if (topo_changed && !persist_path_.empty()) {
    SaveToFileLocked();
  }
}

std::vector<MeshSyncEntry> MeshRouter::ExportSyncSnapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<MeshSyncEntry> snapshot;
  snapshot.reserve(routes_.size());

  for (const auto &kv : routes_) {
    const auto &cands = kv.second;
    if (cands.empty()) {
      continue;
    }
    std::vector<RouteCandidate> sorted = cands;
    std::sort(sorted.begin(), sorted.end(),
              [&](const RouteCandidate &a, const RouteCandidate &b) {
                bool ha = IsCandidateHealthyLocked(a);
                bool hb = IsCandidateHealthyLocked(b);
                if (ha != hb) return ha > hb;
                if (a.provisional != b.provisional) {
                  return a.provisional < b.provisional;
                }
                if (a.path.size() != b.path.size()) {
                  return a.path.size() < b.path.size();
                }
                return a.rtt_ms < b.rtt_ms;
              });
    const RouteCandidate &best = sorted.front();
    if (best.path.empty()) {
      continue;
    }
    MeshSyncEntry e;
    e.target = best.path.back();
    e.rtt_ms = best.rtt_ms;
    e.alive = IsCandidateHealthyLocked(best) ? 1 : 0;
    e.path = best.path;
    snapshot.push_back(e);
    if (snapshot.size() >= kMaxMeshRoutes) {
      break;
    }
  }
  return snapshot;
}

std::vector<HopPlan> MeshRouter::BuildHopPlans(
    const std::vector<ForwardNode> &req_nodes, uint8_t hops_traversed,
    bool strict_mode, bool *out_should_sync_on_miss) const {
  if (out_should_sync_on_miss) {
    *out_should_sync_on_miss = false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (hops_traversed >= kMaxForwardTTL || req_nodes.empty()) {
    return {};
  }
  size_t budget = static_cast<size_t>(kMaxForwardTTL - (hops_traversed + 1));

  if (strict_mode) {
    if (IsSelfLocked(req_nodes[0]) || req_nodes.size() - 1 > budget) {
      return {};
    }
    HopPlan p;
    p.first_hop = req_nodes[0];
    p.remaining_tail.assign(req_nodes.begin() + 1, req_nodes.end());
    return {p};
  }

  // Collapse consecutive duplicates and strip any leading self entries
  std::vector<ForwardNode> clean_nodes;
  clean_nodes.reserve(req_nodes.size());
  for (const auto &n : req_nodes) {
    if (clean_nodes.empty() && IsSelfLocked(n)) {
      continue;
    }
    if (!clean_nodes.empty() && SameNode(clean_nodes.back(), n)) {
      continue;
    }
    clean_nodes.push_back(n);
  }
  if (clean_nodes.empty() || IsSelfLocked(clean_nodes.back())) {
    return {};
  }

  std::vector<HopPlan> plans;
  auto add_plan = [&](const std::vector<ForwardNode> &full_chain) {
    if (full_chain.empty() || IsSelfLocked(full_chain[0])) {
      return;
    }
    for (const auto &n : full_chain) {
      if (IsSelfLocked(n)) {
        return;
      }
    }
    size_t tail_len = full_chain.size() - 1;
    if (tail_len > budget) {
      return;
    }
    HopPlan hp;
    hp.first_hop = full_chain[0];
    hp.remaining_tail.assign(full_chain.begin() + 1, full_chain.end());
    for (const auto &existing : plans) {
      if (SameNode(existing.first_hop, hp.first_hop) &&
          SamePath(existing.remaining_tail, hp.remaining_tail)) {
        return;
      }
    }
    plans.push_back(hp);
  };

  const ForwardNode &n0 = clean_nodes[0];
  std::vector<ForwardNode> tail_after_n0(clean_nodes.begin() + 1,
                                         clean_nodes.end());
  uint64_t n0_key = NodeKey(n0);

  // 1a. Healthy direct neighbor n0
  if (IsNeighborAliveLocked(n0)) {
    add_plan(ConcatAndDedupePath({n0}, tail_after_n0));
  }

  // 1b. Healthy multi-hop candidates to n0
  auto r_n0 = routes_.find(n0_key);
  if (r_n0 != routes_.end()) {
    std::vector<RouteCandidate> multi;
    for (const auto &c : r_n0->second) {
      if (!SameNode(c.next_hop, n0) && IsCandidateHealthyLocked(c)) {
        multi.push_back(c);
      }
    }
    std::sort(multi.begin(), multi.end(),
              [](const RouteCandidate &a, const RouteCandidate &b) {
                if (a.provisional != b.provisional) {
                  return a.provisional < b.provisional;
                }
                if (a.path.size() != b.path.size()) {
                  return a.path.size() < b.path.size();
                }
                return a.rtt_ms < b.rtt_ms;
              });
    for (const auto &c : multi) {
      add_plan(ConcatAndDedupePath(c.path, tail_after_n0));
    }
  }

  // 1c. Completely unknown n0 -> direct attempt + signal on-demand neighbor
  // sync
  bool n0_known = (neighbors_.count(n0_key) > 0) ||
                  (r_n0 != routes_.end() && !r_n0->second.empty());
  if (!n0_known) {
    add_plan(ConcatAndDedupePath({n0}, tail_after_n0));
    if (!neighbors_.empty() && out_should_sync_on_miss) {
      *out_should_sync_on_miss = true;
    }
  }

  // 2. Waypoint bypass around failed/unreachable intermediate n0 (when k >= 2)
  if (clean_nodes.size() >= 2) {
    for (size_t idx = 1; idx < clean_nodes.size(); ++idx) {
      const ForwardNode &wp = clean_nodes[idx];
      std::vector<ForwardNode> tail_after_wp(clean_nodes.begin() + idx + 1,
                                             clean_nodes.end());
      if (!SameNode(wp, n0) && IsNeighborAliveLocked(wp)) {
        add_plan(ConcatAndDedupePath({wp}, tail_after_wp));
      }
      auto r_wp = routes_.find(NodeKey(wp));
      if (r_wp != routes_.end()) {
        std::vector<RouteCandidate> bypass_cands;
        for (const auto &c : r_wp->second) {
          if (IsCandidateHealthyLocked(c) && !PathContainsNode(c.path, n0)) {
            bypass_cands.push_back(c);
          }
        }
        std::sort(bypass_cands.begin(), bypass_cands.end(),
                  [](const RouteCandidate &a, const RouteCandidate &b) {
                    if (a.provisional != b.provisional) {
                      return a.provisional < b.provisional;
                    }
                    if (a.path.size() != b.path.size()) {
                      return a.path.size() < b.path.size();
                    }
                    return a.rtt_ms < b.rtt_ms;
                  });
        for (const auto &c : bypass_cands) {
          add_plan(ConcatAndDedupePath(c.path, tail_after_wp));
        }
      }
    }
  }

  // 1d. Degraded fallback reconnect (supports recovery before next heartbeat)
  auto nit = neighbors_.find(n0_key);
  if (nit != neighbors_.end() && !nit->second.alive) {
    add_plan(ConcatAndDedupePath({n0}, tail_after_n0));
  }
  if (r_n0 != routes_.end()) {
    std::vector<RouteCandidate> degraded;
    for (const auto &c : r_n0->second) {
      if (!SameNode(c.next_hop, n0) && !IsCandidateHealthyLocked(c)) {
        degraded.push_back(c);
      }
    }
    std::sort(degraded.begin(), degraded.end(),
              [](const RouteCandidate &a, const RouteCandidate &b) {
                if (a.path.size() != b.path.size()) {
                  return a.path.size() < b.path.size();
                }
                return a.rtt_ms < b.rtt_ms;
              });
    for (const auto &c : degraded) {
      add_plan(ConcatAndDedupePath(c.path, tail_after_n0));
    }
  }

  return plans;
}

bool MeshRouter::SaveToFile() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return SaveToFileLocked();
}

bool MeshRouter::SaveToFileLocked() const {
  if (persist_path_.empty()) {
    return false;
  }
  std::string tmp_tmpl = persist_path_ + ".tmp.XXXXXX";
  std::vector<char> tmp_buf(tmp_tmpl.begin(), tmp_tmpl.end());
  tmp_buf.push_back('\0');
  int fd = mkstemp(tmp_buf.data());
  if (fd < 0) {
    LOG_WARNING("[MESH]mkstemp(%s) failed, %d", tmp_buf.data(), errno);
    return false;
  }
  fchmod(fd, kDefaultFileMode);

  std::ostringstream oss;
  oss << "# ForwardMesh Route Cache v1\n";
  for (const auto &kv : neighbors_) {
    oss << "NEIGHBOR " << FormatNodeAddr(kv.second.node) << " "
        << kv.second.rtt_ms << "\n";
  }
  for (const auto &kv : routes_) {
    for (const auto &c : kv.second) {
      if (c.path.size() < 2) {
        continue;
      }
      oss << "ROUTE " << FormatNodeAddr(c.path.back()) << " " << c.rtt_ms
          << " ";
      for (size_t i = 0; i < c.path.size(); ++i) {
        if (i > 0) oss << ",";
        oss << FormatNodeAddr(c.path[i]);
      }
      oss << "\n";
    }
  }

  std::string content = oss.str();
  bool ok = (SendSync(fd, content.data(), content.size()) == 0);
  close(fd);
  if (!ok) {
    unlink(tmp_buf.data());
    return false;
  }
  if (rename(tmp_buf.data(), persist_path_.c_str()) != 0) {
    unlink(tmp_buf.data());
    return false;
  }
  return true;
}

bool MeshRouter::LoadFromFile() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (persist_path_.empty()) {
    return false;
  }
  std::ifstream ifs(persist_path_.c_str());
  if (!ifs.is_open()) {
    return false;
  }

  uint64_t now = SteadyNowMs();
  std::string line;
  while (std::getline(ifs, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::istringstream iss(line);
    std::string tag;
    if (!(iss >> tag)) {
      continue;
    }
    if (tag == "NEIGHBOR") {
      std::string addr_str;
      uint32_t rtt = 1;
      if (!(iss >> addr_str >> rtt)) {
        continue;
      }
      ForwardNode n;
      if (!ParseNodeAddr(addr_str, &n) || IsSelfLocked(n)) {
        continue;
      }
      uint16_t clamped_rtt = static_cast<uint16_t>(
          std::min<uint32_t>(65535, std::max<uint32_t>(1, rtt)));
      uint64_t k = NodeKey(n);
      neighbors_[k] = NeighborState{n, true, clamped_rtt, 0, now};
      RouteCandidate dc{n, {n}, clamped_rtt, true, false, now};
      UpsertCandidateLocked(k, dc, true);
    } else if (tag == "ROUTE") {
      std::string target_str;
      uint32_t rtt = 1;
      std::string chain_str;
      if (!(iss >> target_str >> rtt >> chain_str)) {
        continue;
      }
      ForwardNode target;
      if (!ParseNodeAddr(target_str, &target) || IsSelfLocked(target)) {
        continue;
      }
      std::vector<ForwardNode> path;
      size_t pos = 0;
      bool parse_ok = true;
      while (pos < chain_str.size()) {
        size_t comma = chain_str.find(',', pos);
        std::string hop_str = (comma == std::string::npos)
                                  ? chain_str.substr(pos)
                                  : chain_str.substr(pos, comma - pos);
        ForwardNode hop;
        if (!ParseNodeAddr(hop_str, &hop)) {
          parse_ok = false;
          break;
        }
        path.push_back(hop);
        if (path.size() > kMaxForwardTTL) {
          parse_ok = false;
          break;
        }
        if (comma == std::string::npos) {
          break;
        }
        pos = comma + 1;
      }
      if (!parse_ok || !IsValidPathLocked(path) ||
          !SameNode(path.back(), target)) {
        continue;
      }
      uint16_t clamped_rtt = static_cast<uint16_t>(
          std::min<uint32_t>(65535, std::max<uint32_t>(1, rtt)));
      uint64_t fk = NodeKey(path[0]);
      if (neighbors_.find(fk) == neighbors_.end()) {
        neighbors_[fk] = NeighborState{path[0], true, 1, 0, now};
        RouteCandidate dc{path[0], {path[0]}, 1, true, false, now};
        UpsertCandidateLocked(fk, dc, true);
      }
      RouteCandidate rc{path[0], path, clamped_rtt, true, false, now};
      UpsertCandidateLocked(NodeKey(target), rc, true);
    }
  }
  return true;
}
