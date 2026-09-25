// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "slf/builder.hpp"

#include "fixtures.hpp"
#include "slf_test.hpp"

using namespace slf;
using namespace slf::test;

namespace {

/// Builder preloaded with one leaf and one spine in generation 1.
struct Base {
  FabricBuilder builder{FabricId{1}, "base", TopologyGeneration{1}, Epoch{1}};
  Base() {
    (void)builder.add_leaf(leaf_record(LeafSpec{1, "", LeafRole::Tor, 1, NodeState::Active, 100000, 1},
                                       TopologyGeneration{1}));
    (void)builder.add_spine(spine_record(SpineSpec{1, "", SpineRole::FabricSpine, 1, NodeState::Active,
                                                   100000, 1},
                                         TopologyGeneration{1}));
  }
};

PortRecord make_port(std::uint32_t id, NodeKey owner, std::uint64_t speed = 100000) {
  PortRecord port;
  port.id = PortId{id};
  port.owner = owner;
  port.speed_mbps = speed;
  port.generation = TopologyGeneration{1};
  return port;
}

LinkRecord make_link(std::uint32_t id, NodeKey a, PortId pa, NodeKey b, PortId pb,
                     LinkClass cls = LinkClass::LeafSpine) {
  LinkRecord link;
  link.id = LinkId{id};
  link.a = a;
  link.port_a = pa;
  link.b = b;
  link.port_b = pb;
  link.cls = cls;
  link.generation = TopologyGeneration{1};
  return link;
}

}  // namespace

SLF_TEST(builder_duplicate_identities) {
  Base base;
  SLF_EXPECT_CODE(base.builder.add_leaf(leaf_record(LeafSpec{1, "dup", LeafRole::Tor, 1},
                                                    TopologyGeneration{1})),
                  StatusCode::DuplicateIdentity);
  SLF_EXPECT_CODE(base.builder.add_spine(spine_record(SpineSpec{1, "dup", SpineRole::FabricSpine, 1},
                                                      TopologyGeneration{1})),
                  StatusCode::DuplicateIdentity);
  SLF_EXPECT(base.builder.add_port(make_port(1, NodeKey{LeafId{1}})).ok());
  SLF_EXPECT_CODE(base.builder.add_port(make_port(1, NodeKey{SpineId{1}})), StatusCode::DuplicateIdentity);
  FailureDomainRecord domain;
  domain.id = FailureDomainId{1};
  domain.name = "d1";
  domain.generation = TopologyGeneration{1};
  SLF_EXPECT(base.builder.add_failure_domain(domain).ok());
  SLF_EXPECT_CODE(base.builder.add_failure_domain(domain), StatusCode::DuplicateIdentity);

  // Zero identities are reserved, never silently accepted.
  LeafRecord zero_leaf = leaf_record(LeafSpec{0, "", LeafRole::Tor, 1}, TopologyGeneration{1});
  SLF_EXPECT_CODE(base.builder.add_leaf(zero_leaf), StatusCode::Invalid);
}

SLF_TEST(builder_dangling_edges) {
  Base base;
  SLF_EXPECT(base.builder.add_port(make_port(1, NodeKey{LeafId{1}})).ok());
  SLF_EXPECT(base.builder.add_port(make_port(2, NodeKey{SpineId{1}})).ok());

  // Link to a node that does not exist.
  SLF_EXPECT_CODE(base.builder.add_link(make_link(1, NodeKey{LeafId{9}}, PortId{1}, NodeKey{SpineId{1}},
                                                  PortId{2})),
                  StatusCode::DanglingEdge);
  // Link whose port belongs to a different node.
  SLF_EXPECT_CODE(base.builder.add_link(make_link(2, NodeKey{LeafId{1}}, PortId{1}, NodeKey{SpineId{1}},
                                                  PortId{1})),
                  StatusCode::DanglingEdge);
  // Link referencing a port that does not exist.
  SLF_EXPECT_CODE(base.builder.add_link(make_link(3, NodeKey{LeafId{1}}, PortId{7}, NodeKey{SpineId{1}},
                                                  PortId{2})),
                  StatusCode::DanglingEdge);
  // Port owned by an unknown node.
  SLF_EXPECT_CODE(base.builder.add_port(make_port(3, NodeKey{SpineId{9}})), StatusCode::DanglingEdge);
  // Evidence for an unknown link.
  LinkEvidence evidence;
  evidence.link = LinkId{5};
  evidence.observed = LinkObservation::Up;
  evidence.generation = TopologyGeneration{1};
  evidence.observer = mint_incarnation();
  evidence.epoch = Epoch{1};
  evidence.observed_seq = SequenceNumber{1};
  SLF_EXPECT_CODE(base.builder.add_evidence(evidence), StatusCode::DanglingEdge);
}

SLF_TEST(builder_same_tier_adjacency_policy) {
  // Policy off: any same-tier adjacency is a structural error.
  {
    FabricBuilder builder(FabricId{1}, "policy-off", TopologyGeneration{1}, Epoch{1});
    SLF_EXPECT(builder.add_leaf(leaf_record(LeafSpec{1, "", LeafRole::BorderLeaf, 1}, TopologyGeneration{1}))
                   .ok());
    SLF_EXPECT(builder.add_leaf(leaf_record(LeafSpec{2, "", LeafRole::BorderLeaf, 2}, TopologyGeneration{1}))
                   .ok());
    SLF_EXPECT(builder.add_port(make_port(1, NodeKey{LeafId{1}})).ok());
    SLF_EXPECT(builder.add_port(make_port(2, NodeKey{LeafId{2}})).ok());
    SLF_EXPECT_CODE(builder.add_link(make_link(1, NodeKey{LeafId{1}}, PortId{1}, NodeKey{LeafId{2}}, PortId{2},
                                               LinkClass::SameTierPeer)),
                    StatusCode::IllegalAdjacency);
  }
  // Policy on, but the roles are not peer capable.
  {
    FabricBuilder builder(FabricId{1}, "policy-on-tor", TopologyGeneration{1}, Epoch{1},
                          FabricOptions{.allow_same_tier_adjacency = true});
    SLF_EXPECT(builder.add_leaf(leaf_record(LeafSpec{1, "", LeafRole::Tor, 1}, TopologyGeneration{1})).ok());
    SLF_EXPECT(builder.add_leaf(leaf_record(LeafSpec{2, "", LeafRole::Tor, 2}, TopologyGeneration{1})).ok());
    SLF_EXPECT(builder.add_port(make_port(1, NodeKey{LeafId{1}})).ok());
    SLF_EXPECT(builder.add_port(make_port(2, NodeKey{LeafId{2}})).ok());
    SLF_EXPECT_CODE(builder.add_link(make_link(1, NodeKey{LeafId{1}}, PortId{1}, NodeKey{LeafId{2}}, PortId{2},
                                               LinkClass::SameTierPeer)),
                    StatusCode::ContradictoryRole);
  }
  // Policy on and both endpoints peer capable: accepted and frozen.
  {
    FabricBuilder builder(FabricId{1}, "policy-on-border", TopologyGeneration{1}, Epoch{1},
                          FabricOptions{.allow_same_tier_adjacency = true});
    SLF_EXPECT(builder.add_leaf(leaf_record(LeafSpec{1, "", LeafRole::BorderLeaf, 1}, TopologyGeneration{1}))
                   .ok());
    SLF_EXPECT(builder.add_leaf(leaf_record(LeafSpec{2, "", LeafRole::BorderLeaf, 2}, TopologyGeneration{1}))
                   .ok());
    SLF_EXPECT(builder.add_port(make_port(1, NodeKey{LeafId{1}})).ok());
    SLF_EXPECT(builder.add_port(make_port(2, NodeKey{LeafId{2}})).ok());
    SLF_EXPECT(builder.add_link(make_link(1, NodeKey{LeafId{1}}, PortId{1}, NodeKey{LeafId{2}}, PortId{2},
                                          LinkClass::SameTierPeer))
                   .ok());
    const auto frozen = builder.freeze();
    SLF_EXPECT(frozen.ok());
    SLF_EXPECT_EQ(frozen.value().links.front().cls, LinkClass::SameTierPeer);
  }
}

SLF_TEST(builder_contradictions_and_self_loops) {
  Base base;
  SLF_EXPECT(base.builder.add_port(make_port(1, NodeKey{LeafId{1}})).ok());
  SLF_EXPECT(base.builder.add_port(make_port(2, NodeKey{SpineId{1}})).ok());
  // A leaf-to-spine pair cannot be classified as a peer link.
  SLF_EXPECT_CODE(base.builder.add_link(make_link(1, NodeKey{LeafId{1}}, PortId{1}, NodeKey{SpineId{1}},
                                                  PortId{2}, LinkClass::SameTierPeer)),
                  StatusCode::ContradictoryRole);
  // A self loop is never a fabric link.
  SLF_EXPECT_CODE(base.builder.add_link(make_link(2, NodeKey{LeafId{1}}, PortId{1}, NodeKey{LeafId{1}},
                                                  PortId{1}, LinkClass::SameTierPeer)),
                  StatusCode::IllegalAdjacency);
  // Declared link speed above the slower port is contradictory.
  LinkRecord fast = make_link(3, NodeKey{LeafId{1}}, PortId{1}, NodeKey{SpineId{1}}, PortId{2});
  fast.declared_speed_mbps = 200000;
  SLF_EXPECT_CODE(base.builder.add_link(fast), StatusCode::Conflicting);
  // A parallel link on the same ports is a duplicate adjacency.
  SLF_EXPECT(base.builder.add_link(make_link(4, NodeKey{LeafId{1}}, PortId{1}, NodeKey{SpineId{1}},
                                              PortId{2}))
                 .ok());
  SLF_EXPECT_CODE(base.builder.add_link(make_link(5, NodeKey{LeafId{1}}, PortId{1}, NodeKey{SpineId{1}},
                                                  PortId{2})),
                  StatusCode::AlreadyExists);
}

SLF_TEST(builder_generation_binding) {
  Base base;
  // A node stamped with a future generation is a cross-generation reference.
  SLF_EXPECT_CODE(base.builder.add_leaf(leaf_record(LeafSpec{2, "", LeafRole::Tor, 1},
                                                    TopologyGeneration{2})),
                  StatusCode::CrossGeneration);
  SLF_EXPECT_CODE(base.builder.add_spine(spine_record(SpineSpec{2, "", SpineRole::FabricSpine, 1},
                                                      TopologyGeneration{9})),
                  StatusCode::CrossGeneration);
  SLF_EXPECT(base.builder.add_port(make_port(1, NodeKey{LeafId{1}})).ok());
  PortRecord stale_port = make_port(2, NodeKey{SpineId{1}});
  stale_port.generation = TopologyGeneration{0};
  SLF_EXPECT_CODE(base.builder.add_port(stale_port), StatusCode::CrossGeneration);
  SLF_EXPECT(base.builder.add_port(make_port(3, NodeKey{SpineId{1}})).ok());
  LinkRecord stale_link = make_link(1, NodeKey{LeafId{1}}, PortId{1}, NodeKey{SpineId{1}}, PortId{3});
  stale_link.generation = TopologyGeneration{0};
  SLF_EXPECT_CODE(base.builder.add_link(stale_link), StatusCode::CrossGeneration);
  // Evidence from the future is refused as well.
  LinkEvidence future;
  future.link = LinkId{1};
  future.generation = TopologyGeneration{4};
  future.observer = mint_incarnation();
  future.observed_seq = SequenceNumber{1};
  SLF_EXPECT_CODE(base.builder.add_evidence(future), StatusCode::CrossGeneration);
}

SLF_TEST(builder_history_links) {
  FabricBuilder builder(FabricId{1}, "history", TopologyGeneration{3}, Epoch{1});
  LinkRecord retired = make_link(1, NodeKey{LeafId{1}}, PortId{1}, NodeKey{SpineId{1}}, PortId{2});
  retired.generation = TopologyGeneration{2};
  SLF_EXPECT(builder.add_history_link(retired).ok());
  // A historical link must belong to an older generation.
  LinkRecord current = retired;
  current.id = LinkId{2};
  current.generation = TopologyGeneration{3};
  SLF_EXPECT_CODE(builder.add_history_link(current), StatusCode::CrossGeneration);
  LinkRecord duplicate = retired;
  SLF_EXPECT_CODE(builder.add_history_link(duplicate), StatusCode::DuplicateIdentity);
}

SLF_TEST(builder_bounds_and_text) {
  BuilderLimits limits;
  limits.max_leaves = 1;
  limits.max_name_bytes = 8;
  limits.max_capacity_mbps = 1000;
  FabricBuilder builder(FabricId{1}, "bounds", TopologyGeneration{1}, Epoch{1}, FabricOptions{}, limits);
  const auto first = builder.add_leaf(leaf_record(LeafSpec{1, "", LeafRole::Tor, 1, NodeState::Active, 500, 1},
                                                  TopologyGeneration{1}));
  SLF_EXPECT(first.ok());
  SLF_EXPECT_CODE(builder.add_leaf(leaf_record(LeafSpec{2, "", LeafRole::Tor, 1}, TopologyGeneration{1})),
                  StatusCode::Exhausted);

  FabricBuilder names(FabricId{1}, "names", TopologyGeneration{1}, Epoch{1}, FabricOptions{}, limits);
  LeafRecord long_name = leaf_record(LeafSpec{1, "", LeafRole::Tor, 1}, TopologyGeneration{1});
  long_name.name = "a-very-long-name";
  SLF_EXPECT_CODE(names.add_leaf(long_name), StatusCode::Exhausted);
  LeafRecord invalid_utf8 = leaf_record(LeafSpec{1, "", LeafRole::Tor, 1}, TopologyGeneration{1});
  invalid_utf8.name = "bad\xFF";
  SLF_EXPECT_CODE(names.add_leaf(invalid_utf8), StatusCode::Invalid);
  LeafRecord empty_name = leaf_record(LeafSpec{1, "", LeafRole::Tor, 1}, TopologyGeneration{1});
  empty_name.name.clear();
  SLF_EXPECT_CODE(names.add_leaf(empty_name), StatusCode::Invalid);
  LeafRecord too_much = leaf_record(LeafSpec{1, "", LeafRole::Tor, 1, NodeState::Active, 5000, 1},
                                    TopologyGeneration{1});
  SLF_EXPECT_CODE(names.add_leaf(too_much), StatusCode::Overflow);
}

SLF_TEST(builder_freeze_report_and_errors) {
  FabricBuilder builder(FabricId{1}, "report", TopologyGeneration{1}, Epoch{1});
  FailureDomainRecord declared;
  declared.id = FailureDomainId{1};
  declared.name = "domain-1";
  declared.generation = TopologyGeneration{1};
  SLF_EXPECT(builder.add_failure_domain(declared).ok());
  // Failure domain 7 is not declared: a warning, not an error.
  SLF_EXPECT(builder.add_leaf(leaf_record(LeafSpec{1, "", LeafRole::Tor, 7}, TopologyGeneration{1})).ok());
  SLF_EXPECT(builder.add_spine(spine_record(SpineSpec{1, "", SpineRole::FabricSpine, 7},
                                            TopologyGeneration{1}))
                 .ok());
  SLF_EXPECT(builder.add_port(make_port(1, NodeKey{LeafId{1}})).ok());
  SLF_EXPECT(builder.add_port(make_port(2, NodeKey{SpineId{1}})).ok());
  SLF_EXPECT(builder.add_link(make_link(1, NodeKey{LeafId{1}}, PortId{1}, NodeKey{SpineId{1}}, PortId{2}))
                 .ok());
  const auto frozen = builder.freeze();
  SLF_EXPECT(frozen.ok());
  SLF_EXPECT_EQ(builder.report().leaf_count, 1U);
  SLF_EXPECT_EQ(builder.report().link_count, 1U);
  SLF_EXPECT_EQ(builder.report().error_count, 0U);
  SLF_EXPECT(builder.report().warning_count >= 2U);
  SLF_EXPECT(builder.report().has_code(StatusCode::NotFound));
  SLF_EXPECT(builder.report().summary().find("structurally valid") != std::string::npos);
  SLF_EXPECT(!builder.report().digest().is_zero());
  // A frozen builder refuses further work.
  SLF_EXPECT_CODE(builder.add_leaf(leaf_record(LeafSpec{2, "", LeafRole::Tor, 1}, TopologyGeneration{1})),
                  StatusCode::Closed);
  SLF_EXPECT_CODE(builder.freeze(), StatusCode::Closed);

  // An error-level issue refuses the freeze with that exact code.
  FabricBuilder failing(FabricId{1}, "failing", TopologyGeneration{1}, Epoch{1});
  SLF_EXPECT(failing.add_leaf(leaf_record(LeafSpec{1, "", LeafRole::Tor, 1}, TopologyGeneration{1})).ok());
}

SLF_TEST(builder_build_fabric_helper) {
  const auto fabric = make_grid(TopologyGeneration{1},
                                {{1, "", LeafRole::Tor, 1, NodeState::Active, 100000, 1},
                                 {2, "", LeafRole::Tor, 2, NodeState::Active, 100000, 2}},
                                {{1, "", SpineRole::FabricSpine, 1, NodeState::Active, 400000, 1}},
                                {{1, 1, 100000}, {2, 1, 100000}});
  SLF_EXPECT(fabric.ok());
  ValidationReport report;
  const auto via_helper = build_fabric(FabricId{1}, "test-fabric", TopologyGeneration{1}, Epoch{1},
                                       FabricOptions{},
                                       fabric.value().leaves, fabric.value().spines, fabric.value().ports,
                                       fabric.value().links, &report);
  SLF_EXPECT(via_helper.ok());
  SLF_EXPECT_EQ(report.leaf_count, 2U);
  SLF_EXPECT_EQ(via_helper.value().topology_digest, fabric.value().topology_digest);
  // The helper surfaces the first refusal.
  std::vector<PortRecord> broken_ports = fabric.value().ports;
  broken_ports.push_back(make_port(99, NodeKey{SpineId{9}}));
  const auto refused = build_fabric(FabricId{1}, "test-fabric", TopologyGeneration{1}, Epoch{1},
                                    FabricOptions{}, fabric.value().leaves, fabric.value().spines,
                                    broken_ports, fabric.value().links, &report);
  SLF_EXPECT_CODE(refused, StatusCode::DanglingEdge);
  SLF_EXPECT(!report.structural_ok());
}
