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
#include <cstddef>
#include <functional>
#include <unordered_set>
#include <utility>

#include "slf/checked.hpp"

namespace slf {
namespace {

[[nodiscard]] bool adjacency_less(const Adjacency& a, const Adjacency& b) noexcept {
  if (a.peer != b.peer) {
    return a.peer < b.peer;
  }
  return a.link < b.link;
}

}  // namespace

std::string_view to_string(ReachabilityClass cls) noexcept {
  switch (cls) {
    case ReachabilityClass::SameNode: return "same_node";
    case ReachabilityClass::DirectLeafSpine: return "direct_leaf_spine";
    case ReachabilityClass::DirectPeer: return "direct_peer";
    case ReachabilityClass::LeafSpineViaPeer: return "leaf_spine_via_peer";
    case ReachabilityClass::LeafLeafSingleSpine: return "leaf_leaf_single_spine";
    case ReachabilityClass::LeafLeafMultiSpine: return "leaf_leaf_multi_spine";
    case ReachabilityClass::SpineSpineViaLeaf: return "spine_spine_via_leaf";
    case ReachabilityClass::Unreachable: return "unreachable";
    case ReachabilityClass::Indeterminate: return "indeterminate";
  }
  return "invalid";
}

std::size_t TopologyIndex::node_slot(NodeKey key) const noexcept {
  const auto it = std::lower_bound(keys_.begin(), keys_.end(), key);
  if (it == keys_.end() || *it != key) {
    return kNoSlot;
  }
  return static_cast<std::size_t>(it - keys_.begin());
}

Outcome<TopologyIndex> TopologyIndex::build(const Fabric& fabric) {
  return build(std::make_shared<const Fabric>(fabric));
}

Outcome<TopologyIndex> TopologyIndex::build(std::shared_ptr<const Fabric> fabric_handle) {
  if (fabric_handle == nullptr) {
    return Status(StatusCode::Invalid, "cannot index a null fabric");
  }
  const Fabric& fabric = *fabric_handle;
  if (fabric.topology_digest.is_zero()) {
    return Status(StatusCode::Invalid, "cannot index a fabric that has not been frozen");
  }
  TopologyIndex index;
  index.owner_ = std::move(fabric_handle);
  index.fabric_ = index.owner_.get();
  index.leaf_count_ = static_cast<std::uint32_t>(fabric.leaves.size());
  index.spine_count_ = static_cast<std::uint32_t>(fabric.spines.size());
  std::uint64_t total_nodes = 0;
  if (!checked_add(index.leaf_count_, index.spine_count_, total_nodes) ||
      total_nodes > 0xFFFFFFFFULL) {
    return Status(StatusCode::Overflow, "node count overflow");
  }
  index.node_count_ = static_cast<std::uint32_t>(total_nodes);

  // Node keys are sorted by (tier, index) because the record vectors are sorted
  // and every leaf precedes every spine in the tier ordering. Slot lookup is a
  // binary search, so identifiers may be sparse.
  index.keys_.reserve(index.node_count_);
  for (const auto& leaf : fabric.leaves) {
    index.keys_.emplace_back(leaf.id);
  }
  for (const auto& spine : fabric.spines) {
    index.keys_.emplace_back(spine.id);
  }
  std::vector<Adjacency> edges;
  std::vector<Adjacency> leaf_spine_edges;
  std::vector<Adjacency> peer_edges;
  edges.reserve(fabric.links.size() * 2);
  leaf_spine_edges.reserve(fabric.links.size() * 2);
  peer_edges.reserve(8);

  for (const auto& link : fabric.links) {
    if (link.admin != LinkAdmin::Enabled) {
      continue;
    }
    const auto speed = fabric.effective_link_speed_mbps(link);
    if (!speed.has_value()) {
      return Status(StatusCode::Invalid, "link " + to_string(link.id) + " has no resolvable speed");
    }
    Adjacency forward;
    forward.link = link.id;
    forward.self = link.a;
    forward.peer = link.b;
    forward.local_port = link.port_a;
    forward.peer_port = link.port_b;
    forward.cls = link.cls;
    forward.admin = link.admin;
    forward.speed_mbps = *speed;
    forward.generation = link.generation;

    Adjacency backward;
    backward.link = link.id;
    backward.self = link.b;
    backward.peer = link.a;
    backward.local_port = link.port_b;
    backward.peer_port = link.port_a;
    backward.cls = link.cls;
    backward.admin = link.admin;
    backward.speed_mbps = *speed;
    backward.generation = link.generation;

    if (link.cls == LinkClass::LeafSpine) {
      if (link.a.tier == Tier::Leaf) {
        edges.push_back(forward);
        leaf_spine_edges.push_back(forward);
        edges.push_back(backward);
        leaf_spine_edges.push_back(backward);
      } else {
        edges.push_back(forward);
        leaf_spine_edges.push_back(forward);
        edges.push_back(backward);
        leaf_spine_edges.push_back(backward);
      }
    } else {
      edges.push_back(forward);
      peer_edges.push_back(forward);
      edges.push_back(backward);
      peer_edges.push_back(backward);
    }
  }

  std::sort(edges.begin(), edges.end(), adjacency_less);
  std::sort(leaf_spine_edges.begin(), leaf_spine_edges.end(), adjacency_less);
  std::sort(peer_edges.begin(), peer_edges.end(), adjacency_less);

  const auto build_csr = [&index](const std::vector<Adjacency>& source, std::vector<std::uint32_t>& offsets,
                                  std::vector<Adjacency>& destination) -> Status {
    std::vector<std::uint32_t> counts(static_cast<std::size_t>(index.node_count_) + 1, 0);
    for (const auto& adjacency : source) {
      const std::size_t slot = index.node_slot(adjacency.self);
      if (slot == TopologyIndex::kNoSlot) {
        return Status(StatusCode::DanglingEdge, "adjacency references an unknown node");
      }
      if (counts[slot] == 0xFFFFFFFFU) {
        return Status(StatusCode::Overflow, "adjacency count overflow");
      }
      ++counts[slot];
    }
    offsets.assign(static_cast<std::size_t>(index.node_count_) + 1, 0);
    for (std::size_t i = 0; i < index.node_count_; ++i) {
      offsets[i + 1] = offsets[i] + counts[i];
    }
    destination.assign(source.size(), Adjacency{});
    std::vector<std::uint32_t> cursor(offsets.begin(), offsets.end() - 1);
    for (const auto& adjacency : source) {
      const std::size_t slot = index.node_slot(adjacency.self);
      if (slot == TopologyIndex::kNoSlot) {
        return Status(StatusCode::DanglingEdge, "adjacency references an unknown node");
      }
      destination[cursor[slot]] = adjacency;
      ++cursor[slot];
    }
    return Status::success();
  };

  Status status = build_csr(edges, index.offsets_, index.edges_);
  if (!status.ok()) {
    return status;
  }
  status = build_csr(leaf_spine_edges, index.leaf_spine_offsets_, index.edges_leaf_spine_);
  if (!status.ok()) {
    return status;
  }
  status = build_csr(peer_edges, index.peer_offsets_, index.edges_peer_);
  if (!status.ok()) {
    return status;
  }
  return index;
}

std::span<const Adjacency> TopologyIndex::adjacencies(NodeKey key) const noexcept {
  const std::size_t slot = node_slot(key);
  if (slot == kNoSlot || offsets_.empty()) {
    return {};
  }
  const std::uint32_t begin = offsets_[slot];
  const std::uint32_t end = offsets_[slot + 1];
  return std::span<const Adjacency>(edges_.data() + begin, static_cast<std::size_t>(end - begin));
}

std::span<const Adjacency> TopologyIndex::uplinks(LeafId id) const noexcept {
  const std::size_t slot = node_slot(NodeKey{id});
  if (slot == kNoSlot || leaf_spine_offsets_.empty()) {
    return {};
  }
  const std::uint32_t begin = leaf_spine_offsets_[slot];
  const std::uint32_t end = leaf_spine_offsets_[slot + 1];
  return std::span<const Adjacency>(edges_leaf_spine_.data() + begin,
                                    static_cast<std::size_t>(end - begin));
}

std::span<const Adjacency> TopologyIndex::peer_links(NodeKey key) const noexcept {
  const std::size_t slot = node_slot(key);
  if (slot == kNoSlot || peer_offsets_.empty()) {
    return {};
  }
  const std::uint32_t begin = peer_offsets_[slot];
  const std::uint32_t end = peer_offsets_[slot + 1];
  return std::span<const Adjacency>(edges_peer_.data() + begin, static_cast<std::size_t>(end - begin));
}

std::span<const Adjacency> TopologyIndex::downlinks(SpineId id) const noexcept {
  const std::size_t slot = node_slot(NodeKey{id});
  if (slot == kNoSlot || leaf_spine_offsets_.empty()) {
    return {};
  }
  const std::uint32_t begin = leaf_spine_offsets_[slot];
  const std::uint32_t end = leaf_spine_offsets_[slot + 1];
  return std::span<const Adjacency>(edges_leaf_spine_.data() + begin,
                                    static_cast<std::size_t>(end - begin));
}

std::optional<Adjacency> TopologyIndex::adjacency_between(NodeKey a, NodeKey b) const noexcept {
  for (const auto& adjacency : adjacencies(a)) {
    if (adjacency.peer == b) {
      return adjacency;
    }
  }
  return std::nullopt;
}

bool TopologyIndex::adjacent(NodeKey a, NodeKey b) const noexcept {
  return adjacency_between(a, b).has_value();
}

std::uint32_t TopologyIndex::degree(NodeKey key) const noexcept {
  return static_cast<std::uint32_t>(adjacencies(key).size());
}

std::vector<NodeKey> TopologyIndex::nodes() const { return keys_; }

std::vector<SpineId> TopologyIndex::common_spines(LeafId a, LeafId b) const {
  std::vector<SpineId> out;
  if (a == b) {
    return out;
  }
  const auto first = uplinks(a);
  const auto second = uplinks(b);
  if (first.empty() || second.empty()) {
    return out;
  }
  const auto& probe = first.size() <= second.size() ? first : second;
  const auto& other = first.size() <= second.size() ? second : first;
  for (const auto& adjacency : probe) {
    if (adjacency.peer.tier != Tier::Spine) {
      continue;
    }
    const bool found = std::any_of(other.begin(), other.end(), [&adjacency](const Adjacency& candidate) {
      return candidate.peer == adjacency.peer;
    });
    if (found) {
      out.push_back(SpineId{adjacency.peer.index});
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

ReachabilityClass TopologyIndex::structural_class(NodeKey a, NodeKey b) const noexcept {
  if (a == b) {
    return ReachabilityClass::SameNode;
  }
  const auto direct = adjacency_between(a, b);
  if (direct.has_value()) {
    return direct->cls == LinkClass::LeafSpine ? ReachabilityClass::DirectLeafSpine
                                               : ReachabilityClass::DirectPeer;
  }
  if (a.tier == Tier::Leaf && b.tier == Tier::Leaf) {
    const auto spines = common_spines(LeafId{a.index}, LeafId{b.index});
    if (spines.size() >= 2U) {
      return ReachabilityClass::LeafLeafMultiSpine;
    }
    if (spines.size() == 1U) {
      return ReachabilityClass::LeafLeafSingleSpine;
    }
    for (const auto& peer : peer_links(a)) {
      if (adjacency_between(peer.peer, b).has_value()) {
        return ReachabilityClass::LeafSpineViaPeer;
      }
    }
    for (const auto& peer : peer_links(b)) {
      if (adjacency_between(peer.peer, a).has_value()) {
        return ReachabilityClass::LeafSpineViaPeer;
      }
    }
    return ReachabilityClass::Unreachable;
  }
  if (a.tier == Tier::Spine && b.tier == Tier::Spine) {
    for (const auto& downlink : downlinks(SpineId{a.index})) {
      if (adjacency_between(downlink.peer, b).has_value()) {
        return ReachabilityClass::SpineSpineViaLeaf;
      }
    }
    return ReachabilityClass::Unreachable;
  }
  // One leaf, one spine, not directly adjacent: the only remaining permitted
  // shape is leaf -> peer leaf -> spine.
  const NodeKey leaf = a.tier == Tier::Leaf ? a : b;
  const NodeKey spine = a.tier == Tier::Leaf ? b : a;
  for (const auto& peer : peer_links(leaf)) {
    if (adjacency_between(peer.peer, spine).has_value()) {
      return ReachabilityClass::LeafSpineViaPeer;
    }
  }
  return ReachabilityClass::Unreachable;
}

std::vector<std::vector<NodeKey>> TopologyIndex::enumerate_paths(NodeKey a, NodeKey b,
                                                                std::uint32_t max_hops,
                                                                std::uint32_t max_paths) const {
  std::vector<std::vector<NodeKey>> paths;
  if (max_hops == 0 || max_paths == 0) {
    return paths;
  }
  std::vector<NodeKey> current;
  std::unordered_set<std::uint64_t> visited;
  const auto encode = [](NodeKey key) {
    return (static_cast<std::uint64_t>(key.tier == Tier::Leaf ? 0U : 1U) << 32U) |
           static_cast<std::uint64_t>(key.index);
  };
  const std::function<void(NodeKey, std::uint32_t)> walk = [&](NodeKey node, std::uint32_t depth) {
    if (paths.size() >= max_paths) {
      return;
    }
    current.push_back(node);
    visited.insert(encode(node));
    if (node == b && current.size() > 1U) {
      paths.push_back(current);
    } else if (depth < max_hops) {
      for (const auto& adjacency : adjacencies(node)) {
        if (visited.count(encode(adjacency.peer)) != 0U) {
          continue;
        }
        walk(adjacency.peer, depth + 1U);
        if (paths.size() >= max_paths) {
          break;
        }
      }
    }
    visited.erase(encode(node));
    current.pop_back();
  };
  walk(a, 0);
  return paths;
}

bool TopologyIndex::all_edges_resolve() const noexcept {
  for (const auto& adjacency : edges_) {
    const std::size_t slot = node_slot(adjacency.self);
    const std::size_t peer_slot = node_slot(adjacency.peer);
    if (slot == kNoSlot || peer_slot == kNoSlot) {
      return false;
    }
    if (fabric_->find_link(adjacency.link) == nullptr) {
      return false;
    }
  }
  return true;
}

}  // namespace slf
