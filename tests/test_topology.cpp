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

#include "slf/topology.hpp"

#include <algorithm>

#include "fixtures.hpp"
#include "slf_test.hpp"

using namespace slf;
using namespace slf::test;

namespace {

Outcome<Fabric> three_leaf_fabric() {
  return make_grid(TopologyGeneration{1},
                   {{1, "", LeafRole::Tor, 1, NodeState::Active, 400000, 11},
                    {2, "", LeafRole::Tor, 2, NodeState::Active, 400000, 12},
                    {3, "", LeafRole::Tor, 3, NodeState::Active, 400000, 13}},
                   {{1, "", SpineRole::FabricSpine, 1, NodeState::Active, 3200000, 21},
                    {2, "", SpineRole::FabricSpine, 2, NodeState::Active, 3200000, 22}},
                   {{1, 1, 100000}, {1, 2, 100000}, {2, 1, 100000}, {2, 2, 100000}, {3, 1, 100000}});
}

}  // namespace

SLF_TEST(topology_reachability_classes) {
  const auto fabric = three_leaf_fabric();
  SLF_EXPECT(fabric.ok());
  const auto index = TopologyIndex::build(fabric.value());
  SLF_EXPECT(index.ok());
  const TopologyIndex& graph = index.value();

  SLF_EXPECT_EQ(graph.structural_class(NodeKey{LeafId{1}}, NodeKey{LeafId{1}}), ReachabilityClass::SameNode);
  SLF_EXPECT_EQ(graph.structural_class(NodeKey{LeafId{1}}, NodeKey{SpineId{1}}),
                ReachabilityClass::DirectLeafSpine);
  SLF_EXPECT_EQ(graph.structural_class(NodeKey{LeafId{1}}, NodeKey{LeafId{2}}),
                ReachabilityClass::LeafLeafMultiSpine);
  SLF_EXPECT_EQ(graph.structural_class(NodeKey{LeafId{1}}, NodeKey{LeafId{3}}),
                ReachabilityClass::LeafLeafSingleSpine);
  SLF_EXPECT_EQ(graph.structural_class(NodeKey{SpineId{1}}, NodeKey{SpineId{2}}),
                ReachabilityClass::SpineSpineViaLeaf);
  // Leaf 3 is only attached to spine 1, so leaf 3 and spine 2 are not adjacent
  // and there is no peer link to bridge them.
  SLF_EXPECT_EQ(graph.structural_class(NodeKey{LeafId{3}}, NodeKey{SpineId{2}}),
                ReachabilityClass::Unreachable);
  SLF_EXPECT_EQ(graph.structural_class(NodeKey{LeafId{3}}, NodeKey{LeafId{3}}), ReachabilityClass::SameNode);
  SLF_EXPECT_EQ(to_string(ReachabilityClass::LeafLeafMultiSpine), std::string_view("leaf_leaf_multi_spine"));
}

SLF_TEST(topology_adjacency_accessors) {
  const auto fabric = three_leaf_fabric();
  SLF_EXPECT(fabric.ok());
  const auto index = TopologyIndex::build(fabric.value());
  SLF_EXPECT(index.ok());
  const TopologyIndex& graph = index.value();

  SLF_EXPECT_EQ(graph.node_count(), 5U);
  SLF_EXPECT_EQ(graph.edge_count(), 10U);
  SLF_EXPECT_EQ(graph.degree(NodeKey{LeafId{1}}), 2U);
  SLF_EXPECT_EQ(graph.uplinks(LeafId{1}).size(), 2U);
  SLF_EXPECT_EQ(graph.uplinks(LeafId{9}).size(), 0U);
  SLF_EXPECT_EQ(graph.downlinks(SpineId{1}).size(), 3U);
  SLF_EXPECT_EQ(graph.peer_links(NodeKey{LeafId{1}}).size(), 0U);
  SLF_EXPECT(graph.adjacent(NodeKey{LeafId{1}}, NodeKey{SpineId{2}}));
  SLF_EXPECT(!graph.adjacent(NodeKey{LeafId{3}}, NodeKey{SpineId{2}}));
  const auto adjacency = graph.adjacency_between(NodeKey{LeafId{1}}, NodeKey{SpineId{1}});
  SLF_EXPECT(adjacency.has_value());
  SLF_EXPECT_EQ(adjacency->speed_mbps, 100000U);
  SLF_EXPECT_EQ(adjacency->cls, LinkClass::LeafSpine);
  SLF_EXPECT(graph.all_edges_resolve());
  SLF_EXPECT_EQ(graph.nodes().size(), 5U);

  const auto common = graph.common_spines(LeafId{1}, LeafId{2});
  SLF_EXPECT_EQ(common.size(), 2U);
  SLF_EXPECT_EQ(common[0], SpineId{1});
  SLF_EXPECT_EQ(common[1], SpineId{2});
  SLF_EXPECT_EQ(graph.common_spines(LeafId{1}, LeafId{3}).size(), 1U);

  // Adjacency lists are ordered by peer key.
  const auto first = graph.adjacencies(NodeKey{LeafId{1}});
  SLF_EXPECT(first.size() == 2U && first[0].peer < first[1].peer);
}

SLF_TEST(topology_disabled_links_leave_the_struct) {
  const auto fabric = make_grid(TopologyGeneration{1},
                                {{1, "", LeafRole::Tor, 1, NodeState::Active, 400000, 11},
                                 {2, "", LeafRole::Tor, 2, NodeState::Active, 400000, 12}},
                                {{1, "", SpineRole::FabricSpine, 1, NodeState::Active, 3200000, 21}},
                                {{1, 1, 100000}, {2, 1, 100000, LinkAdmin::Disabled}});
  SLF_EXPECT(fabric.ok());
  const auto index = TopologyIndex::build(fabric.value());
  SLF_EXPECT(index.ok());
  // The administratively disabled link is configuration, so the structural
  // graph simply does not contain it.
  SLF_EXPECT_EQ(index.value().edge_count(), 2U);
  SLF_EXPECT_EQ(index.value().structural_class(NodeKey{LeafId{2}}, NodeKey{SpineId{1}}),
                ReachabilityClass::Unreachable);
}

SLF_TEST(topology_sparse_identifiers) {
  const auto fabric = make_grid(TopologyGeneration{1},
                                {{5, "", LeafRole::Tor, 1, NodeState::Active, 400000, 11},
                                 {9, "", LeafRole::Tor, 2, NodeState::Active, 400000, 12}},
                                {{3, "", SpineRole::FabricSpine, 1, NodeState::Active, 3200000, 21}},
                                {{5, 3, 100000}, {9, 3, 100000}});
  SLF_EXPECT(fabric.ok());
  const auto index = TopologyIndex::build(fabric.value());
  SLF_EXPECT(index.ok());
  // Identifiers are sparse: slot lookup must not assume dense numbering.
  SLF_EXPECT_EQ(index.value().uplinks(LeafId{5}).size(), 1U);
  SLF_EXPECT_EQ(index.value().uplinks(LeafId{9}).size(), 1U);
  SLF_EXPECT_EQ(index.value().uplinks(LeafId{1}).size(), 0U);
  SLF_EXPECT_EQ(index.value().downlinks(SpineId{3}).size(), 2U);
  SLF_EXPECT_EQ(index.value().structural_class(NodeKey{LeafId{5}}, NodeKey{LeafId{9}}),
                ReachabilityClass::LeafLeafSingleSpine);
  SLF_EXPECT(index.value().all_edges_resolve());
}

SLF_TEST(topology_peer_links_and_paths) {
  FabricBuilder builder(FabricId{1}, "peers", TopologyGeneration{1}, Epoch{1},
                        FabricOptions{.allow_same_tier_adjacency = true});
  SLF_EXPECT(builder.add_leaf(leaf_record(LeafSpec{1, "", LeafRole::BorderLeaf, 1}, TopologyGeneration{1})).ok());
  SLF_EXPECT(builder.add_leaf(leaf_record(LeafSpec{2, "", LeafRole::BorderLeaf, 2}, TopologyGeneration{1})).ok());
  SLF_EXPECT(builder.add_spine(spine_record(SpineSpec{1, "", SpineRole::FabricSpine, 1}, TopologyGeneration{1}))
                 .ok());
  PortRecord p1;
  p1.id = PortId{1};
  p1.owner = NodeKey{LeafId{1}};
  p1.speed_mbps = 100000;
  p1.generation = TopologyGeneration{1};
  PortRecord p2;
  p2.id = PortId{2};
  p2.owner = NodeKey{LeafId{2}};
  p2.speed_mbps = 100000;
  p2.generation = TopologyGeneration{1};
  PortRecord p3;
  p3.id = PortId{3};
  p3.owner = NodeKey{LeafId{1}};
  p3.speed_mbps = 100000;
  p3.generation = TopologyGeneration{1};
  PortRecord p4;
  p4.id = PortId{4};
  p4.owner = NodeKey{SpineId{1}};
  p4.speed_mbps = 100000;
  p4.generation = TopologyGeneration{1};
  SLF_EXPECT(builder.add_port(p1).ok());
  SLF_EXPECT(builder.add_port(p2).ok());
  SLF_EXPECT(builder.add_port(p3).ok());
  SLF_EXPECT(builder.add_port(p4).ok());
  LinkRecord peer;
  peer.id = LinkId{1};
  peer.a = NodeKey{LeafId{1}};
  peer.port_a = PortId{1};
  peer.b = NodeKey{LeafId{2}};
  peer.port_b = PortId{2};
  peer.cls = LinkClass::SameTierPeer;
  peer.generation = TopologyGeneration{1};
  LinkRecord uplink;
  uplink.id = LinkId{2};
  uplink.a = NodeKey{LeafId{1}};
  uplink.port_a = PortId{3};
  uplink.b = NodeKey{SpineId{1}};
  uplink.port_b = PortId{4};
  uplink.cls = LinkClass::LeafSpine;
  uplink.generation = TopologyGeneration{1};
  SLF_EXPECT(builder.add_link(peer).ok());
  SLF_EXPECT(builder.add_link(uplink).ok());
  const auto fabric = builder.freeze();
  SLF_EXPECT(fabric.ok());
  const auto index = TopologyIndex::build(fabric.value());
  SLF_EXPECT(index.ok());
  SLF_EXPECT_EQ(index.value().structural_class(NodeKey{LeafId{1}}, NodeKey{LeafId{2}}),
                ReachabilityClass::DirectPeer);
  SLF_EXPECT_EQ(index.value().structural_class(NodeKey{LeafId{2}}, NodeKey{SpineId{1}}),
                ReachabilityClass::LeafSpineViaPeer);
  SLF_EXPECT_EQ(index.value().peer_links(NodeKey{LeafId{1}}).size(), 1U);
  // Bounded path enumeration never explodes and never returns duplicates.
  const auto paths = index.value().enumerate_paths(NodeKey{LeafId{2}}, NodeKey{SpineId{1}}, 3, 8);
  SLF_EXPECT(!paths.empty());
  SLF_EXPECT(paths.size() <= 8U);
  for (const auto& path : paths) {
    SLF_EXPECT(path.size() <= 4U);
    SLF_EXPECT_EQ(path.front(), NodeKey{LeafId{2}});
    SLF_EXPECT_EQ(path.back(), NodeKey{SpineId{1}});
  }
  SLF_EXPECT(index.value().enumerate_paths(NodeKey{LeafId{2}}, NodeKey{SpineId{1}}, 0, 8).empty());
  SLF_EXPECT(index.value().enumerate_paths(NodeKey{LeafId{2}}, NodeKey{SpineId{1}}, 3, 0).empty());
}

SLF_TEST(topology_rejects_unfrozen_fabric) {
  Fabric unfrozen;
  unfrozen.name = "unfrozen";
  SLF_EXPECT_CODE(TopologyIndex::build(unfrozen), StatusCode::Invalid);
}
