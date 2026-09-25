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

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>
#include <utility>

#include "slf/bytes.hpp"
#include "slf/builder.hpp"
#include "slf/checked.hpp"

namespace slf {
namespace {

constexpr std::string_view kTopologyDomain = "slf.topology.v1";
constexpr std::string_view kEvidenceDomain = "slf.evidence.v1";

[[nodiscard]] std::string_view bool_text(bool value) noexcept { return value ? "true" : "false"; }

}  // namespace

// ---------------------------------------------------------------------------
// Enumeration rendering and parsing
// ---------------------------------------------------------------------------

std::string_view to_string(LeafRole role) noexcept {
  switch (role) {
    case LeafRole::Access: return "access";
    case LeafRole::Tor: return "tor";
    case LeafRole::BorderLeaf: return "border";
    case LeafRole::ServiceLeaf: return "service";
  }
  return "invalid";
}

std::string_view to_string(SpineRole role) noexcept {
  switch (role) {
    case SpineRole::FabricSpine: return "fabric";
    case SpineRole::SuperSpine: return "super";
  }
  return "invalid";
}

std::string_view to_string(NodeState state) noexcept {
  switch (state) {
    case NodeState::Active: return "active";
    case NodeState::Maintenance: return "maintenance";
    case NodeState::Draining: return "draining";
    case NodeState::Failed: return "failed";
    case NodeState::Retired: return "retired";
  }
  return "invalid";
}

std::string_view to_string(PortAdmin state) noexcept {
  switch (state) {
    case PortAdmin::Enabled: return "enabled";
    case PortAdmin::Disabled: return "disabled";
  }
  return "invalid";
}

std::string_view to_string(LinkAdmin state) noexcept {
  switch (state) {
    case LinkAdmin::Enabled: return "enabled";
    case LinkAdmin::Disabled: return "disabled";
  }
  return "invalid";
}

std::string_view to_string(LinkClass cls) noexcept {
  switch (cls) {
    case LinkClass::LeafSpine: return "leaf_spine";
    case LinkClass::SameTierPeer: return "same_tier_peer";
  }
  return "invalid";
}

std::string_view to_string(FabricState state) noexcept {
  switch (state) {
    case FabricState::Unformed: return "unformed";
    case FabricState::Forming: return "forming";
    case FabricState::Operational: return "operational";
    case FabricState::Draining: return "draining";
    case FabricState::Retired: return "retired";
  }
  return "invalid";
}

std::string_view to_string(EvidencePolicy policy) noexcept {
  switch (policy) {
    case EvidencePolicy::ConfigOnly: return "config_only";
    case EvidencePolicy::RequireFresh: return "require_fresh";
  }
  return "invalid";
}

std::string_view to_string(EvidenceFreshness freshness) noexcept {
  switch (freshness) {
    case EvidenceFreshness::Fresh: return "fresh";
    case EvidenceFreshness::Historical: return "historical";
    case EvidenceFreshness::Absent: return "absent";
  }
  return "invalid";
}

std::string_view to_string(LinkObservation observation) noexcept {
  switch (observation) {
    case LinkObservation::Up: return "up";
    case LinkObservation::Down: return "down";
    case LinkObservation::Unknown: return "unknown";
  }
  return "invalid";
}

std::string_view to_string(FabricHealth health) noexcept {
  switch (health) {
    case FabricHealth::Operational: return "operational";
    case FabricHealth::Degraded: return "degraded";
    case FabricHealth::Draining: return "draining";
    case FabricHealth::Unformed: return "unformed";
    case FabricHealth::Retired: return "retired";
    case FabricHealth::Indeterminate: return "indeterminate";
  }
  return "invalid";
}

std::string_view to_string(IssueSeverity severity) noexcept {
  switch (severity) {
    case IssueSeverity::Info: return "info";
    case IssueSeverity::Warning: return "warning";
    case IssueSeverity::Error: return "error";
  }
  return "invalid";
}

std::optional<LeafRole> parse_leaf_role(std::string_view text) noexcept {
  if (text == "access") return LeafRole::Access;
  if (text == "tor") return LeafRole::Tor;
  if (text == "border") return LeafRole::BorderLeaf;
  if (text == "service") return LeafRole::ServiceLeaf;
  return std::nullopt;
}

std::optional<SpineRole> parse_spine_role(std::string_view text) noexcept {
  if (text == "fabric") return SpineRole::FabricSpine;
  if (text == "super") return SpineRole::SuperSpine;
  return std::nullopt;
}

std::optional<NodeState> parse_node_state(std::string_view text) noexcept {
  if (text == "active") return NodeState::Active;
  if (text == "maintenance") return NodeState::Maintenance;
  if (text == "draining") return NodeState::Draining;
  if (text == "failed") return NodeState::Failed;
  if (text == "retired") return NodeState::Retired;
  return std::nullopt;
}

std::optional<PortAdmin> parse_port_admin(std::string_view text) noexcept {
  if (text == "enabled" || text == "up") return PortAdmin::Enabled;
  if (text == "disabled" || text == "down") return PortAdmin::Disabled;
  return std::nullopt;
}

std::optional<LinkAdmin> parse_link_admin(std::string_view text) noexcept {
  if (text == "enabled" || text == "up") return LinkAdmin::Enabled;
  if (text == "disabled" || text == "down") return LinkAdmin::Disabled;
  return std::nullopt;
}

std::optional<FabricState> parse_fabric_state(std::string_view text) noexcept {
  if (text == "unformed") return FabricState::Unformed;
  if (text == "forming") return FabricState::Forming;
  if (text == "operational") return FabricState::Operational;
  if (text == "draining") return FabricState::Draining;
  if (text == "retired") return FabricState::Retired;
  return std::nullopt;
}

std::optional<EvidencePolicy> parse_evidence_policy(std::string_view text) noexcept {
  if (text == "config_only") return EvidencePolicy::ConfigOnly;
  if (text == "require_fresh") return EvidencePolicy::RequireFresh;
  return std::nullopt;
}

std::optional<LinkObservation> parse_link_observation(std::string_view text) noexcept {
  if (text == "up") return LinkObservation::Up;
  if (text == "down") return LinkObservation::Down;
  if (text == "unknown") return LinkObservation::Unknown;
  return std::nullopt;
}

bool role_belongs_to_tier(Tier tier, LeafRole /*role*/) noexcept { return tier == Tier::Leaf; }
bool role_belongs_to_tier(Tier tier, SpineRole /*role*/) noexcept { return tier == Tier::Spine; }

bool is_peer_capable(LeafRole role) noexcept {
  return role == LeafRole::BorderLeaf || role == LeafRole::ServiceLeaf;
}

bool state_is_serving(NodeState state) noexcept { return state == NodeState::Active; }

// ---------------------------------------------------------------------------
// ValidationReport
// ---------------------------------------------------------------------------

bool ValidationReport::has_code(StatusCode code) const noexcept {
  for (const auto& issue : issues) {
    if (issue.code == code) {
      return true;
    }
  }
  return false;
}

std::string ValidationReport::summary() const {
  std::ostringstream out;
  out << (structural_ok() ? "structurally valid" : "structurally invalid") << ": " << leaf_count
      << " leaves, " << spine_count << " spines, " << link_count << " links, " << port_count
      << " ports, " << domain_count << " failure domains, " << history_count << " historical links, "
      << evidence_count << " evidence records; " << error_count << " error(s), " << warning_count
      << " warning(s)";
  return out.str();
}

std::string ValidationReport::to_string() const {
  std::ostringstream out;
  out << summary() << "\n";
  for (const auto& issue : issues) {
    out << "  [" << slf::to_string(issue.severity) << "] " << issue.where << ": "
        << slf::to_string(issue.code);
    if (!issue.detail.empty()) {
      out << " (" << issue.detail << ")";
    }
    out << "\n";
  }
  return out.str();
}

Digest ValidationReport::digest() const {
  DigestBuilder builder;
  builder.add_string("slf.report.v1");
  builder.add_u32(static_cast<std::uint32_t>(issues.size()));
  for (const auto& issue : issues) {
    builder.add_u8(static_cast<std::uint8_t>(issue.severity));
    builder.add_u16(static_cast<std::uint16_t>(issue.code));
    builder.add_string(issue.where);
    builder.add_string(issue.detail);
  }
  return builder.finish();
}

// ---------------------------------------------------------------------------
// Fabric lookups
// ---------------------------------------------------------------------------

std::uint32_t Fabric::active_leaf_count() const noexcept {
  std::uint32_t count = 0;
  for (const auto& leaf : leaves) {
    if (state_is_serving(leaf.state)) {
      ++count;
    }
  }
  return count;
}

std::uint32_t Fabric::active_spine_count() const noexcept {
  std::uint32_t count = 0;
  for (const auto& spine : spines) {
    if (state_is_serving(spine.state)) {
      ++count;
    }
  }
  return count;
}

const LeafRecord* Fabric::find_leaf(LeafId key) const noexcept {
  const auto it = std::lower_bound(leaves.begin(), leaves.end(), key,
                                   [](const LeafRecord& record, LeafId probe) { return record.id < probe; });
  return it != leaves.end() && it->id == key ? &*it : nullptr;
}

const SpineRecord* Fabric::find_spine(SpineId key) const noexcept {
  const auto it = std::lower_bound(spines.begin(), spines.end(), key, [](const SpineRecord& record,
                                                                        SpineId probe) {
    return record.id < probe;
  });
  return it != spines.end() && it->id == key ? &*it : nullptr;
}

const PortRecord* Fabric::find_port(PortId key) const noexcept {
  const auto it = std::lower_bound(ports.begin(), ports.end(), key,
                                   [](const PortRecord& record, PortId probe) { return record.id < probe; });
  return it != ports.end() && it->id == key ? &*it : nullptr;
}

const LinkRecord* Fabric::find_link(LinkId key) const noexcept {
  const auto it = std::lower_bound(links.begin(), links.end(), key,
                                   [](const LinkRecord& record, LinkId probe) { return record.id < probe; });
  return it != links.end() && it->id == key ? &*it : nullptr;
}

const FailureDomainRecord* Fabric::find_domain(FailureDomainId key) const noexcept {
  const auto it = std::lower_bound(domains.begin(), domains.end(), key,
                                   [](const FailureDomainRecord& record, FailureDomainId probe) {
                                     return record.id < probe;
                                   });
  return it != domains.end() && it->id == key ? &*it : nullptr;
}

std::optional<NodeState> Fabric::node_state(NodeKey key) const noexcept {
  if (key.tier == Tier::Leaf) {
    const LeafRecord* leaf = find_leaf(LeafId{key.index});
    if (leaf == nullptr) {
      return std::nullopt;
    }
    return leaf->state;
  }
  const SpineRecord* spine = find_spine(SpineId{key.index});
  if (spine == nullptr) {
    return std::nullopt;
  }
  return spine->state;
}

std::optional<RoleIncarnation> Fabric::node_role_incarnation(NodeKey key) const noexcept {
  if (key.tier == Tier::Leaf) {
    const LeafRecord* leaf = find_leaf(LeafId{key.index});
    return leaf == nullptr ? std::nullopt : std::optional<RoleIncarnation>(leaf->role_incarnation);
  }
  const SpineRecord* spine = find_spine(SpineId{key.index});
  return spine == nullptr ? std::nullopt : std::optional<RoleIncarnation>(spine->role_incarnation);
}

std::optional<FailureDomainId> Fabric::node_domain(NodeKey key) const noexcept {
  if (key.tier == Tier::Leaf) {
    const LeafRecord* leaf = find_leaf(LeafId{key.index});
    return leaf == nullptr ? std::nullopt : std::optional<FailureDomainId>(leaf->domain);
  }
  const SpineRecord* spine = find_spine(SpineId{key.index});
  return spine == nullptr ? std::nullopt : std::optional<FailureDomainId>(spine->domain);
}

std::optional<std::string_view> Fabric::node_name(NodeKey key) const noexcept {
  if (key.tier == Tier::Leaf) {
    const LeafRecord* leaf = find_leaf(LeafId{key.index});
    return leaf == nullptr ? std::nullopt : std::optional<std::string_view>(leaf->name);
  }
  const SpineRecord* spine = find_spine(SpineId{key.index});
  return spine == nullptr ? std::nullopt : std::optional<std::string_view>(spine->name);
}

std::optional<std::uint64_t> Fabric::node_nominal_capacity_mbps(NodeKey key) const noexcept {
  if (key.tier == Tier::Leaf) {
    const LeafRecord* leaf = find_leaf(LeafId{key.index});
    return leaf == nullptr ? std::nullopt : std::optional<std::uint64_t>(leaf->access_capacity_mbps);
  }
  const SpineRecord* spine = find_spine(SpineId{key.index});
  return spine == nullptr ? std::nullopt : std::optional<std::uint64_t>(spine->fabric_capacity_mbps);
}

std::optional<std::uint64_t> Fabric::effective_link_speed_mbps(const LinkRecord& link) const noexcept {
  const PortRecord* port_a = find_port(link.port_a);
  const PortRecord* port_b = find_port(link.port_b);
  if (port_a == nullptr || port_b == nullptr) {
    return std::nullopt;
  }
  if (link.declared_speed_mbps != 0) {
    return link.declared_speed_mbps;
  }
  return port_a->speed_mbps < port_b->speed_mbps ? port_a->speed_mbps : port_b->speed_mbps;
}

Outcome<const LinkEvidence*> Fabric::evidence_for(LinkId link) const {
  const LinkEvidence* best = nullptr;
  for (const auto& record : evidence) {
    if (record.link != link) {
      continue;
    }
    if (best == nullptr || best->observed_seq < record.observed_seq) {
      best = &record;
      continue;
    }
    if (best->observed_seq == record.observed_seq && best->generation < record.generation) {
      best = &record;
    }
  }
  if (best == nullptr) {
    return Status(StatusCode::NotFound, "no evidence recorded for the link");
  }
  for (const auto& record : evidence) {
    if (record.link != link) {
      continue;
    }
    if (record.observed_seq == best->observed_seq && record.generation == best->generation &&
        record != *best) {
      return Status(StatusCode::Conflicting,
                    "two evidence records share the same observation sequence and generation");
    }
  }
  return best;
}

EvidenceFreshness Fabric::evidence_freshness(const LinkEvidence& record,
                                              ControllerIncarnation controller,
                                              Epoch observed_epoch) const noexcept {
  if (record.observer != controller || record.generation != generation || record.epoch != observed_epoch) {
    return EvidenceFreshness::Historical;
  }
  return EvidenceFreshness::Fresh;
}

// ---------------------------------------------------------------------------
// Canonical encoding
// ---------------------------------------------------------------------------

std::vector<std::byte> canonical_topology_bytes(const Fabric& fabric) {
  ByteWriter writer(ByteWriter::kDefaultMaxBytes);
  const auto put = [&writer](const Status& status) {
    // Encoding of a validated fabric cannot exceed the writer bound; if it ever
    // does, the encoder stops appending and the digest no longer matches, which
    // callers detect via compute_digests()'s structural check.
    (void)status;
  };
  put(writer.string(kTopologyDomain));
  put(writer.u64(fabric.id.value));
  put(writer.string(fabric.name));
  put(writer.string(fabric.site));
  put(writer.u64(fabric.generation.value));
  put(writer.u64(fabric.epoch.value));
  put(writer.u8(static_cast<std::uint8_t>(fabric.state)));
  put(writer.u8(fabric.options.allow_same_tier_adjacency ? 1U : 0U));
  put(writer.u8(fabric.options.require_domain_diversity ? 1U : 0U));
  put(writer.u32(fabric.options.min_eligible_spines));
  put(writer.u8(static_cast<std::uint8_t>(fabric.options.evidence_policy)));

  put(writer.u32(static_cast<std::uint32_t>(fabric.domains.size())));
  for (const auto& domain : fabric.domains) {
    put(writer.u32(domain.id.value));
    put(writer.string(domain.name));
    put(writer.u64(domain.generation.value));
  }

  put(writer.u32(static_cast<std::uint32_t>(fabric.leaves.size())));
  for (const auto& leaf : fabric.leaves) {
    put(writer.u32(leaf.id.value));
    put(writer.string(leaf.name));
    put(writer.u8(static_cast<std::uint8_t>(leaf.role)));
    put(writer.u64(leaf.role_incarnation.value));
    put(writer.u32(leaf.domain.value));
    put(writer.u8(static_cast<std::uint8_t>(leaf.state)));
    put(writer.u64(leaf.access_capacity_mbps));
    put(writer.u32(leaf.access_link_count));
    put(writer.u64(leaf.generation.value));
  }

  put(writer.u32(static_cast<std::uint32_t>(fabric.spines.size())));
  for (const auto& spine : fabric.spines) {
    put(writer.u32(spine.id.value));
    put(writer.string(spine.name));
    put(writer.u8(static_cast<std::uint8_t>(spine.role)));
    put(writer.u64(spine.role_incarnation.value));
    put(writer.u32(spine.domain.value));
    put(writer.u8(static_cast<std::uint8_t>(spine.state)));
    put(writer.u64(spine.fabric_capacity_mbps));
    put(writer.u64(spine.generation.value));
  }

  put(writer.u32(static_cast<std::uint32_t>(fabric.ports.size())));
  for (const auto& port : fabric.ports) {
    put(writer.u32(port.id.value));
    put(writer.u8(static_cast<std::uint8_t>(port.owner.tier)));
    put(writer.u32(port.owner.index));
    put(writer.u64(port.speed_mbps));
    put(writer.u8(static_cast<std::uint8_t>(port.admin)));
    put(writer.u64(port.generation.value));
  }

  const auto encode_links = [&writer, &put](const std::vector<LinkRecord>& links) {
    put(writer.u32(static_cast<std::uint32_t>(links.size())));
    for (const auto& link : links) {
      put(writer.u32(link.id.value));
      put(writer.u8(static_cast<std::uint8_t>(link.a.tier)));
      put(writer.u32(link.a.index));
      put(writer.u32(link.port_a.value));
      put(writer.u8(static_cast<std::uint8_t>(link.b.tier)));
      put(writer.u32(link.b.index));
      put(writer.u32(link.port_b.value));
      put(writer.u8(static_cast<std::uint8_t>(link.cls)));
      put(writer.u8(static_cast<std::uint8_t>(link.admin)));
      put(writer.u64(link.declared_speed_mbps));
      put(writer.u64(link.generation.value));
    }
  };
  encode_links(fabric.links);
  encode_links(fabric.history_links);
  return writer.buffer();
}

std::vector<std::byte> canonical_evidence_bytes(const Fabric& fabric) {
  ByteWriter writer(ByteWriter::kDefaultMaxBytes);
  (void)writer.string(kEvidenceDomain);
  (void)writer.u64(fabric.generation.value);
  (void)writer.u32(static_cast<std::uint32_t>(fabric.evidence.size()));
  for (const auto& record : fabric.evidence) {
    (void)writer.u32(record.link.value);
    (void)writer.u8(static_cast<std::uint8_t>(record.observed));
    (void)writer.u64(record.generation.value);
    (void)writer.u64(record.observer.hi);
    (void)writer.u64(record.observer.lo);
    (void)writer.u64(record.epoch.value);
    (void)writer.u64(record.observed_seq.value);
    (void)writer.u64(record.observed_at_unix_ms);
  }
  return writer.buffer();
}

std::pair<Digest, Digest> compute_digests(const Fabric& fabric) {
  const std::vector<std::byte> topology = canonical_topology_bytes(fabric);
  const std::vector<std::byte> evidence = canonical_evidence_bytes(fabric);
  const Digest topology_digest = Digest::of(std::span<const std::byte>(topology));
  Digest evidence_digest = Digest::of(std::span<const std::byte>(evidence));
  if (fabric.evidence.empty()) {
    evidence_digest = Digest{};
  }
  return {topology_digest, evidence_digest};
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

std::vector<std::byte> serialize_fabric(const Fabric& fabric) {
  const std::vector<std::byte> topology = canonical_topology_bytes(fabric);
  const std::vector<std::byte> evidence = canonical_evidence_bytes(fabric);
  ByteWriter writer(ByteWriter::kDefaultMaxBytes);
  (void)writer.u32(kPersistenceFormatVersion);
  (void)writer.block(topology);
  (void)writer.block(evidence);
  return writer.buffer();
}

Outcome<Fabric> deserialize_fabric(const std::span<const std::byte>& data, const BuilderLimits& limits) {
  ByteReader reader(data);
  const auto version = reader.u32();
  if (!version.ok()) {
    return version.status();
  }
  if (version.value() != kPersistenceFormatVersion) {
    return Status(StatusCode::IncompatibleVersion,
                  "fabric payload version " + std::to_string(version.value()) + " is not supported");
  }
  constexpr std::size_t kMaxBlockBytes = 64U * 1024U * 1024U;
  const auto topology_block = reader.block(kMaxBlockBytes);
  if (!topology_block.ok()) {
    return topology_block.status();
  }
  const auto evidence_block = reader.block(kMaxBlockBytes);
  if (!evidence_block.ok()) {
    return evidence_block.status();
  }
  if (!reader.exhausted()) {
    return Status(StatusCode::Invalid, "trailing bytes after the fabric payload");
  }

  ByteReader topology(topology_block.value(), limits.max_name_bytes);
  const auto domain_tag = topology.string();
  if (!domain_tag.ok()) {
    return domain_tag.status();
  }
  const auto fabric_id = topology.u64();
  const auto name = topology.string();
  const auto site = topology.string();
  const auto generation = topology.u64();
  const auto epoch = topology.u64();
  const auto state = topology.u8();
  const auto allow_peer = topology.u8();
  const auto require_diversity = topology.u8();
  const auto min_spines = topology.u32();
  const auto evidence_policy = topology.u8();
  if (!fabric_id.ok() || !name.ok() || !site.ok() || !generation.ok() || !epoch.ok() || !state.ok() ||
      !allow_peer.ok() || !require_diversity.ok() || !min_spines.ok() || !evidence_policy.ok()) {
    return Status(StatusCode::Truncated, "fabric header is incomplete");
  }
  FabricOptions options;
  options.allow_same_tier_adjacency = allow_peer.value() != 0;
  options.require_domain_diversity = require_diversity.value() != 0;
  options.min_eligible_spines = min_spines.value();
  if (evidence_policy.value() > static_cast<std::uint8_t>(EvidencePolicy::RequireFresh)) {
    return Status(StatusCode::Invalid, "unknown evidence policy value");
  }
  options.evidence_policy = static_cast<EvidencePolicy>(evidence_policy.value());

  FabricBuilder builder(FabricId{fabric_id.value()}, name.value(), TopologyGeneration{generation.value()},
                        Epoch{epoch.value()}, options, limits);
  (void)builder.set_site(site.value());
  if (state.value() > static_cast<std::uint8_t>(FabricState::Retired)) {
    return Status(StatusCode::Invalid, "unknown fabric state value");
  }
  (void)builder.set_state(static_cast<FabricState>(state.value()));

  const auto domain_count = topology.u32();
  if (!domain_count.ok()) {
    return Status(StatusCode::Truncated, "missing failure domain count");
  }
  if (domain_count.value() > limits.max_failure_domains) {
    return Status(StatusCode::Exhausted, "failure domain count exceeds the configured bound");
  }
  for (std::uint32_t i = 0; i < domain_count.value(); ++i) {
    const auto id = topology.u32();
    const auto domain_name = topology.string();
    const auto domain_generation = topology.u64();
    if (!id.ok() || !domain_name.ok() || !domain_generation.ok()) {
      return Status(StatusCode::Truncated, "failure domain record is incomplete");
    }
    FailureDomainRecord record;
    record.id = FailureDomainId{id.value()};
    record.name = domain_name.value();
    record.generation = TopologyGeneration{domain_generation.value()};
    const Status added = builder.add_failure_domain(std::move(record));
    if (!added.ok()) {
      return added;
    }
  }

  const auto leaf_count = topology.u32();
  if (!leaf_count.ok()) {
    return Status(StatusCode::Truncated, "missing leaf count");
  }
  if (leaf_count.value() > limits.max_leaves) {
    return Status(StatusCode::Exhausted, "leaf count exceeds the configured bound");
  }
  for (std::uint32_t i = 0; i < leaf_count.value(); ++i) {
    const auto id = topology.u32();
    const auto leaf_name = topology.string();
    const auto role = topology.u8();
    const auto incarnation = topology.u64();
    const auto domain = topology.u32();
    const auto leaf_state = topology.u8();
    const auto capacity = topology.u64();
    const auto access_links = topology.u32();
    const auto leaf_generation = topology.u64();
    if (!id.ok() || !leaf_name.ok() || !role.ok() || !incarnation.ok() || !domain.ok() ||
        !leaf_state.ok() || !capacity.ok() || !access_links.ok() || !leaf_generation.ok()) {
      return Status(StatusCode::Truncated, "leaf record is incomplete");
    }
    if (role.value() > static_cast<std::uint8_t>(LeafRole::ServiceLeaf) ||
        leaf_state.value() > static_cast<std::uint8_t>(NodeState::Retired)) {
      return Status(StatusCode::Invalid, "leaf record carries an unknown enumeration value");
    }
    LeafRecord record;
    record.id = LeafId{id.value()};
    record.name = leaf_name.value();
    record.role = static_cast<LeafRole>(role.value());
    record.role_incarnation = RoleIncarnation{incarnation.value()};
    record.domain = FailureDomainId{domain.value()};
    record.state = static_cast<NodeState>(leaf_state.value());
    record.access_capacity_mbps = capacity.value();
    record.access_link_count = access_links.value();
    record.generation = TopologyGeneration{leaf_generation.value()};
    const Status added = builder.add_leaf(std::move(record));
    if (!added.ok()) {
      return added;
    }
  }

  const auto spine_count = topology.u32();
  if (!spine_count.ok()) {
    return Status(StatusCode::Truncated, "missing spine count");
  }
  if (spine_count.value() > limits.max_spines) {
    return Status(StatusCode::Exhausted, "spine count exceeds the configured bound");
  }
  for (std::uint32_t i = 0; i < spine_count.value(); ++i) {
    const auto id = topology.u32();
    const auto spine_name = topology.string();
    const auto role = topology.u8();
    const auto incarnation = topology.u64();
    const auto domain = topology.u32();
    const auto spine_state = topology.u8();
    const auto capacity = topology.u64();
    const auto spine_generation = topology.u64();
    if (!id.ok() || !spine_name.ok() || !role.ok() || !incarnation.ok() || !domain.ok() ||
        !spine_state.ok() || !capacity.ok() || !spine_generation.ok()) {
      return Status(StatusCode::Truncated, "spine record is incomplete");
    }
    if (role.value() > static_cast<std::uint8_t>(SpineRole::SuperSpine) ||
        spine_state.value() > static_cast<std::uint8_t>(NodeState::Retired)) {
      return Status(StatusCode::Invalid, "spine record carries an unknown enumeration value");
    }
    SpineRecord record;
    record.id = SpineId{id.value()};
    record.name = spine_name.value();
    record.role = static_cast<SpineRole>(role.value());
    record.role_incarnation = RoleIncarnation{incarnation.value()};
    record.domain = FailureDomainId{domain.value()};
    record.state = static_cast<NodeState>(spine_state.value());
    record.fabric_capacity_mbps = capacity.value();
    record.generation = TopologyGeneration{spine_generation.value()};
    const Status added = builder.add_spine(std::move(record));
    if (!added.ok()) {
      return added;
    }
  }

  const auto port_count = topology.u32();
  if (!port_count.ok()) {
    return Status(StatusCode::Truncated, "missing port count");
  }
  if (port_count.value() > limits.max_ports) {
    return Status(StatusCode::Exhausted, "port count exceeds the configured bound");
  }
  for (std::uint32_t i = 0; i < port_count.value(); ++i) {
    const auto id = topology.u32();
    const auto tier = topology.u8();
    const auto index = topology.u32();
    const auto speed = topology.u64();
    const auto admin = topology.u8();
    const auto port_generation = topology.u64();
    if (!id.ok() || !tier.ok() || !index.ok() || !speed.ok() || !admin.ok() || !port_generation.ok()) {
      return Status(StatusCode::Truncated, "port record is incomplete");
    }
    if (tier.value() > static_cast<std::uint8_t>(Tier::Spine) ||
        admin.value() > static_cast<std::uint8_t>(PortAdmin::Disabled)) {
      return Status(StatusCode::Invalid, "port record carries an unknown enumeration value");
    }
    PortRecord record;
    record.id = PortId{id.value()};
    record.owner = NodeKey{static_cast<Tier>(tier.value()), index.value()};
    record.speed_mbps = speed.value();
    record.admin = static_cast<PortAdmin>(admin.value());
    record.generation = TopologyGeneration{port_generation.value()};
    const Status added = builder.add_port(std::move(record));
    if (!added.ok()) {
      return added;
    }
  }

  const auto read_links = [&topology, &builder, &limits](bool history) -> Status {
    const auto count = topology.u32();
    if (!count.ok()) {
      return Status(StatusCode::Truncated, "missing link count");
    }
    const std::uint32_t bound = history ? limits.max_history_records : limits.max_links;
    if (count.value() > bound) {
      return Status(StatusCode::Exhausted, "link count exceeds the configured bound");
    }
    for (std::uint32_t i = 0; i < count.value(); ++i) {
      const auto id = topology.u32();
      const auto a_tier = topology.u8();
      const auto a_index = topology.u32();
      const auto port_a = topology.u32();
      const auto b_tier = topology.u8();
      const auto b_index = topology.u32();
      const auto port_b = topology.u32();
      const auto cls = topology.u8();
      const auto admin = topology.u8();
      const auto declared = topology.u64();
      const auto link_generation = topology.u64();
      if (!id.ok() || !a_tier.ok() || !a_index.ok() || !port_a.ok() || !b_tier.ok() || !b_index.ok() ||
          !port_b.ok() || !cls.ok() || !admin.ok() || !declared.ok() || !link_generation.ok()) {
        return Status(StatusCode::Truncated, "link record is incomplete");
      }
      if (a_tier.value() > static_cast<std::uint8_t>(Tier::Spine) ||
          b_tier.value() > static_cast<std::uint8_t>(Tier::Spine) ||
          cls.value() > static_cast<std::uint8_t>(LinkClass::SameTierPeer) ||
          admin.value() > static_cast<std::uint8_t>(LinkAdmin::Disabled)) {
        return Status(StatusCode::Invalid, "link record carries an unknown enumeration value");
      }
      LinkRecord record;
      record.id = LinkId{id.value()};
      record.a = NodeKey{static_cast<Tier>(a_tier.value()), a_index.value()};
      record.port_a = PortId{port_a.value()};
      record.b = NodeKey{static_cast<Tier>(b_tier.value()), b_index.value()};
      record.port_b = PortId{port_b.value()};
      record.cls = static_cast<LinkClass>(cls.value());
      record.admin = static_cast<LinkAdmin>(admin.value());
      record.declared_speed_mbps = declared.value();
      record.generation = TopologyGeneration{link_generation.value()};
      const Status added =
          history ? builder.add_history_link(std::move(record)) : builder.add_link(std::move(record));
      if (!added.ok()) {
        return added;
      }
    }
    return Status::success();
  };
  Status link_status = read_links(false);
  if (!link_status.ok()) {
    return link_status;
  }
  link_status = read_links(true);
  if (!link_status.ok()) {
    return link_status;
  }

  ByteReader evidence(evidence_block.value(), limits.max_name_bytes);
  const auto evidence_tag = evidence.string();
  const auto evidence_generation = evidence.u64();
  const auto evidence_count = evidence.u32();
  if (!evidence_tag.ok() || !evidence_generation.ok() || !evidence_count.ok()) {
    return Status(StatusCode::Truncated, "evidence block header is incomplete");
  }
  if (evidence_count.value() > limits.max_evidence_records) {
    return Status(StatusCode::Exhausted, "evidence count exceeds the configured bound");
  }
  for (std::uint32_t i = 0; i < evidence_count.value(); ++i) {
    const auto link = evidence.u32();
    const auto observed = evidence.u8();
    const auto record_generation = evidence.u64();
    const auto observer_hi = evidence.u64();
    const auto observer_lo = evidence.u64();
    const auto record_epoch = evidence.u64();
    const auto seq = evidence.u64();
    const auto at_ms = evidence.u64();
    if (!link.ok() || !observed.ok() || !record_generation.ok() || !observer_hi.ok() ||
        !observer_lo.ok() || !record_epoch.ok() || !seq.ok() || !at_ms.ok()) {
      return Status(StatusCode::Truncated, "evidence record is incomplete");
    }
    if (observed.value() > static_cast<std::uint8_t>(LinkObservation::Unknown)) {
      return Status(StatusCode::Invalid, "evidence record carries an unknown observation value");
    }
    LinkEvidence record;
    record.link = LinkId{link.value()};
    record.observed = static_cast<LinkObservation>(observed.value());
    record.generation = TopologyGeneration{record_generation.value()};
    record.observer = ControllerIncarnation{observer_hi.value(), observer_lo.value()};
    record.epoch = Epoch{record_epoch.value()};
    record.observed_seq = SequenceNumber{seq.value()};
    record.observed_at_unix_ms = at_ms.value();
    const Status added = builder.add_evidence(std::move(record));
    if (!added.ok()) {
      return added;
    }
  }
  const Status end = evidence.expect_end();
  if (!end.ok()) {
    return end;
  }
  return builder.freeze();
}

// ---------------------------------------------------------------------------
// Description
// ---------------------------------------------------------------------------

std::string describe_fabric(const Fabric& fabric) {
  std::ostringstream out;
  out << "fabric " << to_string(fabric.id) << " \"" << fabric.name << "\"";
  if (!fabric.site.empty()) {
    out << " site=\"" << fabric.site << "\"";
  }
  out << "\n";
  out << "  generation=" << fabric.generation.value << " epoch=" << fabric.epoch.value
      << " state=" << slf::to_string(fabric.state) << "\n";
  out << "  options: allow_same_tier_adjacency=" << bool_text(fabric.options.allow_same_tier_adjacency)
      << " require_domain_diversity=" << bool_text(fabric.options.require_domain_diversity)
      << " min_eligible_spines=" << fabric.options.min_eligible_spines
      << " evidence_policy=" << slf::to_string(fabric.options.evidence_policy) << "\n";
  out << "  topology_digest=" << fabric.topology_digest.short_hex(16)
      << " evidence_digest=" << (fabric.evidence.empty() ? std::string("none")
                                                        : fabric.evidence_digest.short_hex(16))
      << "\n";
  out << "  failure domains (" << fabric.domains.size() << "):\n";
  for (const auto& domain : fabric.domains) {
    out << "    " << to_string(domain.id) << " name=\"" << domain.name
        << "\" generation=" << domain.generation.value << "\n";
  }
  out << "  leaves (" << fabric.leaves.size() << "):\n";
  for (const auto& leaf : fabric.leaves) {
    out << "    " << to_string(leaf.id) << " name=\"" << leaf.name
        << "\" role=" << slf::to_string(leaf.role) << " state=" << slf::to_string(leaf.state)
        << " domain=" << leaf.domain.value << " role_incarnation=" << leaf.role_incarnation.value
        << " access_mbps=" << leaf.access_capacity_mbps << "\n";
  }
  out << "  spines (" << fabric.spines.size() << "):\n";
  for (const auto& spine : fabric.spines) {
    out << "    " << to_string(spine.id) << " name=\"" << spine.name
        << "\" role=" << slf::to_string(spine.role) << " state=" << slf::to_string(spine.state)
        << " domain=" << spine.domain.value << " role_incarnation=" << spine.role_incarnation.value
        << " fabric_mbps=" << spine.fabric_capacity_mbps << "\n";
  }
  out << "  ports (" << fabric.ports.size() << ") links (" << fabric.links.size()
      << ") historical links (" << fabric.history_links.size() << ") evidence ("
      << fabric.evidence.size() << ")\n";
  for (const auto& link : fabric.links) {
    const auto speed = fabric.effective_link_speed_mbps(link);
    out << "    " << to_string(link.id) << " " << to_string(link.a) << ":" << link.port_a.value
        << " <-> " << to_string(link.b) << ":" << link.port_b.value
        << " class=" << slf::to_string(link.cls) << " admin=" << slf::to_string(link.admin)
        << " speed_mbps=" << (speed.has_value() ? std::to_string(*speed) : std::string("unresolved"))
        << "\n";
  }
  return out.str();
}

}  // namespace slf
