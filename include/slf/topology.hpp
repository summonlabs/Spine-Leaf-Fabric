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

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "slf/model.hpp"

namespace slf {

/// Structural reachability classes of the tiered fabric.
///
/// The classes describe the shape of the tiered fabric only. Whether a path is
/// operationally eligible (draining nodes, disabled ports, missing fresh
/// evidence) is a separate question answered by c EligibilityEngine.
enum class ReachabilityClass : std::uint8_t {
  /// The two endpoints are the same node.
  SameNode = 0,
  /// leaf <-> spine over a direct link.
  DirectLeafSpine = 1,
  /// leaf <-> leaf or spine <-> spine over an enabled same-tier peer link.
  DirectPeer = 2,
  /// leaf <-> spine over two hops through a peer leaf.
  LeafSpineViaPeer = 3,
  /// leaf <-> leaf with exactly one spine adjacent to both.
  LeafLeafSingleSpine = 4,
  /// leaf <-> leaf with two or more spines adjacent to both.
  LeafLeafMultiSpine = 5,
  /// spine <-> spine through a leaf attached to both (only possible when the
  /// fabric contains leaves that are adjacent to both spines).
  SpineSpineViaLeaf = 6,
  /// No permitted path within the bounded hop limit.
  Unreachable = 7,
  /// A path may exist but the evidence needed to decide is missing or stale.
  Indeterminate = 8,
};

[[nodiscard]] std::string_view to_string(ReachabilityClass cls) noexcept;

/// One endpoint-relative adjacency entry.
struct Adjacency {
  LinkId link{};
  NodeKey self{};
  NodeKey peer{};
  PortId local_port{};
  PortId peer_port{};
  LinkClass cls{LinkClass::LeafSpine};
  LinkAdmin admin{LinkAdmin::Enabled};
  std::uint64_t speed_mbps{0};
  TopologyGeneration generation{};

  friend bool operator==(const Adjacency&, const Adjacency&) = default;
};

/// Immutable adjacency index over one fabric generation.
///
/// Storage is a compressed adjacency array (offsets plus one flat edge vector),
/// built from the sorted record vectors of the fabric. Consequently the index
/// is independent of insertion order, has no per-node dynamic allocation, and
/// cannot be invalidated by concurrent readers: it is immutable after build.
class TopologyIndex {
 public:
  TopologyIndex() = default;

  /// Builds the index over the fabric behind p fabric_handle. The index
  /// **shares ownership** of the fabric, so an index published independently of
  /// the caller's handle can never dangle: every accessor resolves through the
  /// shared pointer. Fails with StatusCode::Invalid when the fabric has not been
  /// frozen (digests unset) or is null.
  [[nodiscard]] static Outcome<TopologyIndex> build(std::shared_ptr<const Fabric> fabric_handle);

  /// Convenience overload for offline callers that own a fabric by value. It
  /// takes a shared copy so the index still owns what it describes; use the
  /// shared-pointer overload to avoid the copy.
  [[nodiscard]] static Outcome<TopologyIndex> build(const Fabric& fabric);

  [[nodiscard]] const Fabric& fabric() const noexcept { return *fabric_; }
  [[nodiscard]] bool valid() const noexcept { return fabric_ != nullptr; }
  /// The fabric this index describes, as a shared handle.
  [[nodiscard]] const std::shared_ptr<const Fabric>& fabric_handle() const noexcept { return owner_; }

  /// All adjacencies of a node, ordered by (peer key, link id). Empty span for
  /// an unknown node.
  [[nodiscard]] std::span<const Adjacency> adjacencies(NodeKey key) const noexcept;
  /// Adjacencies of a node restricted to c LinkClass::LeafSpine.
  [[nodiscard]] std::span<const Adjacency> uplinks(LeafId id) const noexcept;
  /// Adjacencies of a node restricted to c LinkClass::SameTierPeer.
  [[nodiscard]] std::span<const Adjacency> peer_links(NodeKey key) const noexcept;
  /// Adjacencies of a spine restricted to c LinkClass::LeafSpine.
  [[nodiscard]] std::span<const Adjacency> downlinks(SpineId id) const noexcept;

  [[nodiscard]] bool adjacent(NodeKey a, NodeKey b) const noexcept;
  [[nodiscard]] std::optional<Adjacency> adjacency_between(NodeKey a, NodeKey b) const noexcept;
  [[nodiscard]] std::uint32_t degree(NodeKey key) const noexcept;
  [[nodiscard]] std::uint32_t node_count() const noexcept { return node_count_; }
  [[nodiscard]] std::uint64_t edge_count() const noexcept { return static_cast<std::uint64_t>(edges_.size()); }
  [[nodiscard]] std::vector<NodeKey> nodes() const;

  /// Structural reachability class, computed from enabled structural links only.
  [[nodiscard]] ReachabilityClass structural_class(NodeKey a, NodeKey b) const noexcept;

  /// Spines adjacent to both leaves over c LinkClass::LeafSpine links,
  /// ascending. Structural only.
  [[nodiscard]] std::vector<SpineId> common_spines(LeafId a, LeafId b) const;

  /// Bounded simple-path enumeration (config-only). Used by the reachability
  /// explanation and by tests; the enumeration never exceeds p max_hops hops
  /// or p max_paths paths so it cannot blow up on a hostile fabric.
  [[nodiscard]] std::vector<std::vector<NodeKey>> enumerate_paths(NodeKey a, NodeKey b, std::uint32_t max_hops,
                                                                  std::uint32_t max_paths) const;

  /// Independent structural checks used by the differential tests: every edge
  /// endpoint exists as a node in this generation.
  [[nodiscard]] bool all_edges_resolve() const noexcept;

 private:
  /// Slot of a node, resolved by binary search over the sorted key vector. A
  /// missing node yields c kNoSlot.
  [[nodiscard]] std::size_t node_slot(NodeKey key) const noexcept;

  static constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);

  std::shared_ptr<const Fabric> owner_{};
  const Fabric* fabric_{nullptr};        // == owner_.get(); cached for fast access
  std::vector<NodeKey> keys_;            // sorted; slot i describes keys_[i]
  std::vector<std::uint32_t> offsets_;   // size node_count_ + 1
  std::vector<Adjacency> edges_;
  // Parallel compressed indexes per link class, so class-restricted queries
  // return spans without allocating.
  std::vector<std::uint32_t> leaf_spine_offsets_;
  std::vector<Adjacency> edges_leaf_spine_;
  std::vector<std::uint32_t> peer_offsets_;
  std::vector<Adjacency> edges_peer_;
  std::uint32_t node_count_{0};
  std::uint32_t leaf_count_{0};
  std::uint32_t spine_count_{0};
};

}  // namespace slf
