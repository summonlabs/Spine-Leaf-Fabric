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
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace slf {

/// Strongly typed identities.
///
/// Every identifier in the model is a distinct type. A c LeafId can never be
/// passed where a c SpineId, c PortId, c LinkId, generation, epoch, or lease
/// is expected, and arithmetic on identifiers is not provided.
template <class Tag, class Rep>
struct StrongId {
  using rep_type = Rep;
  using tag_type = Tag;

  Rep value{};

  constexpr StrongId() noexcept = default;
  constexpr explicit StrongId(Rep raw) noexcept : value(raw) {}

  [[nodiscard]] constexpr bool is_zero() const noexcept { return value == Rep{0}; }

  friend constexpr bool operator==(StrongId a, StrongId b) noexcept { return a.value == b.value; }
  friend constexpr bool operator!=(StrongId a, StrongId b) noexcept { return a.value != b.value; }
  friend constexpr bool operator<(StrongId a, StrongId b) noexcept { return a.value < b.value; }
  friend constexpr bool operator>(StrongId a, StrongId b) noexcept { return a.value > b.value; }
  friend constexpr bool operator<=(StrongId a, StrongId b) noexcept { return a.value <= b.value; }
  friend constexpr bool operator>=(StrongId a, StrongId b) noexcept { return a.value >= b.value; }
};

struct FabricTag;
struct LeafTag;
struct SpineTag;
struct PortTag;
struct LinkTag;
struct FailureDomainTag;
struct TopologyGenerationTag;
struct EpochTag;
struct RoleIncarnationTag;
struct LeaseTag;
struct RequestTag;
struct SequenceTag;

using FabricId = StrongId<FabricTag, std::uint64_t>;
using LeafId = StrongId<LeafTag, std::uint32_t>;
using SpineId = StrongId<SpineTag, std::uint32_t>;
using PortId = StrongId<PortTag, std::uint32_t>;
using LinkId = StrongId<LinkTag, std::uint32_t>;
using FailureDomainId = StrongId<FailureDomainTag, std::uint32_t>;
using TopologyGeneration = StrongId<TopologyGenerationTag, std::uint64_t>;
using Epoch = StrongId<EpochTag, std::uint64_t>;
using RoleIncarnation = StrongId<RoleIncarnationTag, std::uint64_t>;
using LeaseId = StrongId<LeaseTag, std::uint64_t>;
using RequestId = StrongId<RequestTag, std::uint64_t>;
using SequenceNumber = StrongId<SequenceTag, std::uint64_t>;

/// Fabric tier.
enum class Tier : std::uint8_t {
  Leaf = 0,
  Spine = 1,
};

[[nodiscard]] std::string_view to_string(Tier tier) noexcept;
[[nodiscard]] std::optional<Tier> parse_tier(std::string_view text) noexcept;

/// Position of a node in the fabric: a tier plus the identifier index within
/// that tier. The tier is part of the identity, so c leaf:1 and c spine:1 are
/// different nodes.
struct NodeKey {
  Tier tier{Tier::Leaf};
  std::uint32_t index{0};

  constexpr NodeKey() noexcept = default;
  constexpr NodeKey(Tier t, std::uint32_t i) noexcept : tier(t), index(i) {}
  constexpr NodeKey(LeafId id) noexcept : tier(Tier::Leaf), index(id.value) {}
  constexpr NodeKey(SpineId id) noexcept : tier(Tier::Spine), index(id.value) {}

  [[nodiscard]] constexpr LeafId as_leaf() const noexcept { return LeafId{index}; }
  [[nodiscard]] constexpr SpineId as_spine() const noexcept { return SpineId{index}; }

  friend constexpr bool operator==(NodeKey a, NodeKey b) noexcept {
    return a.tier == b.tier && a.index == b.index;
  }
  friend constexpr bool operator!=(NodeKey a, NodeKey b) noexcept { return !(a == b); }
  friend constexpr bool operator<(NodeKey a, NodeKey b) noexcept {
    return a.tier != b.tier ? a.tier < b.tier : a.index < b.index;
  }
  friend constexpr bool operator>(NodeKey a, NodeKey b) noexcept { return b < a; }
  friend constexpr bool operator<=(NodeKey a, NodeKey b) noexcept { return !(b < a); }
  friend constexpr bool operator>=(NodeKey a, NodeKey b) noexcept { return !(a < b); }
};

/// 128-bit controller/process incarnation. A new incarnation is minted every
/// time the controller process starts; it is the outermost fencing coordinate.
struct ControllerIncarnation {
  std::uint64_t hi{0};
  std::uint64_t lo{0};

  [[nodiscard]] constexpr bool is_zero() const noexcept { return hi == 0 && lo == 0; }
  friend constexpr bool operator==(ControllerIncarnation a, ControllerIncarnation b) noexcept {
    return a.hi == b.hi && a.lo == b.lo;
  }
  friend constexpr bool operator!=(ControllerIncarnation a, ControllerIncarnation b) noexcept {
    return !(a == b);
  }
  friend constexpr bool operator<(ControllerIncarnation a, ControllerIncarnation b) noexcept {
    return a.hi != b.hi ? a.hi < b.hi : a.lo < b.lo;
  }
};

/// Formats an incarnation as 32 lower-case hex digits.
[[nodiscard]] std::string to_string(ControllerIncarnation incarnation);

/// Parses exactly 32 hex digits (optionally prefixed with c 0x).
[[nodiscard]] std::optional<ControllerIncarnation> parse_incarnation(std::string_view text) noexcept;

/// Mints a fresh incarnation from the process id, a monotonic clock reading, and
/// a process-local counter. Two calls in the same process never return equal
/// values, and two processes with distinct pids never collide.
[[nodiscard]] ControllerIncarnation mint_incarnation() noexcept;

// ---- Formatting (stable, documented, and used by CLI output) ----------------

[[nodiscard]] std::string to_string(FabricId id);
[[nodiscard]] std::string to_string(LeafId id);
[[nodiscard]] std::string to_string(SpineId id);
[[nodiscard]] std::string to_string(PortId id);
[[nodiscard]] std::string to_string(LinkId id);
[[nodiscard]] std::string to_string(FailureDomainId id);
[[nodiscard]] std::string to_string(NodeKey key);
template <class Tag, class Rep>
[[nodiscard]] std::string to_string(StrongId<Tag, Rep> id) {
  return std::to_string(id.value);
}

// ---- Parsing (strict: the whole input must be consumed) ---------------------

[[nodiscard]] std::optional<std::uint64_t> parse_u64(std::string_view text) noexcept;
[[nodiscard]] std::optional<std::uint32_t> parse_u32(std::string_view text) noexcept;
[[nodiscard]] std::optional<NodeKey> parse_node_key(std::string_view text) noexcept;
[[nodiscard]] std::optional<LeafId> parse_leaf_id(std::string_view text) noexcept;
[[nodiscard]] std::optional<SpineId> parse_spine_id(std::string_view text) noexcept;

/// True when p text is well formed UTF-8 (no overlong forms, no surrogate
/// code points, no code points above U+10FFFF).
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

}  // namespace slf

template <class Tag, class Rep>
struct std::hash<slf::StrongId<Tag, Rep>> {
  [[nodiscard]] std::size_t operator()(slf::StrongId<Tag, Rep> id) const noexcept {
    return std::hash<Rep>{}(id.value);
  }
};

template <>
struct std::hash<slf::NodeKey> {
  [[nodiscard]] std::size_t operator()(slf::NodeKey key) const noexcept {
    return (std::hash<std::uint32_t>{}(key.index) << 1U) ^
           std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(key.tier));
  }
};

template <>
struct std::hash<slf::ControllerIncarnation> {
  [[nodiscard]] std::size_t operator()(slf::ControllerIncarnation value) const noexcept {
    return std::hash<std::uint64_t>{}(value.hi) ^ (std::hash<std::uint64_t>{}(value.lo) << 1U);
  }
};
