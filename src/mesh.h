#ifndef MESH_H_
#define MESH_H_

#include <stdint.h>

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "protocol.h"
#include "util.h"

struct HopPlan {
  ForwardNode first_hop;
  std::vector<ForwardNode> remaining_tail;
};

class MeshRouter {
 public:
  MeshRouter();

  void Init(uint32_t listen_port, const std::string &persist_path,
            uint32_t heartbeat_ms);

  void RegisterSelfIp(uint32_t ip_nbo);
  void RegisterSelfFromSocket(int fd);
  bool IsSelf(const ForwardNode &node) const;

  std::vector<ForwardNode> GetDirectNeighbors() const;

  void RecordTransferSuccess(const ForwardNode &first_hop,
                             uint16_t connect_rtt_ms,
                             const std::vector<ForwardNode> &downstream_tail,
                             bool authoritative);
  void RecordConnectFailure(const ForwardNode &hop);
  void RecordDownstreamUnreachable(const ForwardNode &first_hop,
                                   const ForwardNode &final_target);

  void ApplyNeighborSync(const ForwardNode &neighbor, uint16_t neighbor_rtt_ms,
                         const std::vector<MeshSyncEntry> &entries);
  std::vector<MeshSyncEntry> ExportSyncSnapshot() const;

  std::vector<HopPlan> BuildHopPlans(const std::vector<ForwardNode> &req_nodes,
                                     uint8_t hops_traversed, bool strict_mode,
                                     bool *out_should_sync_on_miss) const;

  bool SaveToFile() const;
  bool LoadFromFile();

  static std::vector<ForwardNode> ConcatAndDedupePath(
      const std::vector<ForwardNode> &prefix,
      const std::vector<ForwardNode> &suffix);

 private:
  struct NeighborState {
    ForwardNode node;
    bool alive;
    uint16_t rtt_ms;
    uint32_t fail_count;
    uint64_t last_ok_ms;
  };

  struct RouteCandidate {
    ForwardNode next_hop;
    std::vector<ForwardNode> path;
    uint16_t rtt_ms;
    bool healthy;
    bool provisional;
    uint64_t updated_ms;
  };

  static uint64_t NodeKey(const ForwardNode &node);
  bool IsSelfLocked(const ForwardNode &node) const;
  bool IsValidPathLocked(const std::vector<ForwardNode> &path) const;
  bool IsNeighborAliveLocked(const ForwardNode &hop) const;
  bool IsCandidateHealthyLocked(const RouteCandidate &cand) const;
  bool UpsertCandidateLocked(uint64_t target_key, const RouteCandidate &cand,
                             bool allow_overwrite_authoritative);
  bool SaveToFileLocked() const;

  mutable std::mutex mutex_;
  uint32_t listen_port_;
  std::string persist_path_;
  uint32_t heartbeat_ms_;
  std::set<uint32_t> self_ips_;
  std::map<uint64_t, NeighborState> neighbors_;
  std::map<uint64_t, std::vector<RouteCandidate>> routes_;
};

#endif  // MESH_H_
