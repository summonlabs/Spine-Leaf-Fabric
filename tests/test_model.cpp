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

#include "slf/model.hpp"

#include "fixtures.hpp"
#include "slf_test.hpp"

using namespace slf;
using namespace slf::test;

SLF_TEST(model_enums_roundtrip) {
  for (const LeafRole role : {LeafRole::Access, LeafRole::Tor, LeafRole::BorderLeaf, LeafRole::ServiceLeaf}) {
    SLF_EXPECT_EQ(parse_leaf_role(to_string(role)), std::optional<LeafRole>(role));
  }
  for (const SpineRole role : {SpineRole::FabricSpine, SpineRole::SuperSpine}) {
    SLF_EXPECT_EQ(parse_spine_role(to_string(role)), std::optional<SpineRole>(role));
  }
  for (const NodeState state : {NodeState::Active, NodeState::Maintenance, NodeState::Draining,
                                NodeState::Failed, NodeState::Retired}) {
    SLF_EXPECT_EQ(parse_node_state(to_string(state)), std::optional<NodeState>(state));
  }
  for (const FabricState state : {FabricState::Unformed, FabricState::Forming, FabricState::Operational,
                                  FabricState::Draining, FabricState::Retired}) {
    SLF_EXPECT_EQ(parse_fabric_state(to_string(state)), std::optional<FabricState>(state));
  }
  for (const EvidencePolicy policy : {EvidencePolicy::ConfigOnly, EvidencePolicy::RequireFresh}) {
    SLF_EXPECT_EQ(parse_evidence_policy(to_string(policy)), std::optional<EvidencePolicy>(policy));
  }
  for (const LinkObservation observation : {LinkObservation::Up, LinkObservation::Down,
                                            LinkObservation::Unknown}) {
    SLF_EXPECT_EQ(parse_link_observation(to_string(observation)), std::optional<LinkObservation>(observation));
  }
  SLF_EXPECT(!parse_leaf_role("fabric").has_value());
  SLF_EXPECT(!parse_node_state("bogus").has_value());
  SLF_EXPECT(is_peer_capable(LeafRole::BorderLeaf));
  SLF_EXPECT(!is_peer_capable(LeafRole::Tor));
  SLF_EXPECT(state_is_serving(NodeState::Active));
  SLF_EXPECT(!state_is_serving(NodeState::Draining));
}

SLF_TEST(model_fabric_lookups) {
  const auto fabric = make_two_by_two();
  SLF_EXPECT(fabric.ok());
  const Fabric& value = fabric.value();
  SLF_EXPECT_EQ(value.leaves.size(), 2U);
  SLF_EXPECT_EQ(value.spines.size(), 2U);
  SLF_EXPECT_EQ(value.links.size(), 4U);
  SLF_EXPECT_EQ(value.ports.size(), 8U);
  SLF_EXPECT(value.find_leaf(LeafId{1}) != nullptr);
  SLF_EXPECT(value.find_leaf(LeafId{9}) == nullptr);
  SLF_EXPECT(value.find_spine(SpineId{2}) != nullptr);
  SLF_EXPECT(value.find_port(PortId{1}) != nullptr);
  SLF_EXPECT(value.find_link(LinkId{4}) != nullptr);
  SLF_EXPECT(value.find_link(LinkId{5}) == nullptr);
  SLF_EXPECT_EQ(value.node_state(NodeKey{LeafId{1}}), std::optional<NodeState>(NodeState::Active));
  SLF_EXPECT(!value.node_state(NodeKey{LeafId{7}}).has_value());
  SLF_EXPECT_EQ(value.node_role_incarnation(NodeKey{LeafId{1}}),
                std::optional<RoleIncarnation>(RoleIncarnation{11}));
  SLF_EXPECT_EQ(value.node_domain(NodeKey{SpineId{2}}), std::optional<FailureDomainId>(FailureDomainId{2}));
  SLF_EXPECT_EQ(value.node_name(NodeKey{LeafId{2}}), std::optional<std::string_view>("leaf-2"));
  SLF_EXPECT_EQ(value.node_nominal_capacity_mbps(NodeKey{LeafId{1}}), std::optional<std::uint64_t>(400000));
  SLF_EXPECT_EQ(value.active_leaf_count(), 2U);
  SLF_EXPECT(!value.empty());

  const LinkRecord* link = value.find_link(LinkId{1});
  SLF_EXPECT(link != nullptr);
  SLF_EXPECT_EQ(value.effective_link_speed_mbps(*link), std::optional<std::uint64_t>(100000));
  SLF_EXPECT(link->touches(NodeKey{LeafId{1}}));
  SLF_EXPECT_EQ(link->other(NodeKey{LeafId{1}}), NodeKey{SpineId{1}});
}

SLF_TEST(model_evidence_requires_minted_observer) {
  FabricBuilder builder(FabricId{1}, "observer", TopologyGeneration{1}, Epoch{1});
  const LeafSpec leaf{1, "", LeafRole::Tor, 1, NodeState::Active, 100000, 1};
  const SpineSpec spine{1, "", SpineRole::FabricSpine, 1, NodeState::Active, 100000, 1};
  SLF_EXPECT(builder.add_leaf(leaf_record(leaf, TopologyGeneration{1})).ok());
  SLF_EXPECT(builder.add_spine(spine_record(spine, TopologyGeneration{1})).ok());
  PortRecord port_a;
  port_a.id = PortId{1};
  port_a.owner = NodeKey{LeafId{1}};
  port_a.speed_mbps = 100000;
  port_a.generation = TopologyGeneration{1};
  PortRecord port_b;
  port_b.id = PortId{2};
  port_b.owner = NodeKey{SpineId{1}};
  port_b.speed_mbps = 100000;
  port_b.generation = TopologyGeneration{1};
  SLF_EXPECT(builder.add_port(port_a).ok());
  SLF_EXPECT(builder.add_port(port_b).ok());
  LinkRecord link;
  link.id = LinkId{1};
  link.a = NodeKey{LeafId{1}};
  link.port_a = PortId{1};
  link.b = NodeKey{SpineId{1}};
  link.port_b = PortId{2};
  link.generation = TopologyGeneration{1};
  SLF_EXPECT(builder.add_link(link).ok());

  // A zero incarnation is not a minted grant: evidence claiming to come from
  // nobody is refused, and the builder refuses to freeze afterwards because the
  // refusal is recorded as an error issue rather than silently dropped.
  LinkEvidence invalid;
  invalid.link = LinkId{1};
  invalid.observed = LinkObservation::Up;
  invalid.generation = TopologyGeneration{1};
  invalid.observer = ControllerIncarnation{};
  invalid.epoch = Epoch{1};
  invalid.observed_seq = SequenceNumber{1};
  SLF_EXPECT_CODE(builder.add_evidence(invalid), StatusCode::Invalid);
  SLF_EXPECT_CODE(builder.freeze(), StatusCode::Invalid);

  // Evidence with a zero sequence is refused too.
  FabricBuilder other(FabricId{1}, "observer", TopologyGeneration{1}, Epoch{1});
  SLF_EXPECT(other.add_leaf(leaf_record(leaf, TopologyGeneration{1})).ok());
  SLF_EXPECT(other.add_spine(spine_record(spine, TopologyGeneration{1})).ok());
  SLF_EXPECT(other.add_port(port_a).ok());
  SLF_EXPECT(other.add_port(port_b).ok());
  SLF_EXPECT(other.add_link(link).ok());
  LinkEvidence zero_sequence = invalid;
  zero_sequence.observer = mint_incarnation();
  zero_sequence.observed_seq = SequenceNumber{0};
  SLF_EXPECT_CODE(other.add_evidence(zero_sequence), StatusCode::Invalid);
}

SLF_TEST(model_effective_speed_prefers_declared) {
  FabricBuilder builder(FabricId{1}, "declared", TopologyGeneration{1}, Epoch{1});
  const LeafSpec leaf{1, "", LeafRole::Tor, 1, NodeState::Active, 400000, 11};
  const SpineSpec spine{1, "", SpineRole::FabricSpine, 1, NodeState::Active, 3200000, 21};
  SLF_EXPECT(builder.add_leaf(leaf_record(leaf, TopologyGeneration{1})).ok());
  SLF_EXPECT(builder.add_spine(spine_record(spine, TopologyGeneration{1})).ok());
  PortRecord leaf_port;
  leaf_port.id = PortId{1};
  leaf_port.owner = NodeKey{LeafId{1}};
  leaf_port.speed_mbps = 40000;
  leaf_port.generation = TopologyGeneration{1};
  PortRecord spine_port;
  spine_port.id = PortId{2};
  spine_port.owner = NodeKey{SpineId{1}};
  spine_port.speed_mbps = 100000;
  spine_port.generation = TopologyGeneration{1};
  SLF_EXPECT(builder.add_port(leaf_port).ok());
  SLF_EXPECT(builder.add_port(spine_port).ok());
  LinkRecord link;
  link.id = LinkId{1};
  link.a = NodeKey{LeafId{1}};
  link.port_a = PortId{1};
  link.b = NodeKey{SpineId{1}};
  link.port_b = PortId{2};
  link.declared_speed_mbps = 25000;
  link.generation = TopologyGeneration{1};
  SLF_EXPECT(builder.add_link(link).ok());
  const auto frozen = builder.freeze();
  SLF_EXPECT(frozen.ok());
  SLF_EXPECT_EQ(frozen.value().effective_link_speed_mbps(frozen.value().links[0]),
                std::optional<std::uint64_t>(25000));
}

SLF_TEST(model_evidence_selection_and_freshness) {
  const auto base = make_two_by_two();
  SLF_EXPECT(base.ok());
  const ControllerIncarnation observer = mint_incarnation();
  FabricBuilder builder(FabricId{1}, base.value().name, base.value().generation, Epoch{1},
                        base.value().options);
  (void)builder.set_site(base.value().site);
  for (const auto& leaf : base.value().leaves) {
    SLF_EXPECT(builder.add_leaf(leaf).ok());
  }
  for (const auto& spine : base.value().spines) {
    SLF_EXPECT(builder.add_spine(spine).ok());
  }
  for (const auto& port : base.value().ports) {
    SLF_EXPECT(builder.add_port(port).ok());
  }
  for (const auto& link : base.value().links) {
    SLF_EXPECT(builder.add_link(link).ok());
  }
  LinkEvidence older;
  older.link = LinkId{1};
  older.observed = LinkObservation::Down;
  older.generation = base.value().generation;
  older.observer = observer;
  older.epoch = Epoch{1};
  older.observed_seq = SequenceNumber{1};
  SLF_EXPECT(builder.add_evidence(older).ok());
  const ControllerIncarnation other_observer = mint_incarnation();
  LinkEvidence newer = older;
  newer.observed = LinkObservation::Up;
  newer.observer = other_observer;
  newer.observed_seq = SequenceNumber{2};
  SLF_EXPECT(builder.add_evidence(newer).ok());
  const auto frozen = builder.freeze();
  SLF_EXPECT(frozen.ok());
  const auto selected = frozen.value().evidence_for(LinkId{1});
  SLF_EXPECT(selected.ok());
  SLF_EXPECT_EQ(selected.value()->observed_seq, SequenceNumber{2});
  SLF_EXPECT_CODE(frozen.value().evidence_for(LinkId{3}), StatusCode::NotFound);
  SLF_EXPECT_EQ(frozen.value().evidence_freshness(*selected.value(), other_observer, Epoch{1}),
                EvidenceFreshness::Fresh);
  SLF_EXPECT_EQ(frozen.value().evidence_freshness(*selected.value(), observer, Epoch{1}),
                EvidenceFreshness::Historical);
  SLF_EXPECT_EQ(frozen.value().evidence_freshness(*selected.value(), other_observer, Epoch{2}),
                EvidenceFreshness::Historical);
}

SLF_TEST(model_evidence_conflicting_ties) {
  FabricBuilder builder(FabricId{1}, "ties", TopologyGeneration{1}, Epoch{1});
  const LeafSpec leaf{1, "", LeafRole::Tor, 1, NodeState::Active, 100000, 1};
  const SpineSpec spine{1, "", SpineRole::FabricSpine, 1, NodeState::Active, 100000, 1};
  SLF_EXPECT(builder.add_leaf(leaf_record(leaf, TopologyGeneration{1})).ok());
  SLF_EXPECT(builder.add_spine(spine_record(spine, TopologyGeneration{1})).ok());
  PortRecord leaf_port;
  leaf_port.id = PortId{1};
  leaf_port.owner = NodeKey{LeafId{1}};
  leaf_port.speed_mbps = 100000;
  leaf_port.generation = TopologyGeneration{1};
  PortRecord spine_port;
  spine_port.id = PortId{2};
  spine_port.owner = NodeKey{SpineId{1}};
  spine_port.speed_mbps = 100000;
  spine_port.generation = TopologyGeneration{1};
  SLF_EXPECT(builder.add_port(leaf_port).ok());
  SLF_EXPECT(builder.add_port(spine_port).ok());
  LinkRecord link;
  link.id = LinkId{1};
  link.a = NodeKey{LeafId{1}};
  link.port_a = PortId{1};
  link.b = NodeKey{SpineId{1}};
  link.port_b = PortId{2};
  link.generation = TopologyGeneration{1};
  SLF_EXPECT(builder.add_link(link).ok());
  for (int i = 0; i < 2; ++i) {
    LinkEvidence evidence;
    evidence.link = LinkId{1};
    evidence.observed = i == 0 ? LinkObservation::Up : LinkObservation::Down;
    evidence.generation = TopologyGeneration{1};
    evidence.observer = mint_incarnation();
    evidence.epoch = Epoch{1};
    evidence.observed_seq = SequenceNumber{5};
    SLF_EXPECT(builder.add_evidence(evidence).ok());
  }
  const auto frozen = builder.freeze();
  SLF_EXPECT(frozen.ok());
  SLF_EXPECT_CODE(frozen.value().evidence_for(LinkId{1}), StatusCode::Conflicting);
}

SLF_TEST(model_digest_is_insertion_order_independent) {
  const auto first = make_two_by_two();
  SLF_EXPECT(first.ok());
  FabricBuilder builder(FabricId{1}, "test-fabric", TopologyGeneration{1}, Epoch{1});
  for (auto it = first.value().spines.rbegin(); it != first.value().spines.rend(); ++it) {
    SLF_EXPECT(builder.add_spine(*it).ok());
  }
  for (auto it = first.value().leaves.rbegin(); it != first.value().leaves.rend(); ++it) {
    SLF_EXPECT(builder.add_leaf(*it).ok());
  }
  for (auto it = first.value().ports.rbegin(); it != first.value().ports.rend(); ++it) {
    SLF_EXPECT(builder.add_port(*it).ok());
  }
  for (auto it = first.value().links.rbegin(); it != first.value().links.rend(); ++it) {
    SLF_EXPECT(builder.add_link(*it).ok());
  }
  const auto second = builder.freeze();
  SLF_EXPECT(second.ok());
  SLF_EXPECT_EQ(first.value().topology_digest, second.value().topology_digest);
  SLF_EXPECT_EQ(canonical_topology_bytes(first.value()), canonical_topology_bytes(second.value()));

  FabricBuilder changed(FabricId{1}, "test-fabric", TopologyGeneration{1}, Epoch{1});
  for (const auto& leaf : first.value().leaves) {
    LeafRecord record = leaf;
    if (record.id == LeafId{1}) {
      record.role_incarnation = RoleIncarnation{999};
    }
    SLF_EXPECT(changed.add_leaf(record).ok());
  }
  for (const auto& spine : first.value().spines) {
    SLF_EXPECT(changed.add_spine(spine).ok());
  }
  for (const auto& port : first.value().ports) {
    SLF_EXPECT(changed.add_port(port).ok());
  }
  for (const auto& link : first.value().links) {
    SLF_EXPECT(changed.add_link(link).ok());
  }
  const auto altered = changed.freeze();
  SLF_EXPECT(altered.ok());
  SLF_EXPECT_NE(altered.value().topology_digest, first.value().topology_digest);
}

SLF_TEST(model_serialization_roundtrip) {
  const auto fabric = make_two_by_two();
  SLF_EXPECT(fabric.ok());
  const std::vector<std::byte> bytes = serialize_fabric(fabric.value());
  const auto parsed = deserialize_fabric(std::span<const std::byte>(bytes), BuilderLimits{});
  SLF_EXPECT(parsed.ok());
  SLF_EXPECT_EQ(parsed.value().topology_digest, fabric.value().topology_digest);
  SLF_EXPECT_EQ(serialize_fabric(parsed.value()), bytes);

  std::vector<std::byte> wrong_version = bytes;
  wrong_version[0] = std::byte{9};
  SLF_EXPECT_CODE(deserialize_fabric(std::span<const std::byte>(wrong_version), BuilderLimits{}),
                  StatusCode::IncompatibleVersion);
  for (std::size_t cut = 0; cut < bytes.size(); cut += 7) {
    const std::span<const std::byte> prefix(bytes.data(), cut);
    const auto outcome = deserialize_fabric(prefix, BuilderLimits{});
    SLF_EXPECT(!outcome.ok());
  }
  std::vector<std::byte> extended = bytes;
  extended.push_back(std::byte{0});
  SLF_EXPECT_CODE(deserialize_fabric(std::span<const std::byte>(extended), BuilderLimits{}),
                  StatusCode::Invalid);
  BuilderLimits tiny;
  tiny.max_leaves = 1;
  SLF_EXPECT_CODE(deserialize_fabric(std::span<const std::byte>(bytes), tiny), StatusCode::Exhausted);
}
