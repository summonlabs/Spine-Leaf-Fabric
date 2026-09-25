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

#include "slf/governance.hpp"

#include <algorithm>
#include <sstream>

#include "slf/bytes.hpp"

namespace slf {
namespace {

constexpr std::string_view kCommandDomain = "slf.command.v1";
constexpr std::string_view kCommandResultDomain = "slf.command_result.v1";
constexpr std::string_view kPathAuthorityDomain = "slf.path_authority.v1";

}  // namespace

std::string_view to_string(CommandKind kind) noexcept {
  switch (kind) {
    case CommandKind::SetFabricState: return "set_fabric_state";
    case CommandKind::SetLeafState: return "set_leaf_state";
    case CommandKind::SetSpineState: return "set_spine_state";
    case CommandKind::SetPortAdmin: return "set_port_admin";
    case CommandKind::SetLinkAdmin: return "set_link_admin";
    case CommandKind::AddLink: return "add_link";
    case CommandKind::RemoveLink: return "remove_link";
    case CommandKind::SetOptions: return "set_options";
    case CommandKind::SubmitEvidence: return "submit_evidence";
    case CommandKind::ClearEvidence: return "clear_evidence";
  }
  return "invalid";
}

std::optional<CommandKind> parse_command_kind(std::string_view text) noexcept {
  if (text == "set_fabric_state") return CommandKind::SetFabricState;
  if (text == "set_leaf_state") return CommandKind::SetLeafState;
  if (text == "set_spine_state") return CommandKind::SetSpineState;
  if (text == "set_port_admin") return CommandKind::SetPortAdmin;
  if (text == "set_link_admin") return CommandKind::SetLinkAdmin;
  if (text == "add_link") return CommandKind::AddLink;
  if (text == "remove_link") return CommandKind::RemoveLink;
  if (text == "set_options") return CommandKind::SetOptions;
  if (text == "submit_evidence") return CommandKind::SubmitEvidence;
  if (text == "clear_evidence") return CommandKind::ClearEvidence;
  return std::nullopt;
}

AuthorityCheck validate_token(const AuthorityToken& token, const AuthorityContext& context) {
  AuthorityCheck check;
  if (token.controller.is_zero() || token.lease.is_zero()) {
    check.code = StatusCode::Refused;
    check.detail = "authority token is not a minted grant";
    return check;
  }
  if (token.controller != context.controller) {
    check.code = StatusCode::Fenced;
    check.detail = "authority was minted by controller incarnation " + to_string(token.controller) +
                   " but the live incarnation is " + to_string(context.controller);
    return check;
  }
  if (token.epoch != context.epoch) {
    check.code = StatusCode::Stale;
    check.detail = "authority epoch " + std::to_string(token.epoch.value) + " is not the live epoch " +
                   std::to_string(context.epoch.value);
    return check;
  }
  if (token.generation != context.generation) {
    check.code = StatusCode::Stale;
    check.detail = "authority generation " + std::to_string(token.generation.value) +
                   " is not the live generation " + std::to_string(context.generation.value);
    return check;
  }
  if (!(token.topology_digest == context.topology_digest)) {
    check.code = StatusCode::Conflicting;
    check.detail = "authority was minted for topology digest " + token.topology_digest.short_hex(8) +
                   " but the live digest is " + context.topology_digest.short_hex(8);
    return check;
  }
  if (!context.active_lease.has_value() || *context.active_lease != token.lease) {
    check.code = StatusCode::Refused;
    check.detail = "the lease that minted this authority is no longer active";
    return check;
  }
  if (token.expired_at(context.now_unix_ms)) {
    check.code = StatusCode::DeadlineExpired;
    check.detail = "authority lease expired at " + std::to_string(token.expires_at_unix_ms) +
                   " ms (now " + std::to_string(context.now_unix_ms) + " ms)";
    return check;
  }
  return check;
}

Digest compute_path_authority_digest(const PathAuthority& authority) {
  DigestBuilder builder;
  builder.add_string(kPathAuthorityDomain);
  builder.add_u64(authority.controller.hi);
  builder.add_u64(authority.controller.lo);
  builder.add_u64(authority.epoch.value);
  builder.add_u64(authority.generation.value);
  builder.add_bytes(std::span<const std::byte>(authority.topology_digest.bytes));
  builder.add_u64(authority.lease.value);
  builder.add_u8(static_cast<std::uint8_t>(authority.from.tier));
  builder.add_u32(authority.from.index);
  builder.add_u8(static_cast<std::uint8_t>(authority.to.tier));
  builder.add_u32(authority.to.index);
  builder.add_u8(static_cast<std::uint8_t>(authority.cls));
  builder.add_u8(static_cast<std::uint8_t>(authority.verdict));
  builder.add_u32(static_cast<std::uint32_t>(authority.spines.size()));
  for (const auto spine : authority.spines) {
    builder.add_u32(spine.value);
  }
  builder.add_u64(authority.issued_at_unix_ms);
  builder.add_u64(authority.expires_at_unix_ms);
  return builder.finish();
}

Outcome<PathAuthority> mint_path_authority(const PathAssessment& assessment, const AuthorityToken& token,
                                           std::uint64_t now_unix_ms, std::uint64_t ttl_ms) {
  if (assessment.verdict != EligibilityVerdict::Eligible) {
    return Status(assessment.verdict == EligibilityVerdict::Indeterminate ? StatusCode::Indeterminate
                                                                         : StatusCode::Refused,
                  "refusing to mint path authority for a path that is not proven eligible");
  }
  if (assessment.eligible_spines.empty()) {
    return Status(StatusCode::Indeterminate, "assessment carries no eligible spine set");
  }
  if (assessment.controller != token.controller || assessment.epoch != token.epoch ||
      assessment.generation != token.generation) {
    return Status(StatusCode::Stale, "assessment was computed at a different authority coordinate");
  }
  if (!(assessment.topology_digest == token.topology_digest)) {
    return Status(StatusCode::Conflicting, "assessment topology digest does not match the authority token");
  }
  PathAuthority authority;
  authority.controller = token.controller;
  authority.epoch = token.epoch;
  authority.generation = token.generation;
  authority.topology_digest = token.topology_digest;
  authority.lease = token.lease;
  authority.from = assessment.from;
  authority.to = assessment.to;
  authority.cls = assessment.cls;
  authority.verdict = assessment.verdict;
  authority.spines = assessment.eligible_spines;
  authority.issued_at_unix_ms = now_unix_ms;
  authority.expires_at_unix_ms = ttl_ms == 0 ? token.expires_at_unix_ms : now_unix_ms + ttl_ms;
  authority.authority_digest = compute_path_authority_digest(authority);
  return authority;
}

AuthorityCheck validate_path_authority(const PathAuthority& authority, const AuthorityContext& context) {
  AuthorityCheck check;
  if (!(compute_path_authority_digest(authority) == authority.authority_digest)) {
    check.code = StatusCode::IntegrityError;
    check.detail = "path authority body does not match its digest";
    return check;
  }
  if (authority.controller != context.controller) {
    check.code = StatusCode::Fenced;
    check.detail = "path authority was minted by a previous controller incarnation";
    return check;
  }
  if (authority.epoch != context.epoch) {
    check.code = StatusCode::Stale;
    check.detail = "path authority belongs to epoch " + std::to_string(authority.epoch.value);
    return check;
  }
  if (authority.generation != context.generation) {
    check.code = StatusCode::Stale;
    check.detail = "path authority belongs to generation " + std::to_string(authority.generation.value);
    return check;
  }
  if (!(authority.topology_digest == context.topology_digest)) {
    check.code = StatusCode::Conflicting;
    check.detail = "path authority topology digest does not match the live topology";
    return check;
  }
  if (!context.active_lease.has_value() || *context.active_lease != authority.lease) {
    check.code = StatusCode::Refused;
    check.detail = "the lease that minted this path authority is no longer active";
    return check;
  }
  if (context.now_unix_ms >= authority.expires_at_unix_ms) {
    check.code = StatusCode::DeadlineExpired;
    check.detail = "path authority expired";
    return check;
  }
  return check;
}

std::string CommandResult::to_string() const {
  std::ostringstream out;
  out << "request=" << request_id.value << " code=" << slf::to_string(code)
      << " generation=" << generation.value << " epoch=" << epoch.value
      << " deduplicated=" << (deduplicated ? "true" : "false")
      << " mutated=" << (mutated ? "true" : "false");
  if (!detail.empty()) {
    out << " detail=\"" << detail << "\"";
  }
  return out.str();
}

// ---------------------------------------------------------------------------
// Canonical encodings
// ---------------------------------------------------------------------------

std::vector<std::byte> serialize_command(const Command& command) {
  ByteWriter writer(1U * 1024U * 1024U);
  (void)writer.string(kCommandDomain);
  (void)writer.u64(command.request_id.value);
  (void)writer.u64(command.token.controller.hi);
  (void)writer.u64(command.token.controller.lo);
  (void)writer.u64(command.token.epoch.value);
  (void)writer.u64(command.token.generation.value);
  (void)writer.u64(command.token.lease.value);
  (void)writer.bytes(std::span<const std::byte>(command.token.topology_digest.bytes));
  (void)writer.u64(command.token.issued_at_unix_ms);
  (void)writer.u64(command.token.expires_at_unix_ms);
  (void)writer.u64(command.expected_generation.value);
  (void)writer.u64(command.expected_epoch.value);
  (void)writer.u8(static_cast<std::uint8_t>(command.kind));
  (void)writer.u8(static_cast<std::uint8_t>(command.fabric_state));
  (void)writer.u8(static_cast<std::uint8_t>(command.node.tier));
  (void)writer.u32(command.node.index);
  (void)writer.u8(static_cast<std::uint8_t>(command.node_state));
  (void)writer.u32(command.port.value);
  (void)writer.u8(static_cast<std::uint8_t>(command.port_admin));
  (void)writer.u32(command.link.value);
  (void)writer.u8(static_cast<std::uint8_t>(command.link_admin));
  (void)writer.u64(command.new_link.id.value);
  (void)writer.u8(static_cast<std::uint8_t>(command.new_link.a.tier));
  (void)writer.u32(command.new_link.a.index);
  (void)writer.u32(command.new_link.port_a.value);
  (void)writer.u8(static_cast<std::uint8_t>(command.new_link.b.tier));
  (void)writer.u32(command.new_link.b.index);
  (void)writer.u32(command.new_link.port_b.value);
  (void)writer.u8(static_cast<std::uint8_t>(command.new_link.cls));
  (void)writer.u8(static_cast<std::uint8_t>(command.new_link.admin));
  (void)writer.u64(command.new_link.declared_speed_mbps);
  (void)writer.u64(command.new_link.generation.value);
  (void)writer.u32(command.evidence.link.value);
  (void)writer.u8(static_cast<std::uint8_t>(command.evidence.observed));
  (void)writer.u64(command.evidence.generation.value);
  (void)writer.u64(command.evidence.observer.hi);
  (void)writer.u64(command.evidence.observer.lo);
  (void)writer.u64(command.evidence.epoch.value);
  (void)writer.u64(command.evidence.observed_seq.value);
  (void)writer.u64(command.evidence.observed_at_unix_ms);
  (void)writer.u8(command.options.allow_same_tier_adjacency ? 1U : 0U);
  (void)writer.u8(command.options.require_domain_diversity ? 1U : 0U);
  (void)writer.u32(command.options.min_eligible_spines);
  (void)writer.u8(static_cast<std::uint8_t>(command.options.evidence_policy));
  return writer.buffer();
}

Outcome<Command> deserialize_command(const std::span<const std::byte>& data) {
  ByteReader reader(data, 4096);
  Command command;
  const auto tag = reader.string();
  const auto request = reader.u64();
  const auto inc_hi = reader.u64();
  const auto inc_lo = reader.u64();
  const auto epoch = reader.u64();
  const auto generation = reader.u64();
  const auto lease = reader.u64();
  const auto digest_bytes = reader.bytes(32);
  const auto issued = reader.u64();
  const auto expires = reader.u64();
  const auto expected_generation = reader.u64();
  const auto expected_epoch = reader.u64();
  const auto kind = reader.u8();
  const auto fabric_state = reader.u8();
  const auto node_tier = reader.u8();
  const auto node_index = reader.u32();
  const auto node_state = reader.u8();
  const auto port = reader.u32();
  const auto port_admin = reader.u8();
  const auto link = reader.u32();
  const auto link_admin = reader.u8();
  const auto new_link_id = reader.u64();
  const auto new_link_a_tier = reader.u8();
  const auto new_link_a_index = reader.u32();
  const auto new_link_port_a = reader.u32();
  const auto new_link_b_tier = reader.u8();
  const auto new_link_b_index = reader.u32();
  const auto new_link_port_b = reader.u32();
  const auto new_link_cls = reader.u8();
  const auto new_link_admin = reader.u8();
  const auto new_link_speed = reader.u64();
  const auto new_link_generation = reader.u64();
  const auto evidence_link = reader.u32();
  const auto evidence_observed = reader.u8();
  const auto evidence_generation = reader.u64();
  const auto evidence_observer_hi = reader.u64();
  const auto evidence_observer_lo = reader.u64();
  const auto evidence_epoch = reader.u64();
  const auto evidence_seq = reader.u64();
  const auto evidence_at = reader.u64();
  const auto options_peer = reader.u8();
  const auto options_diversity = reader.u8();
  const auto options_min = reader.u32();
  const auto options_policy = reader.u8();
  if (!tag.ok() || !request.ok() || !inc_hi.ok() || !inc_lo.ok() || !epoch.ok() || !generation.ok() ||
      !lease.ok() || !digest_bytes.ok() || !issued.ok() || !expires.ok() || !expected_generation.ok() ||
      !expected_epoch.ok() || !kind.ok() || !fabric_state.ok() || !node_tier.ok() || !node_index.ok() ||
      !node_state.ok() || !port.ok() || !port_admin.ok() || !link.ok() || !link_admin.ok() ||
      !new_link_id.ok() || !new_link_a_tier.ok() || !new_link_a_index.ok() || !new_link_port_a.ok() ||
      !new_link_b_tier.ok() || !new_link_b_index.ok() || !new_link_port_b.ok() || !new_link_cls.ok() ||
      !new_link_admin.ok() || !new_link_speed.ok() || !new_link_generation.ok() || !evidence_link.ok() ||
      !evidence_observed.ok() || !evidence_generation.ok() || !evidence_observer_hi.ok() ||
      !evidence_observer_lo.ok() || !evidence_epoch.ok() || !evidence_seq.ok() || !evidence_at.ok() ||
      !options_peer.ok() || !options_diversity.ok() || !options_min.ok() || !options_policy.ok()) {
    return Status(StatusCode::Truncated, "command encoding is incomplete");
  }
  if (tag.value() != kCommandDomain) {
    return Status(StatusCode::Invalid, "command encoding has an unexpected domain tag");
  }
  if (kind.value() > static_cast<std::uint8_t>(CommandKind::ClearEvidence) ||
      fabric_state.value() > static_cast<std::uint8_t>(FabricState::Retired) ||
      node_tier.value() > static_cast<std::uint8_t>(Tier::Spine) ||
      node_state.value() > static_cast<std::uint8_t>(NodeState::Retired) ||
      port_admin.value() > static_cast<std::uint8_t>(PortAdmin::Disabled) ||
      link_admin.value() > static_cast<std::uint8_t>(LinkAdmin::Disabled) ||
      new_link_cls.value() > static_cast<std::uint8_t>(LinkClass::SameTierPeer) ||
      evidence_observed.value() > static_cast<std::uint8_t>(LinkObservation::Unknown) ||
      options_policy.value() > static_cast<std::uint8_t>(EvidencePolicy::RequireFresh)) {
    return Status(StatusCode::Invalid, "command encoding carries an unknown enumeration value");
  }
  const Status end = reader.expect_end();
  if (!end.ok()) {
    return end;
  }
  command.request_id = RequestId{request.value()};
  command.token.controller = ControllerIncarnation{inc_hi.value(), inc_lo.value()};
  command.token.epoch = Epoch{epoch.value()};
  command.token.generation = TopologyGeneration{generation.value()};
  command.token.lease = LeaseId{lease.value()};
  std::copy(digest_bytes.value().begin(), digest_bytes.value().end(), command.token.topology_digest.bytes.begin());
  command.token.issued_at_unix_ms = issued.value();
  command.token.expires_at_unix_ms = expires.value();
  command.expected_generation = TopologyGeneration{expected_generation.value()};
  command.expected_epoch = Epoch{expected_epoch.value()};
  command.kind = static_cast<CommandKind>(kind.value());
  command.fabric_state = static_cast<FabricState>(fabric_state.value());
  command.node = NodeKey{static_cast<Tier>(node_tier.value()), node_index.value()};
  command.node_state = static_cast<NodeState>(node_state.value());
  command.port = PortId{port.value()};
  command.port_admin = static_cast<PortAdmin>(port_admin.value());
  command.link = LinkId{link.value()};
  command.link_admin = static_cast<LinkAdmin>(link_admin.value());
  command.new_link.id = LinkId{static_cast<std::uint32_t>(new_link_id.value())};
  command.new_link.a = NodeKey{static_cast<Tier>(new_link_a_tier.value()), new_link_a_index.value()};
  command.new_link.port_a = PortId{new_link_port_a.value()};
  command.new_link.b = NodeKey{static_cast<Tier>(new_link_b_tier.value()), new_link_b_index.value()};
  command.new_link.port_b = PortId{new_link_port_b.value()};
  command.new_link.cls = static_cast<LinkClass>(new_link_cls.value());
  command.new_link.admin = static_cast<LinkAdmin>(new_link_admin.value());
  command.new_link.declared_speed_mbps = new_link_speed.value();
  command.new_link.generation = TopologyGeneration{new_link_generation.value()};
  command.evidence.link = LinkId{evidence_link.value()};
  command.evidence.observed = static_cast<LinkObservation>(evidence_observed.value());
  command.evidence.generation = TopologyGeneration{evidence_generation.value()};
  command.evidence.observer = ControllerIncarnation{evidence_observer_hi.value(), evidence_observer_lo.value()};
  command.evidence.epoch = Epoch{evidence_epoch.value()};
  command.evidence.observed_seq = SequenceNumber{evidence_seq.value()};
  command.evidence.observed_at_unix_ms = evidence_at.value();
  command.options.allow_same_tier_adjacency = options_peer.value() != 0;
  command.options.require_domain_diversity = options_diversity.value() != 0;
  command.options.min_eligible_spines = options_min.value();
  command.options.evidence_policy = static_cast<EvidencePolicy>(options_policy.value());
  return command;
}

std::vector<std::byte> serialize_command_result(const CommandResult& result) {
  ByteWriter writer(1U * 1024U * 1024U);
  (void)writer.string(kCommandResultDomain);
  (void)writer.u64(result.request_id.value);
  (void)writer.u16(static_cast<std::uint16_t>(result.code));
  (void)writer.string(result.detail);
  (void)writer.u64(result.generation.value);
  (void)writer.u64(result.epoch.value);
  (void)writer.bytes(std::span<const std::byte>(result.topology_digest.bytes));
  (void)writer.u8(result.deduplicated ? 1U : 0U);
  (void)writer.u8(result.mutated ? 1U : 0U);
  return writer.buffer();
}

Outcome<CommandResult> deserialize_command_result(const std::span<const std::byte>& data) {
  ByteReader reader(data, 64U * 1024U);
  CommandResult result;
  const auto tag = reader.string();
  const auto request = reader.u64();
  const auto code = reader.u16();
  const auto detail = reader.string();
  const auto generation = reader.u64();
  const auto epoch = reader.u64();
  const auto digest_bytes = reader.bytes(32);
  const auto deduplicated = reader.u8();
  const auto mutated = reader.u8();
  if (!tag.ok() || !request.ok() || !code.ok() || !detail.ok() || !generation.ok() || !epoch.ok() ||
      !digest_bytes.ok() || !deduplicated.ok() || !mutated.ok()) {
    return Status(StatusCode::Truncated, "command result encoding is incomplete");
  }
  if (tag.value() != kCommandResultDomain) {
    return Status(StatusCode::Invalid, "command result encoding has an unexpected domain tag");
  }
  const Status end = reader.expect_end();
  if (!end.ok()) {
    return end;
  }
  result.request_id = RequestId{request.value()};
  result.code = static_cast<StatusCode>(code.value());
  result.detail = detail.value();
  result.generation = TopologyGeneration{generation.value()};
  result.epoch = Epoch{epoch.value()};
  std::copy(digest_bytes.value().begin(), digest_bytes.value().end(), result.topology_digest.bytes.begin());
  result.deduplicated = deduplicated.value() != 0;
  result.mutated = mutated.value() != 0;
  return result;
}

std::vector<std::byte> serialize_path_authority(const PathAuthority& authority) {
  ByteWriter writer(1U * 1024U * 1024U);
  (void)writer.string(kPathAuthorityDomain);
  (void)writer.u64(authority.controller.hi);
  (void)writer.u64(authority.controller.lo);
  (void)writer.u64(authority.epoch.value);
  (void)writer.u64(authority.generation.value);
  (void)writer.bytes(std::span<const std::byte>(authority.topology_digest.bytes));
  (void)writer.u64(authority.lease.value);
  (void)writer.u8(static_cast<std::uint8_t>(authority.from.tier));
  (void)writer.u32(authority.from.index);
  (void)writer.u8(static_cast<std::uint8_t>(authority.to.tier));
  (void)writer.u32(authority.to.index);
  (void)writer.u8(static_cast<std::uint8_t>(authority.cls));
  (void)writer.u8(static_cast<std::uint8_t>(authority.verdict));
  (void)writer.u32(static_cast<std::uint32_t>(authority.spines.size()));
  for (const auto spine : authority.spines) {
    (void)writer.u32(spine.value);
  }
  (void)writer.u64(authority.issued_at_unix_ms);
  (void)writer.u64(authority.expires_at_unix_ms);
  (void)writer.bytes(std::span<const std::byte>(authority.authority_digest.bytes));
  return writer.buffer();
}

Outcome<PathAuthority> deserialize_path_authority(const std::span<const std::byte>& data) {
  ByteReader reader(data, 4096);
  PathAuthority authority;
  const auto tag = reader.string();
  const auto inc_hi = reader.u64();
  const auto inc_lo = reader.u64();
  const auto epoch = reader.u64();
  const auto generation = reader.u64();
  const auto digest_bytes = reader.bytes(32);
  const auto lease = reader.u64();
  const auto from_tier = reader.u8();
  const auto from_index = reader.u32();
  const auto to_tier = reader.u8();
  const auto to_index = reader.u32();
  const auto cls = reader.u8();
  const auto verdict = reader.u8();
  const auto spine_count = reader.u32();
  if (!tag.ok() || !inc_hi.ok() || !inc_lo.ok() || !epoch.ok() || !generation.ok() ||
      !digest_bytes.ok() || !lease.ok() || !from_tier.ok() || !from_index.ok() || !to_tier.ok() ||
      !to_index.ok() || !cls.ok() || !verdict.ok() || !spine_count.ok()) {
    return Status(StatusCode::Truncated, "path authority encoding is incomplete");
  }
  if (tag.value() != kPathAuthorityDomain) {
    return Status(StatusCode::Invalid, "path authority encoding has an unexpected domain tag");
  }
  constexpr std::uint32_t kMaxAuthoritySpines = 4096;
  if (spine_count.value() > kMaxAuthoritySpines) {
    return Status(StatusCode::Exhausted, "path authority spine count exceeds the configured bound");
  }
  if (from_tier.value() > static_cast<std::uint8_t>(Tier::Spine) ||
      to_tier.value() > static_cast<std::uint8_t>(Tier::Spine) ||
      cls.value() > static_cast<std::uint8_t>(ReachabilityClass::Indeterminate) ||
      verdict.value() > static_cast<std::uint8_t>(EligibilityVerdict::Indeterminate)) {
    return Status(StatusCode::Invalid, "path authority carries an unknown enumeration value");
  }
  authority.controller = ControllerIncarnation{inc_hi.value(), inc_lo.value()};
  authority.epoch = Epoch{epoch.value()};
  authority.generation = TopologyGeneration{generation.value()};
  std::copy(digest_bytes.value().begin(), digest_bytes.value().end(), authority.topology_digest.bytes.begin());
  authority.lease = LeaseId{lease.value()};
  authority.from = NodeKey{static_cast<Tier>(from_tier.value()), from_index.value()};
  authority.to = NodeKey{static_cast<Tier>(to_tier.value()), to_index.value()};
  authority.cls = static_cast<ReachabilityClass>(cls.value());
  authority.verdict = static_cast<EligibilityVerdict>(verdict.value());
  authority.spines.reserve(spine_count.value());
  for (std::uint32_t i = 0; i < spine_count.value(); ++i) {
    const auto spine = reader.u32();
    if (!spine.ok()) {
      return Status(StatusCode::Truncated, "path authority spine list is incomplete");
    }
    authority.spines.push_back(SpineId{spine.value()});
  }
  const auto issued = reader.u64();
  const auto expires = reader.u64();
  const auto authority_digest = reader.bytes(32);
  if (!issued.ok() || !expires.ok() || !authority_digest.ok()) {
    return Status(StatusCode::Truncated, "path authority trailer is incomplete");
  }
  authority.issued_at_unix_ms = issued.value();
  authority.expires_at_unix_ms = expires.value();
  std::copy(authority_digest.value().begin(), authority_digest.value().end(),
            authority.authority_digest.bytes.begin());
  const Status end = reader.expect_end();
  if (!end.ok()) {
    return end;
  }
  return authority;
}

}  // namespace slf
