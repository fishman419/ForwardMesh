#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fstream>
#include <string>
#include <vector>

#include "mesh.h"
#include "protocol.h"
#include "util.h"

namespace {
ForwardNode MakeNode(const char *ip_str, uint32_t port) {
  ForwardNode n;
  uint32_t ip = 0;
  int rc = inet_pton(AF_INET, ip_str, &ip);
  assert(rc == 1);
  (void)rc;
  n.ip = ip;
  n.port = port;
  return n;
}

bool SameNode(const ForwardNode &a, const ForwardNode &b) {
  return a.ip == b.ip && a.port == b.port;
}

void TestRouteRankingOrder() {
  printf("[UT-01] TestRouteRankingOrder...\n");
  MeshRouter router;
  router.Init(40001, "", 3000);

  ForwardNode n2 = MakeNode("127.0.0.1", 40002);
  ForwardNode n3 = MakeNode("127.0.0.1", 40003);
  ForwardNode n4 = MakeNode("127.0.0.1", 40004);
  ForwardNode n5 = MakeNode("127.0.0.1", 40005);
  ForwardNode n6 = MakeNode("127.0.0.1", 40006);

  // Provisional 2-hop [n2, n4] vs authoritative 3-hop [n3, n5, n4]
  router.RecordTransferSuccess(n2, 5, {n4}, /*authoritative=*/false);
  router.RecordTransferSuccess(n3, 10, {n5, n4}, /*authoritative=*/true);

  bool sync_miss = false;
  std::vector<HopPlan> plans = router.BuildHopPlans({n4}, 0, false, &sync_miss);
  assert(!plans.empty());
  // Authoritative [n3, n5, n4] beats provisional [n2, n4]
  assert(SameNode(plans[0].first_hop, n3));
  assert(plans[0].remaining_tail.size() == 2);

  // Promote [n2, n4] to authoritative with RTT 20+5=25ms via sync
  MeshSyncEntry e_n2_n4;
  e_n2_n4.target = n4;
  e_n2_n4.rtt_ms = 20;
  e_n2_n4.alive = 1;
  e_n2_n4.path = {n4};
  router.ApplyNeighborSync(n2, 5, {e_n2_n4});

  // Add authoritative 2-hop [n6, n4] with lower total RTT 2+3=5ms
  MeshSyncEntry e_n6_n4;
  e_n6_n4.target = n4;
  e_n6_n4.rtt_ms = 3;
  e_n6_n4.alive = 1;
  e_n6_n4.path = {n4};
  router.ApplyNeighborSync(n6, 2, {e_n6_n4});

  plans = router.BuildHopPlans({n4}, 0, false, &sync_miss);
  assert(plans.size() >= 3);
  // 2-hop 5ms (n6) -> 2-hop 25ms (n2) -> 3-hop (n3)
  assert(SameNode(plans[0].first_hop, n6));
  assert(SameNode(plans[1].first_hop, n2));
  assert(SameNode(plans[2].first_hop, n3));

  // Mark n6 connect failure -> n2 immediately becomes primary
  router.RecordConnectFailure(n6);
  plans = router.BuildHopPlans({n4}, 0, false, &sync_miss);
  assert(!plans.empty());
  assert(SameNode(plans[0].first_hop, n2));
  printf("  PASS\n");
}

void TestLoopPreventionAndSelfEviction() {
  printf("[UT-02] TestLoopPreventionAndSelfEviction...\n");
  MeshRouter router;
  router.Init(40001, "", 3000);

  ForwardNode self_lo = MakeNode("127.0.0.1", 40001);
  ForwardNode self_nic2 = MakeNode("192.168.50.1", 40001);
  router.RegisterSelfIp(self_nic2.ip);
  assert(router.IsSelf(self_lo));
  assert(router.IsSelf(self_nic2));

  ForwardNode n0 = MakeNode("10.0.0.9", 40000);
  ForwardNode n2 = MakeNode("10.0.1.2", 40002);
  ForwardNode n3 = MakeNode("10.0.2.3", 40003);
  ForwardNode n4 = MakeNode("10.0.3.4", 40004);
  ForwardNode n5 = MakeNode("10.0.4.5", 40005);

  // Cycle splicing: [n2, n3, n0] + [n3, n5] -> [n2, n3, n5]
  std::vector<ForwardNode> merged =
      MeshRouter::ConcatAndDedupePath({n2, n3, n0}, {n3, n5});
  assert(merged.size() == 3);
  assert(SameNode(merged[0], n2));
  assert(SameNode(merged[1], n3));
  assert(SameNode(merged[2], n5));

  // Duplicate waypoints in user input collapsed: [n2, n2, n3]
  router.RecordTransferSuccess(n2, 1, {}, true);
  bool sync_miss = false;
  std::vector<HopPlan> dup_plans =
      router.BuildHopPlans({n2, n2, n3}, 0, false, &sync_miss);
  assert(!dup_plans.empty());
  assert(SameNode(dup_plans[0].first_hop, n2));
  assert(dup_plans[0].remaining_tail.size() == 1);
  assert(SameNode(dup_plans[0].remaining_tail[0], n3));

  // Self-targeting rejected
  std::vector<HopPlan> self_plans =
      router.BuildHopPlans({self_nic2}, 0, false, &sync_miss);
  assert(self_plans.empty());

  // Seed provisional route [n2, n4], then n2 syncs [self_nic2, n4] -> must
  // evict!
  router.RecordTransferSuccess(n2, 2, {n4}, /*authoritative=*/false);
  assert(!router.BuildHopPlans({n4}, 0, false, &sync_miss).empty());

  MeshSyncEntry loop_entry;
  loop_entry.target = n4;
  loop_entry.rtt_ms = 5;
  loop_entry.alive = 1;
  loop_entry.path = {self_nic2, n4};

  MeshSyncEntry dup_hop_entry;
  dup_hop_entry.target = n5;
  dup_hop_entry.rtt_ms = 5;
  dup_hop_entry.alive = 1;
  dup_hop_entry.path = {n3, n3, n5};

  router.ApplyNeighborSync(n2, 2, {loop_entry, dup_hop_entry});

  // Ensure neither looped n4 nor duplicate-hop n5 exists in exported snapshot
  std::vector<MeshSyncEntry> snap = router.ExportSyncSnapshot();
  for (const auto &e : snap) {
    assert(!SameNode(e.target, n4));
    assert(!SameNode(e.target, n5));
  }
  printf("  PASS\n");
}

void TestFlagsBitfieldAndHopBudget() {
  printf("[UT-03] TestFlagsBitfieldAndHopBudget...\n");
  uint8_t flags = MakePacketFlags(kFlagStoreOnHop | kFlagStrictRoute, 17);
  assert((flags & kFlagStoreOnHop) != 0);
  assert((flags & kFlagStrictRoute) != 0);
  assert(GetHopsTraversed(flags) == 17);

  MeshRouter router;
  router.Init(40001, "", 3000);
  ForwardNode n2 = MakeNode("127.0.0.1", 40002);
  ForwardNode n3 = MakeNode("127.0.0.1", 40003);
  ForwardNode n4 = MakeNode("127.0.0.1", 40004);
  router.RecordTransferSuccess(n2, 2, {n3, n4}, true);

  bool sync_miss = false;
  // 3-hop route [n2, n3, n4]: this hop uses 1 + remaining_tail(2) = 3 hops.
  // hops_traversed = 29 -> 29 + 3 = 32 <= kMaxForwardTTL(32): allowed
  assert(!router.BuildHopPlans({n4}, 29, false, &sync_miss).empty());
  // hops_traversed = 30 -> 30 + 3 = 33 > 32: rejected!
  assert(router.BuildHopPlans({n4}, 30, false, &sync_miss).empty());
  // hops_traversed = 32 -> rejected even for 1-hop direct neighbor
  assert(router.BuildHopPlans({n2}, 32, false, &sync_miss).empty());
  printf("  PASS\n");
}

void TestSyncCodecAndRemoteDegradation() {
  printf("[UT-04] TestSyncCodecAndRemoteDegradation...\n");
  // Empty codec round-trip
  std::vector<uint8_t> empty_buf;
  EncodeMeshSyncPayload({}, &empty_buf);
  assert(empty_buf.size() == sizeof(uint32_t));
  std::vector<MeshSyncEntry> decoded_empty;
  assert(DecodeMeshSyncPayload(empty_buf.data(), empty_buf.size(),
                               &decoded_empty) == 0);
  assert(decoded_empty.empty());
  // Truncated buffer rejected
  assert(DecodeMeshSyncPayload(empty_buf.data(), 2, &decoded_empty) < 0);

  ForwardNode n2a = MakeNode("10.0.0.2", 40002);
  ForwardNode n2b = MakeNode("10.0.0.3", 40003);
  ForwardNode n3 = MakeNode("192.168.1.3", 40004);

  MeshSyncEntry item;
  item.target = n3;
  item.rtt_ms = 12;
  item.alive = 1;
  item.path = {n2a, n3};

  std::vector<uint8_t> wire_buf;
  EncodeMeshSyncPayload({item}, &wire_buf);
  std::vector<MeshSyncEntry> decoded;
  assert(DecodeMeshSyncPayload(wire_buf.data(), wire_buf.size(), &decoded) ==
         0);
  assert(decoded.size() == 1);
  assert(SameNode(decoded[0].target, n3));
  assert(decoded[0].rtt_ms == 12);
  assert(decoded[0].alive == 1);
  assert(decoded[0].path.size() == 2);
  assert(SameNode(decoded[0].path[0], n2a));
  assert(SameNode(decoded[0].path[1], n3));

  // Remote degradation test: N1 -> {N2A (fast), N2B (slower)} -> N3
  MeshRouter router;
  router.Init(40001, "", 3000);
  MeshSyncEntry to_n3_fast{n3, 3, 1, {n3}};
  MeshSyncEntry to_n3_slow{n3, 20, 1, {n3}};
  router.ApplyNeighborSync(n2a, 2, {to_n3_fast});
  router.ApplyNeighborSync(n2b, 5, {to_n3_slow});

  bool sync_miss = false;
  std::vector<HopPlan> plans = router.BuildHopPlans({n3}, 0, false, &sync_miss);
  assert(plans.size() >= 2);
  assert(SameNode(plans[0].first_hop, n2a));

  // Now N2A reports N3 alive=0 (downstream link N2A->N3 broke while N2A is up)
  MeshSyncEntry to_n3_dead{n3, 3, 0, {n3}};
  router.ApplyNeighborSync(n2a, 2, {to_n3_dead});
  plans = router.BuildHopPlans({n3}, 0, false, &sync_miss);
  assert(!plans.empty());
  assert(SameNode(plans[0].first_hop, n2b));

  // Downstream unreachable on N2B also degrades N2B->N3; single/degraded
  // fallback still remembers N3 is multi-hop (sync_miss == false)
  router.RecordDownstreamUnreachable(n2b, n3);
  plans = router.BuildHopPlans({n3}, 0, false, &sync_miss);
  assert(!sync_miss);
  assert(!plans.empty());
  printf("  PASS\n");
}

void TestPersistenceCorruptLinesAndBypass() {
  printf("[UT-05] TestPersistenceCorruptLinesAndBypass...\n");
  char tmp_dir_tmpl[] = "/tmp/fwd_ut_XXXXXX";
  char *tmp_dir = mkdtemp(tmp_dir_tmpl);
  assert(tmp_dir != NULL);
  std::string route_file = std::string(tmp_dir) + "/mesh_routes.dat";

  ForwardNode n2a = MakeNode("127.0.0.1", 40002);
  ForwardNode n2b = MakeNode("127.0.0.1", 40003);
  ForwardNode n3 = MakeNode("127.0.0.1", 40004);

  {
    MeshRouter r1;
    r1.Init(40001, route_file, 3000);
    r1.RecordTransferSuccess(n2a, 4, {n3}, true);
    r1.RecordTransferSuccess(n2b, 8, {n3}, true);
    assert(r1.SaveToFile());
  }

  // Append corrupt lines to verify resilient parser
  {
    std::ofstream ofs(route_file, std::ios::app);
    ofs << "\n";
    ofs << "UNKNOWN_TAG 1 2 3\n";
    ofs << "NEIGHBOR 999.999.1.1:40002 5\n";
    ofs << "NEIGHBOR 127.0.0.1:99999 5\n";
    ofs << "ROUTE 127.0.0.1:40009 10 127.0.0.1:40001\n";  // self-loop
    std::string huge_chain = "127.0.0.1:40100";
    for (int i = 1; i <= 35; ++i) {
      huge_chain += ",127.0.0.1:" + std::to_string(40100 + i);
    }
    ofs << "ROUTE 127.0.0.1:40135 10 " << huge_chain << "\n";
  }

  MeshRouter r2;
  r2.Init(40001, route_file, 3000);
  assert(r2.LoadFromFile());

  bool sync_miss = false;
  std::vector<HopPlan> plans = r2.BuildHopPlans({n3}, 0, false, &sync_miss);
  assert(plans.size() >= 2);
  assert(SameNode(plans[0].first_hop, n2a));

  // Mark explicit waypoint n2a dead and pass explicit chain [n2a, n3]:
  // Auto mode bypasses dead n2a via n2b; Strict mode only tries n2a!
  r2.RecordConnectFailure(n2a);
  std::vector<HopPlan> auto_bypass =
      r2.BuildHopPlans({n2a, n3}, 0, /*strict_mode=*/false, &sync_miss);
  assert(!auto_bypass.empty());
  assert(SameNode(auto_bypass[0].first_hop, n2b));
  assert(auto_bypass[0].remaining_tail.size() == 1);
  assert(SameNode(auto_bypass[0].remaining_tail[0], n3));

  std::vector<HopPlan> strict_plans =
      r2.BuildHopPlans({n2a, n3}, 0, /*strict_mode=*/true, &sync_miss);
  assert(strict_plans.size() == 1);
  assert(SameNode(strict_plans[0].first_hop, n2a));

  unlink(route_file.c_str());
  rmdir(tmp_dir);
  printf("  PASS\n");
}

void TestSelfDiscoveryZeroIpAndDegradedRecovery() {
  printf("[UT-06] TestSelfDiscoveryZeroIpAndDegradedRecovery...\n");
  ForwardNode parsed;
  assert(!ParseNodeAddr("0.0.0.0:40001", &parsed));
  assert(!ParseNodeAddr("255.255.255.255:40001", &parsed));

  MeshRouter router;
  router.Init(40001, "", 3000);
  assert(router.IsSelf(MakeNode("127.0.0.2", 40001)));
  ForwardNode zero_node{0, 40001};
  assert(router.IsSelf(zero_node));

  // Degraded authoritative route must recover healthy=true after default
  // (non-strict, authoritative=false) successful transfer
  ForwardNode n2a = MakeNode("10.10.0.2", 40002);
  ForwardNode n3 = MakeNode("10.20.0.3", 40003);
  MeshSyncEntry up_entry{n3, 5, 1, {n3}};
  router.ApplyNeighborSync(n2a, 2, {up_entry});

  MeshSyncEntry down_entry{n3, 5, 0, {n3}};
  router.ApplyNeighborSync(n2a, 2, {down_entry});

  std::vector<MeshSyncEntry> snap_down = router.ExportSyncSnapshot();
  bool found_down = false;
  for (const auto &e : snap_down) {
    if (SameNode(e.target, n3)) {
      assert(e.alive == 0);
      found_down = true;
    }
  }
  assert(found_down);

  // Default transfer succeeds -> must restore alive == 1!
  router.RecordTransferSuccess(n2a, 2, {n3}, /*authoritative=*/false);
  std::vector<MeshSyncEntry> snap_up = router.ExportSyncSnapshot();
  bool found_up = false;
  for (const auto &e : snap_up) {
    if (SameNode(e.target, n3)) {
      assert(e.alive == 1);
      found_up = true;
    }
  }
  assert(found_up);
  printf("  PASS\n");
}
}  // namespace

int main() {
  printf("=== ForwardMesh Unit Tests ===\n");
  TestRouteRankingOrder();
  TestLoopPreventionAndSelfEviction();
  TestFlagsBitfieldAndHopBudget();
  TestSyncCodecAndRemoteDegradation();
  TestPersistenceCorruptLinesAndBypass();
  TestSelfDiscoveryZeroIpAndDegradedRecovery();
  printf("=== All unit tests passed ===\n");
  return 0;
}
