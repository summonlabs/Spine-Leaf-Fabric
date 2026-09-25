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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "slf/digest.hpp"
#include "slf/identity.hpp"
#include "slf/status.hpp"
#include "slf/version.hpp"

namespace slf {

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

/// Role of a node in the leaf tier. Every leaf role belongs to the leaf tier;
/// a record that carries a spine role in the leaf tier is contradictory.
enum class LeafRole : std::uint8_t {
  /// Plain host-facing access leaf.
  Access = 0,
  /// Top-of-rack leaf.
  Tor = 1,
  /// Leaf allowed to carry same-tier peer links toward other leaves.
  BorderLeaf = 2,
  /// Leaf dedicated to service/appliance attachment.
  ServiceLeaf = 3,
};

/// Role of a node in the spine tier.
enum class SpineRole : std::uint8_t {
  /// Ordinary fabric spine.
  FabricSpine = 0,
  /// Spine that aggregates other spines in a multi-stage fabric. It is still a
  /// spine-tier node: this runtime does not model a third tier.
  SuperSpine = 1,
};

/// Administrative lifecycle state of a node. Durable configuration.
enum class NodeState : std::uint8_t {
  Active = 0,
  /// Temporarily removed from service, expected back.
  Maintenance = 1,
  /// Being drained: existing paths may finish, no new eligibility.
  Draining = 2,
  /// Declared failed by an operator or an external observer.
  Failed = 3,
  /// Permanently removed from the fabric.
  Retired = 4,
};

/// Administrative state of a port. Durable configuration.
enum class PortAdmin : std::uint8_t {
  Enabled = 0,
  Disabled = 1,
};

/// Administrative state of a link. Durable configuration.
enum class LinkAdmin : std::uint8_t {
  Enabled = 0,
  Disabled = 1,
};

/// Structural class of an adjacency.
enum class LinkClass : std::uint8_t {
  /// leaf <-> spine. Always legal.
  LeafSpine = 0,
  /// leaf <-> leaf or spine <-> spine. Legal only when the fabric policy
  /// enables same-tier adjacency and both endpoints are peer capable.
  SameTierPeer = 1,
};

/// Administrative state of the whole fabric.
enum class FabricState : std::uint8_t {
  /// Not yet formed (initial state of a store with no applied topology).
  Unformed = 0,
  /// Membership is being established; not eligible for paths.
  Forming = 1,
  /// Normal operation.
  Operational = 2,
  /// Administrative drain of the whole fabric.
  Draining = 3,
  /// Retired; kept for evidence only.
  Retired = 4,
};

/// How the runtime treats observed (dynamic) link evidence when deciding
/// operational eligibility.
enum class EvidencePolicy : std::uint8_t {
  /// Eligibility is decided from durable configuration only. Observed evidence
  /// is recorded but not required. Suitable for offline analysis.
  ConfigOnly = 0,
  /// A link is operationally eligible only while fresh evidence observed in the
  /// current incarnation and generation says it is up.
  RequireFresh = 1,
};

/// Freshness of an evidence record relative to a (controller, epoch,
/// generation) coordinate.
enum class EvidenceFreshness : std::uint8_t {
  /// Observed by the current incarnation at the current generation.
  Fresh = 0,
  /// Observed by an earlier incarnation or at an earlier generation. Retained
  /// as history; never silently treated as current.
  Historical = 1,
  /// No evidence record exists for the link.
  Absent = 2,
};

/// Observation of a link reported by an observer.
enum class LinkObservation : std::uint8_t {
  Up = 0,
  Down = 1,
  /// The observer could not determine the state. Never promotes a link.
  Unknown = 2,
};

/// Aggregate health classification computed from the active generation.
enum class FabricHealth : std::uint8_t {
  /// Every active leaf has at least one eligible spine and every active node is
  /// in the active state.
  Operational = 0,
  /// Some element is impaired: a node is failed/maintenance/draining, a leaf
  /// has no eligible spine, or capacity policy is violated.
  Degraded = 1,
  /// The fabric is administratively draining.
  Draining = 2,
  /// No topology has been formed yet.
  Unformed = 3,
  /// The fabric is administratively retired.
  Retired = 4,
  /// Health cannot be decided from the available evidence (for example with
  /// c EvidencePolicy::RequireFresh right after a restart).
  Indeterminate = 5,
};

[[nodiscard]] std::string_view to_string(LeafRole role) noexcept;
[[nodiscard]] std::string_view to_string(SpineRole role) noexcept;
[[nodiscard]] std::string_view to_string(NodeState state) noexcept;
[[nodiscard]] std::string_view to_string(PortAdmin state) noexcept;
[[nodiscard]] std::string_view to_string(LinkAdmin state) noexcept;
[[nodiscard]] std::string_view to_string(LinkClass cls) noexcept;
[[nodiscard]] std::string_view to_string(FabricState state) noexcept;
[[nodiscard]] std::string_view to_string(EvidencePolicy policy) noexcept;
[[nodiscard]] std::string_view to_string(EvidenceFreshness freshness) noexcept;
[[nodiscard]] std::string_view to_string(LinkObservation observation) noexcept;
[[nodiscard]] std::string_view to_string(FabricHealth health) noexcept;

[[nodiscard]] std::optional<LeafRole> parse_leaf_role(std::string_view text) noexcept;
[[nodiscard]] std::optional<SpineRole> parse_spine_role(std::string_view text) noexcept;
[[nodiscard]] std::optional<NodeState> parse_node_state(std::string_view text) noexcept;
[[nodiscard]] std::optional<PortAdmin> parse_port_admin(std::string_view text) noexcept;
[[nodiscard]] std::optional<LinkAdmin> parse_link_admin(std::string_view text) noexcept;
[[nodiscard]] std::optional<FabricState> parse_fabric_state(std::string_view text) noexcept;
[[nodiscard]] std::optional<EvidencePolicy> parse_evidence_policy(std::string_view text) noexcept;
[[nodiscard]] std::optional<LinkObservation> parse_link_observation(std::string_view text) noexcept;

/// True when a role belongs to the given tier.
[[nodiscard]] bool role_belongs_to_tier(Tier tier, LeafRole role) noexcept;
[[nodiscard]] bool role_belongs_to_tier(Tier tier, SpineRole role) noexcept;

/// True when a leaf role may terminate same-tier peer links.
[[nodiscard]] bool is_peer_capable(LeafRole role) noexcept;

/// True when a node in this state can carry new paths.
[[nodiscard]] bool state_is_serving(NodeState state) noexcept;

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

struct FailureDomainRecord {
  FailureDomainId id{};
  std::string name;
  TopologyGeneration generation{};

  friend bool operator==(const FailureDomainRecord&, const FailureDomainRecord&) = default;
};

struct LeafRecord {
  LeafId id{};
  std::string name;
  LeafRole role{LeafRole::Access};
  /// Incremented whenever the role or tier placement of this node is
  /// re-asserted with different meaning. Part of the identity coordinate: a
  /// path authority issued for incarnation N is fenced when the incarnation
  /// advances.
  RoleIncarnation role_incarnation{};
  FailureDomainId domain{};
  NodeState state{NodeState::Active};
  /// Downlink capacity toward hosts, in megabits per second.
  std::uint64_t access_capacity_mbps{0};
  /// Number of host-facing links, used only for reporting.
  std::uint32_t access_link_count{0};
  TopologyGeneration generation{};

  friend bool operator==(const LeafRecord&, const LeafRecord&) = default;
};

struct SpineRecord {
  SpineId id{};
  std::string name;
  SpineRole role{SpineRole::FabricSpine};
  RoleIncarnation role_incarnation{};
  FailureDomainId domain{};
  NodeState state{NodeState::Active};
  /// Total fabric-facing capacity of this spine, in megabits per second.
  std::uint64_t fabric_capacity_mbps{0};
  TopologyGeneration generation{};

  friend bool operator==(const SpineRecord&, const SpineRecord&) = default;
};

struct PortRecord {
  PortId id{};
  NodeKey owner{};
  /// Port speed in megabits per second. Must be non-zero for an enabled port.
  std::uint64_t speed_mbps{0};
  PortAdmin admin{PortAdmin::Enabled};
  TopologyGeneration generation{};

  friend bool operator==(const PortRecord&, const PortRecord&) = default;
};

struct LinkRecord {
  LinkId id{};
  NodeKey a{};
  PortId port_a{};
  NodeKey b{};
  PortId port_b{};
  LinkClass cls{LinkClass::LeafSpine};
  LinkAdmin admin{LinkAdmin::Enabled};
  /// Declared link speed. Zero means "derive from the slower endpoint port".
  /// A declaration above the slower port speed is contradictory.
  std::uint64_t declared_speed_mbps{0};
  TopologyGeneration generation{};

  friend bool operator==(const LinkRecord&, const LinkRecord&) = default;

  [[nodiscard]] bool touches(NodeKey key) const noexcept { return a == key || b == key; }
  [[nodiscard]] NodeKey other(NodeKey key) const noexcept { return a == key ? b : a; }
};

/// Observed link state. Dynamic evidence: it is never durable configuration and
/// it is fenced by (incarnation, epoch, generation).
struct LinkEvidence {
  LinkId link{};
  LinkObservation observed{LinkObservation::Unknown};
  TopologyGeneration generation{};
  ControllerIncarnation observer{};
  Epoch epoch{};
  SequenceNumber observed_seq{};
  std::uint64_t observed_at_unix_ms{0};

  friend bool operator==(const LinkEvidence&, const LinkEvidence&) = default;
};

// ---------------------------------------------------------------------------
// Policy and limits
// ---------------------------------------------------------------------------

struct FabricOptions {
  /// Allow same-tier adjacency (leaf<->leaf, spine<->spine). When false such a
  /// link is a structural error. When true it is still only legal between
  /// peer-capable nodes and is classified as c LinkClass::SameTierPeer.
  bool allow_same_tier_adjacency{false};
  /// Default diversity requirement for eligibility queries. Per-query
  /// constraints can tighten or relax it.
  bool require_domain_diversity{false};
  /// Minimum number of distinct eligible spines for a leaf-to-leaf path.
  std::uint32_t min_eligible_spines{1};
  /// Evidence policy for operational eligibility.
  EvidencePolicy evidence_policy{EvidencePolicy::ConfigOnly};

  friend bool operator==(const FabricOptions&, const FabricOptions&) = default;
};

/// Upper bounds applied by the builder. Every count from an external source is
/// validated against these before allocation.
struct BuilderLimits {
  std::uint32_t max_leaves{4096};
  std::uint32_t max_spines{1024};
  std::uint32_t max_ports{65536};
  std::uint32_t max_links{131072};
  std::uint32_t max_failure_domains{4096};
  std::uint32_t max_evidence_records{131072};
  std::uint32_t max_history_records{4096};
  std::uint32_t max_name_bytes{256};
  /// 1 Pb/s. Any declared capacity above this is rejected as invalid rather
  /// than propagated.
  std::uint64_t max_capacity_mbps{1'000'000'000'000ULL};

  friend bool operator==(const BuilderLimits&, const BuilderLimits&) = default;
};

// ---------------------------------------------------------------------------
// Validation report
// ---------------------------------------------------------------------------

enum class IssueSeverity : std::uint8_t {
  Info = 0,
  Warning = 1,
  Error = 2,
};

[[nodiscard]] std::string_view to_string(IssueSeverity severity) noexcept;

struct ValidationIssue {
  IssueSeverity severity{IssueSeverity::Info};
  StatusCode code{StatusCode::Ok};
  std::string where;    // "leaf:3", "link:7/port_b", "fabric"
  std::string detail;
};

struct ValidationReport {
  std::vector<ValidationIssue> issues{};
  std::uint32_t leaf_count{0};
  std::uint32_t spine_count{0};
  std::uint32_t port_count{0};
  std::uint32_t link_count{0};
  std::uint32_t domain_count{0};
  std::uint32_t history_count{0};
  std::uint32_t evidence_count{0};
  std::uint32_t error_count{0};
  std::uint32_t warning_count{0};

  [[nodiscard]] bool structural_ok() const noexcept { return error_count == 0; }
  [[nodiscard]] bool has_code(StatusCode code) const noexcept;
  [[nodiscard]] std::string summary() const;
  [[nodiscard]] std::string to_string() const;
  /// Deterministic digest of the ordered issue list; two identical reports
  /// always hash the same.
  [[nodiscard]] Digest digest() const;
};

// ---------------------------------------------------------------------------
// Fabric
// ---------------------------------------------------------------------------

/// An immutable, validated fabric snapshot for exactly one topology generation.
///
/// Records are sorted by identity, so iteration order never depends on the
/// order in which the records were supplied. Retired (older-generation) links
/// are retained separately as history and never participate in eligibility.
struct Fabric {
  FabricId id{};
  std::string name;
  std::string site;
  TopologyGeneration generation{};
  Epoch epoch{};
  FabricState state{FabricState::Unformed};
  FabricOptions options{};
  std::vector<FailureDomainRecord> domains;
  std::vector<LeafRecord> leaves;
  std::vector<SpineRecord> spines;
  std::vector<PortRecord> ports;
  std::vector<LinkRecord> links;
  std::vector<LinkRecord> history_links;
  std::vector<LinkEvidence> evidence;
  Digest topology_digest{};
  Digest evidence_digest{};

  [[nodiscard]] bool empty() const noexcept { return leaves.empty() && spines.empty(); }
  [[nodiscard]] std::uint32_t active_leaf_count() const noexcept;
  [[nodiscard]] std::uint32_t active_spine_count() const noexcept;

  [[nodiscard]] const LeafRecord* find_leaf(LeafId id) const noexcept;
  [[nodiscard]] const SpineRecord* find_spine(SpineId id) const noexcept;
  [[nodiscard]] const PortRecord* find_port(PortId id) const noexcept;
  [[nodiscard]] const LinkRecord* find_link(LinkId id) const noexcept;
  [[nodiscard]] const FailureDomainRecord* find_domain(FailureDomainId id) const noexcept;

  /// Node state for either tier; c std::nullopt when the node does not exist
  /// in the active generation.
  [[nodiscard]] std::optional<NodeState> node_state(NodeKey key) const noexcept;
  [[nodiscard]] std::optional<RoleIncarnation> node_role_incarnation(NodeKey key) const noexcept;
  [[nodiscard]] std::optional<FailureDomainId> node_domain(NodeKey key) const noexcept;
  [[nodiscard]] std::optional<std::string_view> node_name(NodeKey key) const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> node_nominal_capacity_mbps(NodeKey key) const noexcept;

  /// Effective speed of a link: the declared speed when non-zero, otherwise the
  /// slower of the two endpoint ports. Returns c std::nullopt when a port is
  /// missing, and reports overflow-free min selection.
  [[nodiscard]] std::optional<std::uint64_t> effective_link_speed_mbps(const LinkRecord& link) const noexcept;

  /// Evidence record with the highest observation sequence that is bound to the
  /// link. When several records share the sequence the later generation wins;
  /// ties are reported as c Conflicting by c evidence_for().
  [[nodiscard]] Outcome<const LinkEvidence*> evidence_for(LinkId link) const;
  [[nodiscard]] EvidenceFreshness evidence_freshness(const LinkEvidence& evidence,
                                                     ControllerIncarnation controller,
                                                     Epoch epoch) const noexcept;
};

// ---------------------------------------------------------------------------
// Canonical encoding and digests
// ---------------------------------------------------------------------------

/// Canonical byte encoding of the structural records of p fabric. Independent
/// of insertion order: the encoding is produced from the sorted record vectors.
[[nodiscard]] std::vector<std::byte> canonical_topology_bytes(const Fabric& fabric);

/// Canonical byte encoding of the dynamic evidence records.
[[nodiscard]] std::vector<std::byte> canonical_evidence_bytes(const Fabric& fabric);

/// Recomputes both digests from the canonical encoding.
[[nodiscard]] std::pair<Digest, Digest> compute_digests(const Fabric& fabric);

/// Full serialization used by snapshots and the wire protocol. Deterministic
/// and stable across processes.
[[nodiscard]] std::vector<std::byte> serialize_fabric(const Fabric& fabric);

/// Parses bytes produced by c serialize_fabric. Every count is validated
/// against p limits before allocation, and trailing bytes are rejected.
[[nodiscard]] Outcome<Fabric> deserialize_fabric(const std::span<const std::byte>& data,
                                                 const BuilderLimits& limits);

/// Human readable multi-line rendering used by c slfctl inspect.
[[nodiscard]] std::string describe_fabric(const Fabric& fabric);

}  // namespace slf
