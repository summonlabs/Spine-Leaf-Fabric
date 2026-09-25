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

#include "slf/service.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "slf/bytes.hpp"
#include "slf/checked.hpp"
#include "slf/platform.hpp"

namespace slf::net {
namespace {

constexpr std::string_view kRequestDomain = "slf.request.v1";
constexpr std::string_view kResponseDomain = "slf.response.v1";
constexpr std::size_t kMaxDetailBytes = 4096;
constexpr std::size_t kMaxNameBytes = 256;
constexpr std::size_t kMaxWireSpines = 4096;
constexpr std::size_t kMaxWireSets = 64;
constexpr std::size_t kMaxWireExplanations = 64;
constexpr std::size_t kMaxWireReasons = 64;
constexpr std::size_t kMaxBlockBytes = 4U * 1024U * 1024U;

[[nodiscard]] std::string clamp_detail(std::string_view text, std::size_t limit = kMaxDetailBytes) {
  if (text.size() <= limit) {
    return std::string(text);
  }
  return std::string(text.substr(0, limit));
}

// ---------------------------------------------------------------------------
// Shared encoders
// ---------------------------------------------------------------------------

[[nodiscard]] Status encode_constraints(ByteWriter& writer, const PathConstraints& constraints) {
  Status status = writer.u8(constraints.require_domain_diversity ? 1U : 0U);
  status = status.ok() ? writer.u32(constraints.min_eligible_spines) : status;
  status = status.ok() ? writer.u64(constraints.min_total_capacity_mbps) : status;
  status = status.ok() ? writer.u8(constraints.max_oversubscription.has_value() ? 1U : 0U) : status;
  const Rational ratio = constraints.max_oversubscription.value_or(Rational{0, 1});
  status = status.ok() ? writer.u64(ratio.num) : status;
  status = status.ok() ? writer.u64(ratio.den) : status;
  status = status.ok() ? writer.u8(constraints.allow_peer_paths ? 1U : 0U) : status;
  status = status.ok() ? writer.u8(constraints.require_evidence ? 1U : 0U) : status;
  status = status.ok() ? writer.u32(constraints.max_sets) : status;
  status = status.ok() ? writer.u32(constraints.max_explanations) : status;
  return status;
}

[[nodiscard]] Outcome<PathConstraints> decode_constraints(ByteReader& reader) {
  PathConstraints constraints;
  const auto diversity = reader.u8();
  const auto min_spines = reader.u32();
  const auto min_capacity = reader.u64();
  const auto has_ratio = reader.u8();
  const auto ratio_num = reader.u64();
  const auto ratio_den = reader.u64();
  const auto peer = reader.u8();
  const auto evidence = reader.u8();
  const auto max_sets = reader.u32();
  const auto max_explanations = reader.u32();
  if (!diversity.ok() || !min_spines.ok() || !min_capacity.ok() || !has_ratio.ok() || !ratio_num.ok() ||
      !ratio_den.ok() || !peer.ok() || !evidence.ok() || !max_sets.ok() || !max_explanations.ok()) {
    return Status(StatusCode::Truncated, "path constraints are incomplete");
  }
  if (max_sets.value() > 4096U || max_explanations.value() > 1024U) {
    return Status(StatusCode::Exhausted, "path constraint bounds exceed the accepted maximum");
  }
  constraints.require_domain_diversity = diversity.value() != 0;
  constraints.min_eligible_spines = min_spines.value();
  constraints.min_total_capacity_mbps = min_capacity.value();
  if (has_ratio.value() != 0) {
    const auto ratio = Rational::make(ratio_num.value(), ratio_den.value());
    if (!ratio.ok()) {
      return ratio.status();
    }
    constraints.max_oversubscription = ratio.value();
  }
  constraints.allow_peer_paths = peer.value() != 0;
  constraints.require_evidence = evidence.value() != 0;
  constraints.max_sets = max_sets.value();
  constraints.max_explanations = max_explanations.value();
  return constraints;
}

[[nodiscard]] Status encode_node(ByteWriter& writer, NodeKey key) {
  Status status = writer.u8(static_cast<std::uint8_t>(key.tier));
  return status.ok() ? writer.u32(key.index) : status;
}

[[nodiscard]] Outcome<NodeKey> decode_node(ByteReader& reader) {
  const auto tier = reader.u8();
  const auto index = reader.u32();
  if (!tier.ok() || !index.ok()) {
    return Status(StatusCode::Truncated, "node reference is incomplete");
  }
  if (tier.value() > static_cast<std::uint8_t>(Tier::Spine)) {
    return Status(StatusCode::Invalid, "node reference carries an unknown tier");
  }
  return NodeKey{static_cast<Tier>(tier.value()), index.value()};
}

[[nodiscard]] Status encode_digest(ByteWriter& writer, const Digest& digest) {
  return writer.bytes(std::span<const std::byte>(digest.bytes));
}

[[nodiscard]] Outcome<Digest> decode_digest(ByteReader& reader) {
  const auto bytes = reader.bytes(32);
  if (!bytes.ok()) {
    return Status(StatusCode::Truncated, "digest is incomplete");
  }
  Digest digest;
  std::copy(bytes.value().begin(), bytes.value().end(), digest.bytes.begin());
  return digest;
}

[[nodiscard]] Status encode_capacity(ByteWriter& writer, const CapacitySummary& capacity) {
  Status status = writer.u64(capacity.uplink_capacity_mbps);
  status = status.ok() ? writer.u32(capacity.uplink_count) : status;
  status = status.ok() ? writer.u64(capacity.downlink_capacity_mbps) : status;
  status = status.ok() ? writer.u32(capacity.downlink_count) : status;
  status = status.ok() ? writer.u64(capacity.access_capacity_mbps) : status;
  status = status.ok() ? writer.u64(capacity.min_uplink_mbps) : status;
  status = status.ok() ? writer.u64(capacity.max_uplink_mbps) : status;
  status = status.ok() ? writer.u64(capacity.spine_capacity_mbps) : status;
  status = status.ok() ? writer.u32(capacity.spine_count) : status;
  status = status.ok() ? writer.u64(capacity.oversubscription.num) : status;
  status = status.ok() ? writer.u64(capacity.oversubscription.den) : status;
  status = status.ok() ? writer.u8(static_cast<std::uint8_t>(capacity.classification)) : status;
  status = status.ok() ? writer.u16(static_cast<std::uint16_t>(capacity.code)) : status;
  status = status.ok() ? writer.string(clamp_detail(capacity.detail)) : status;
  return status;
}

[[nodiscard]] Outcome<CapacitySummary> decode_capacity(ByteReader& reader) {
  CapacitySummary capacity;
  const auto uplink = reader.u64();
  const auto uplink_count = reader.u32();
  const auto downlink = reader.u64();
  const auto downlink_count = reader.u32();
  const auto access = reader.u64();
  const auto min_uplink = reader.u64();
  const auto max_uplink = reader.u64();
  const auto spine_capacity = reader.u64();
  const auto spine_count = reader.u32();
  const auto ratio_num = reader.u64();
  const auto ratio_den = reader.u64();
  const auto classification = reader.u8();
  const auto code = reader.u16();
  const auto detail = reader.string();
  if (!uplink.ok() || !uplink_count.ok() || !downlink.ok() || !downlink_count.ok() || !access.ok() ||
      !min_uplink.ok() || !max_uplink.ok() || !spine_capacity.ok() || !spine_count.ok() || !ratio_num.ok() ||
      !ratio_den.ok() || !classification.ok() || !code.ok() || !detail.ok()) {
    return Status(StatusCode::Truncated, "capacity summary is incomplete");
  }
  if (classification.value() > static_cast<std::uint8_t>(OversubscriptionClass::Indeterminate)) {
    return Status(StatusCode::Invalid, "capacity summary carries an unknown classification");
  }
  const auto ratio = Rational::make(ratio_num.value(), ratio_den.value());
  if (!ratio.ok()) {
    return ratio.status();
  }
  capacity.uplink_capacity_mbps = uplink.value();
  capacity.uplink_count = uplink_count.value();
  capacity.downlink_capacity_mbps = downlink.value();
  capacity.downlink_count = downlink_count.value();
  capacity.access_capacity_mbps = access.value();
  capacity.min_uplink_mbps = min_uplink.value();
  capacity.max_uplink_mbps = max_uplink.value();
  capacity.spine_capacity_mbps = spine_capacity.value();
  capacity.spine_count = spine_count.value();
  capacity.oversubscription = ratio.value();
  capacity.classification = static_cast<OversubscriptionClass>(classification.value());
  capacity.code = static_cast<StatusCode>(code.value());
  capacity.detail = detail.value();
  return capacity;
}

[[nodiscard]] Status encode_spine_eligibility(ByteWriter& writer, const SpineEligibility& entry) {
  Status status = writer.u32(entry.spine.value);
  status = status.ok() ? writer.u8(static_cast<std::uint8_t>(entry.verdict)) : status;
  status = status.ok() ? writer.u16(static_cast<std::uint16_t>(entry.code)) : status;
  status = status.ok() ? writer.u8(entry.structural_ok ? 1U : 0U) : status;
  status = status.ok() ? writer.u8(static_cast<std::uint8_t>(entry.freshness)) : status;
  status = status.ok() ? writer.u64(entry.capacity_mbps) : status;
  status = status.ok() ? writer.u32(entry.domain.value) : status;
  status = status.ok() ? writer.string(clamp_detail(entry.detail)) : status;
  return status;
}

[[nodiscard]] Outcome<SpineEligibility> decode_spine_eligibility(ByteReader& reader) {
  SpineEligibility entry;
  const auto spine = reader.u32();
  const auto verdict = reader.u8();
  const auto code = reader.u16();
  const auto structural = reader.u8();
  const auto freshness = reader.u8();
  const auto capacity = reader.u64();
  const auto domain = reader.u32();
  const auto detail = reader.string();
  if (!spine.ok() || !verdict.ok() || !code.ok() || !structural.ok() || !freshness.ok() || !capacity.ok() ||
      !domain.ok() || !detail.ok()) {
    return Status(StatusCode::Truncated, "spine eligibility entry is incomplete");
  }
  if (verdict.value() > static_cast<std::uint8_t>(EligibilityVerdict::Indeterminate) ||
      freshness.value() > static_cast<std::uint8_t>(EvidenceFreshness::Absent)) {
    return Status(StatusCode::Invalid, "spine eligibility entry carries an unknown enumeration value");
  }
  entry.spine = SpineId{spine.value()};
  entry.verdict = static_cast<EligibilityVerdict>(verdict.value());
  entry.code = static_cast<StatusCode>(code.value());
  entry.structural_ok = structural.value() != 0;
  entry.freshness = static_cast<EvidenceFreshness>(freshness.value());
  entry.capacity_mbps = capacity.value();
  entry.domain = FailureDomainId{domain.value()};
  entry.detail = detail.value();
  return entry;
}

[[nodiscard]] Status encode_assessment(ByteWriter& writer, const PathAssessment& assessment) {
  Status status = encode_node(writer, assessment.from);
  status = status.ok() ? encode_node(writer, assessment.to) : status;
  status = status.ok() ? writer.u8(static_cast<std::uint8_t>(assessment.cls)) : status;
  status = status.ok() ? writer.u8(static_cast<std::uint8_t>(assessment.verdict)) : status;
  status = status.ok() ? writer.u16(static_cast<std::uint16_t>(assessment.code)) : status;
  status = status.ok() ? writer.u64(assessment.generation.value) : status;
  status = status.ok() ? writer.u64(assessment.epoch.value) : status;
  status = status.ok() ? writer.u64(assessment.controller.hi) : status;
  status = status.ok() ? writer.u64(assessment.controller.lo) : status;
  status = status.ok() ? encode_digest(writer, assessment.topology_digest) : status;
  status = status.ok() ? writer.u8(assessment.evidence_required ? 1U : 0U) : status;
  status = status.ok() ? writer.u64(assessment.links_examined) : status;
  status = status.ok() ? writer.u32(assessment.max_domain_loss_tolerance) : status;
  status = status.ok() ? writer.u32(static_cast<std::uint32_t>(assessment.eligible_spines.size())) : status;
  for (const auto spine : assessment.eligible_spines) {
    status = status.ok() ? writer.u32(spine.value) : status;
  }
  status = status.ok() ? writer.u32(static_cast<std::uint32_t>(assessment.covered_domains.size())) : status;
  for (const auto domain : assessment.covered_domains) {
    status = status.ok() ? writer.u32(domain.value) : status;
  }
  status = status.ok() ? writer.u8(assessment.spine_sets.truncated ? 1U : 0U) : status;
  status = status.ok() ? writer.u64(assessment.spine_sets.candidate_combinations) : status;
  status = status.ok() ? writer.u32(static_cast<std::uint32_t>(assessment.spine_sets.sets.size())) : status;
  for (const auto& set : assessment.spine_sets.sets) {
    status = status.ok() ? writer.u64(set.total_capacity_mbps) : status;
    status = status.ok() ? writer.u32(set.distinct_domains) : status;
    status = status.ok() ? writer.u32(static_cast<std::uint32_t>(set.spines.size())) : status;
    for (const auto spine : set.spines) {
      status = status.ok() ? writer.u32(spine.value) : status;
    }
    status = status.ok() ? writer.u32(static_cast<std::uint32_t>(set.domains.size())) : status;
    for (const auto domain : set.domains) {
      status = status.ok() ? writer.u32(domain.value) : status;
    }
  }
  status = status.ok() ? encode_capacity(writer, assessment.capacity) : status;
  status = status.ok() ? writer.u32(static_cast<std::uint32_t>(assessment.explanation.size())) : status;
  for (const auto& line : assessment.explanation) {
    status = status.ok() ? writer.string(clamp_detail(line)) : status;
  }
  return status;
}

[[nodiscard]] Outcome<PathAssessment> decode_assessment(ByteReader& reader) {
  const auto from = decode_node(reader);
  const auto to = decode_node(reader);
  const auto cls = reader.u8();
  const auto verdict = reader.u8();
  const auto code = reader.u16();
  const auto generation = reader.u64();
  const auto epoch = reader.u64();
  const auto controller_hi = reader.u64();
  const auto controller_lo = reader.u64();
  const auto digest = decode_digest(reader);
  const auto evidence_required = reader.u8();
  const auto links_examined = reader.u64();
  const auto tolerance = reader.u32();
  const auto spine_count = reader.u32();
  if (!from.ok() || !to.ok() || !cls.ok() || !verdict.ok() || !code.ok() || !generation.ok() || !epoch.ok() ||
      !controller_hi.ok() || !controller_lo.ok() || !digest.ok() || !evidence_required.ok() ||
      !links_examined.ok() || !tolerance.ok() || !spine_count.ok()) {
    return Status(StatusCode::Truncated, "path assessment is incomplete");
  }
  if (cls.value() > static_cast<std::uint8_t>(ReachabilityClass::Indeterminate) ||
      verdict.value() > static_cast<std::uint8_t>(EligibilityVerdict::Indeterminate)) {
    return Status(StatusCode::Invalid, "path assessment carries an unknown enumeration value");
  }
  if (spine_count.value() > kMaxWireSpines) {
    return Status(StatusCode::Exhausted, "path assessment spine list exceeds the accepted bound");
  }
  PathAssessment assessment;
  assessment.from = from.value();
  assessment.to = to.value();
  assessment.cls = static_cast<ReachabilityClass>(cls.value());
  assessment.verdict = static_cast<EligibilityVerdict>(verdict.value());
  assessment.code = static_cast<StatusCode>(code.value());
  assessment.generation = TopologyGeneration{generation.value()};
  assessment.epoch = Epoch{epoch.value()};
  assessment.controller = ControllerIncarnation{controller_hi.value(), controller_lo.value()};
  assessment.topology_digest = digest.value();
  assessment.evidence_required = evidence_required.value() != 0;
  assessment.links_examined = links_examined.value();
  assessment.max_domain_loss_tolerance = tolerance.value();
  assessment.eligible_spines.reserve(spine_count.value());
  for (std::uint32_t i = 0; i < spine_count.value(); ++i) {
    const auto spine = reader.u32();
    if (!spine.ok()) {
      return Status(StatusCode::Truncated, "path assessment spine list is incomplete");
    }
    assessment.eligible_spines.push_back(SpineId{spine.value()});
  }
  const auto domain_count = reader.u32();
  if (!domain_count.ok()) {
    return Status(StatusCode::Truncated, "path assessment domain list is incomplete");
  }
  if (domain_count.value() > kMaxWireSpines) {
    return Status(StatusCode::Exhausted, "path assessment domain list exceeds the accepted bound");
  }
  for (std::uint32_t i = 0; i < domain_count.value(); ++i) {
    const auto domain = reader.u32();
    if (!domain.ok()) {
      return Status(StatusCode::Truncated, "path assessment domain list is incomplete");
    }
    assessment.covered_domains.push_back(FailureDomainId{domain.value()});
  }
  const auto truncated = reader.u8();
  const auto combinations = reader.u64();
  const auto set_count = reader.u32();
  if (!truncated.ok() || !combinations.ok() || !set_count.ok()) {
    return Status(StatusCode::Truncated, "path assessment spine set summary is incomplete");
  }
  if (set_count.value() > kMaxWireSets) {
    return Status(StatusCode::Exhausted, "path assessment spine set count exceeds the accepted bound");
  }
  assessment.spine_sets.truncated = truncated.value() != 0;
  assessment.spine_sets.candidate_combinations = combinations.value();
  for (std::uint32_t i = 0; i < set_count.value(); ++i) {
    // The encoder writes the set's domain *count* separately from the number of
    // domain records it carries; reading them as one field misaligns every
    // following field, so they are decoded as two.
    EligibleSpineSet set;
    const auto capacity = reader.u64();
    const auto distinct_domains = reader.u32();
    const auto spine_count_in_set = reader.u32();
    if (!capacity.ok() || !distinct_domains.ok() || !spine_count_in_set.ok()) {
      return Status(StatusCode::Truncated, "spine set is incomplete");
    }
    if (spine_count_in_set.value() > kMaxWireSpines || distinct_domains.value() > kMaxWireSpines) {
      return Status(StatusCode::Exhausted, "spine set exceeds the accepted bound");
    }
    set.total_capacity_mbps = capacity.value();
    set.distinct_domains = distinct_domains.value();
    for (std::uint32_t k = 0; k < spine_count_in_set.value(); ++k) {
      const auto spine = reader.u32();
      if (!spine.ok()) {
        return Status(StatusCode::Truncated, "spine set spine list is incomplete");
      }
      set.spines.push_back(SpineId{spine.value()});
    }
    const auto domain_count_in_set = reader.u32();
    if (!domain_count_in_set.ok()) {
      return Status(StatusCode::Truncated, "spine set domain list is incomplete");
    }
    if (domain_count_in_set.value() > kMaxWireSpines) {
      return Status(StatusCode::Exhausted, "spine set domain list exceeds the accepted bound");
    }
    for (std::uint32_t k = 0; k < domain_count_in_set.value(); ++k) {
      const auto domain = reader.u32();
      if (!domain.ok()) {
        return Status(StatusCode::Truncated, "spine set domain list is incomplete");
      }
      set.domains.push_back(FailureDomainId{domain.value()});
    }
    assessment.spine_sets.sets.push_back(std::move(set));
  }
  const auto capacity = decode_capacity(reader);
  if (!capacity.ok()) {
    return capacity.status();
  }
  assessment.capacity = capacity.value();
  const auto explanation_count = reader.u32();
  if (!explanation_count.ok()) {
    return Status(StatusCode::Truncated, "path assessment explanation list is incomplete");
  }
  if (explanation_count.value() > kMaxWireExplanations) {
    return Status(StatusCode::Exhausted, "path assessment explanation count exceeds the accepted bound");
  }
  for (std::uint32_t i = 0; i < explanation_count.value(); ++i) {
    const auto line = reader.string();
    if (!line.ok()) {
      return Status(StatusCode::Truncated, "path assessment explanation line is incomplete");
    }
    assessment.explanation.push_back(line.value());
  }
  return assessment;
}

[[nodiscard]] Status encode_health(ByteWriter& writer, const FabricHealthReport& report) {
  Status status = writer.u8(static_cast<std::uint8_t>(report.health));
  status = status.ok() ? writer.u16(static_cast<std::uint16_t>(report.code)) : status;
  status = status.ok() ? writer.string(clamp_detail(report.summary)) : status;
  status = status.ok() ? writer.u32(static_cast<std::uint32_t>(report.reasons.size())) : status;
  for (const auto& reason : report.reasons) {
    status = status.ok() ? writer.string(clamp_detail(reason, 512)) : status;
  }
  for (const std::uint32_t value : {report.leaves_total, report.leaves_serving, report.leaves_isolated,
                                    report.leaves_indeterminate, report.spines_total, report.spines_serving,
                                    report.spines_failed, report.spines_draining, report.disabled_link_count}) {
    status = status.ok() ? writer.u32(value) : status;
  }
  status = status.ok() ? writer.u64(report.generation.value) : status;
  status = status.ok() ? writer.u64(report.epoch.value) : status;
  status = status.ok() ? writer.u64(report.controller.hi) : status;
  status = status.ok() ? writer.u64(report.controller.lo) : status;
  status = status.ok() ? encode_digest(writer, report.topology_digest) : status;
  return status;
}

[[nodiscard]] Outcome<FabricHealthReport> decode_health(ByteReader& reader) {
  FabricHealthReport report;
  const auto health = reader.u8();
  const auto code = reader.u16();
  const auto summary = reader.string();
  const auto reason_count = reader.u32();
  if (!health.ok() || !code.ok() || !summary.ok() || !reason_count.ok()) {
    return Status(StatusCode::Truncated, "health report is incomplete");
  }
  if (health.value() > static_cast<std::uint8_t>(FabricHealth::Indeterminate)) {
    return Status(StatusCode::Invalid, "health report carries an unknown classification");
  }
  if (reason_count.value() > kMaxWireReasons) {
    return Status(StatusCode::Exhausted, "health report reason count exceeds the accepted bound");
  }
  report.health = static_cast<FabricHealth>(health.value());
  report.code = static_cast<StatusCode>(code.value());
  report.summary = summary.value();
  for (std::uint32_t i = 0; i < reason_count.value(); ++i) {
    const auto reason = reader.string();
    if (!reason.ok()) {
      return Status(StatusCode::Truncated, "health report reason is incomplete");
    }
    report.reasons.push_back(reason.value());
  }
  std::uint32_t counts[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  for (auto& count : counts) {
    const auto value = reader.u32();
    if (!value.ok()) {
      return Status(StatusCode::Truncated, "health report counter is incomplete");
    }
    count = value.value();
  }
  const auto generation = reader.u64();
  const auto epoch = reader.u64();
  const auto controller_hi = reader.u64();
  const auto controller_lo = reader.u64();
  const auto digest = decode_digest(reader);
  if (!generation.ok() || !epoch.ok() || !controller_hi.ok() || !controller_lo.ok() || !digest.ok()) {
    return Status(StatusCode::Truncated, "health report coordinate is incomplete");
  }
  report.leaves_total = counts[0];
  report.leaves_serving = counts[1];
  report.leaves_isolated = counts[2];
  report.leaves_indeterminate = counts[3];
  report.spines_total = counts[4];
  report.spines_serving = counts[5];
  report.spines_failed = counts[6];
  report.spines_draining = counts[7];
  report.disabled_link_count = counts[8];
  report.generation = TopologyGeneration{generation.value()};
  report.epoch = Epoch{epoch.value()};
  report.controller = ControllerIncarnation{controller_hi.value(), controller_lo.value()};
  report.topology_digest = digest.value();
  return report;
}

[[nodiscard]] Status encode_status(ByteWriter& writer, const RuntimeStatus& status) {
  Status result = writer.u8(status.started ? 1U : 0U);
  result = result.ok() ? writer.u64(status.fabric_id.value) : result;
  result = result.ok() ? writer.string(status.name) : result;
  result = result.ok() ? writer.string(status.site) : result;
  result = result.ok() ? writer.u8(static_cast<std::uint8_t>(status.fabric_state)) : result;
  result = result.ok() ? writer.u64(status.controller.hi) : result;
  result = result.ok() ? writer.u64(status.controller.lo) : result;
  result = result.ok() ? writer.u64(status.epoch.value) : result;
  result = result.ok() ? writer.u64(status.generation.value) : result;
  result = result.ok() ? encode_digest(writer, status.topology_digest) : result;
  result = result.ok() ? encode_digest(writer, status.evidence_digest) : result;
  result = result.ok() ? writer.u8(static_cast<std::uint8_t>(status.health)) : result;
  for (const std::uint32_t value : {status.leaf_count, status.spine_count, status.port_count, status.link_count,
                                    status.history_link_count, status.evidence_count}) {
    result = result.ok() ? writer.u32(value) : result;
  }
  for (const std::uint64_t value : {status.commands_applied, status.commands_refused, status.commands_fenced,
                                    status.commands_duplicated, status.queries_served}) {
    result = result.ok() ? writer.u64(value) : result;
  }
  result = result.ok() ? writer.u32(status.worker_threads) : result;
  result = result.ok() ? writer.u8(status.persistence_enabled ? 1U : 0U) : result;
  result = result.ok() ? writer.u64(status.active_lease.value) : result;
  result = result.ok() ? writer.u64(status.lease_expires_at_unix_ms) : result;
  result = result.ok() ? writer.u64(status.uptime_ms) : result;
  result = result.ok() ? writer.u8(static_cast<std::uint8_t>(status.fault_point)) : result;
  result = result.ok() ? writer.u16(static_cast<std::uint16_t>(status.recovery.status)) : result;
  result = result.ok() ? writer.u16(static_cast<std::uint16_t>(status.recovery.code)) : result;
  result = result.ok() ? writer.string(clamp_detail(status.recovery.detail)) : result;
  result = result.ok() ? writer.u64(status.recovery.records_valid) : result;
  result = result.ok() ? writer.u64(status.recovery.bytes_dropped) : result;
  result = result.ok() ? writer.u8(status.recovery.snapshot_used ? 1U : 0U) : result;
  result = result.ok() ? writer.u8(status.recovery.dynamic_evidence_recovered ? 1U : 0U) : result;
  result = result.ok() ? writer.u8(status.recovery.requires_reobservation ? 1U : 0U) : result;
  return result;
}

[[nodiscard]] Outcome<RuntimeStatus> decode_status(ByteReader& reader) {
  RuntimeStatus status;
  const auto started = reader.u8();
  const auto fabric_id = reader.u64();
  const auto name = reader.string();
  const auto site = reader.string();
  const auto state = reader.u8();
  const auto controller_hi = reader.u64();
  const auto controller_lo = reader.u64();
  const auto epoch = reader.u64();
  const auto generation = reader.u64();
  const auto topology_digest = decode_digest(reader);
  const auto evidence_digest = decode_digest(reader);
  const auto health = reader.u8();
  if (!started.ok() || !fabric_id.ok() || !name.ok() || !site.ok() || !state.ok() || !controller_hi.ok() ||
      !controller_lo.ok() || !epoch.ok() || !generation.ok() || !topology_digest.ok() ||
      !evidence_digest.ok() || !health.ok()) {
    return Status(StatusCode::Truncated, "runtime status is incomplete");
  }
  status.started = started.value() != 0;
  status.fabric_id = FabricId{fabric_id.value()};
  status.name = name.value();
  status.site = site.value();
  if (state.value() > static_cast<std::uint8_t>(FabricState::Retired) ||
      health.value() > static_cast<std::uint8_t>(FabricHealth::Indeterminate)) {
    return Status(StatusCode::Invalid, "runtime status carries an unknown enumeration value");
  }
  status.fabric_state = static_cast<FabricState>(state.value());
  status.controller = ControllerIncarnation{controller_hi.value(), controller_lo.value()};
  status.epoch = Epoch{epoch.value()};
  status.generation = TopologyGeneration{generation.value()};
  status.topology_digest = topology_digest.value();
  status.evidence_digest = evidence_digest.value();
  status.health = static_cast<FabricHealth>(health.value());
  std::uint32_t counts[6] = {0, 0, 0, 0, 0, 0};
  for (auto& count : counts) {
    const auto value = reader.u32();
    if (!value.ok()) {
      return Status(StatusCode::Truncated, "runtime status counter is incomplete");
    }
    count = value.value();
  }
  std::uint64_t counters[5] = {0, 0, 0, 0, 0};
  for (auto& counter : counters) {
    const auto value = reader.u64();
    if (!value.ok()) {
      return Status(StatusCode::Truncated, "runtime status counter is incomplete");
    }
    counter = value.value();
  }
  const auto workers = reader.u32();
  const auto persistence = reader.u8();
  const auto lease = reader.u64();
  const auto lease_expiry = reader.u64();
  const auto uptime = reader.u64();
  const auto fault = reader.u8();
  const auto recovery_status = reader.u16();
  const auto recovery_code = reader.u16();
  const auto recovery_detail = reader.string();
  const auto records_valid = reader.u64();
  const auto bytes_dropped = reader.u64();
  const auto snapshot_used = reader.u8();
  const auto evidence_recovered = reader.u8();
  const auto needs_reobservation = reader.u8();
  if (!workers.ok() || !persistence.ok() || !lease.ok() || !lease_expiry.ok() || !uptime.ok() ||
      !fault.ok() || !recovery_status.ok() || !recovery_code.ok() || !recovery_detail.ok() ||
      !records_valid.ok() || !bytes_dropped.ok() || !snapshot_used.ok() || !evidence_recovered.ok() ||
      !needs_reobservation.ok()) {
    return Status(StatusCode::Truncated, "runtime status trailer is incomplete");
  }
  if (fault.value() > static_cast<std::uint8_t>(FaultPoint::AfterAckBeforeSnapshot) ||
      recovery_status.value() > static_cast<std::uint8_t>(RecoveryStatus::IoError)) {
    return Status(StatusCode::Invalid, "runtime status carries an unknown enumeration value");
  }
  status.leaf_count = counts[0];
  status.spine_count = counts[1];
  status.port_count = counts[2];
  status.link_count = counts[3];
  status.history_link_count = counts[4];
  status.evidence_count = counts[5];
  status.commands_applied = counters[0];
  status.commands_refused = counters[1];
  status.commands_fenced = counters[2];
  status.commands_duplicated = counters[3];
  status.queries_served = counters[4];
  status.worker_threads = workers.value();
  status.persistence_enabled = persistence.value() != 0;
  status.active_lease = LeaseId{lease.value()};
  status.lease_expires_at_unix_ms = lease_expiry.value();
  status.uptime_ms = uptime.value();
  status.fault_point = static_cast<FaultPoint>(fault.value());
  status.recovery.status = static_cast<RecoveryStatus>(recovery_status.value());
  status.recovery.code = static_cast<StatusCode>(recovery_code.value());
  status.recovery.detail = recovery_detail.value();
  status.recovery.records_valid = records_valid.value();
  status.recovery.bytes_dropped = bytes_dropped.value();
  status.recovery.snapshot_used = snapshot_used.value() != 0;
  status.recovery.dynamic_evidence_recovered = evidence_recovered.value() != 0;
  status.recovery.requires_reobservation = needs_reobservation.value() != 0;
  return status;
}

[[nodiscard]] Status encode_token(ByteWriter& writer, const AuthorityToken& token) {
  Status status = writer.u64(token.controller.hi);
  status = status.ok() ? writer.u64(token.controller.lo) : status;
  status = status.ok() ? writer.u64(token.epoch.value) : status;
  status = status.ok() ? writer.u64(token.generation.value) : status;
  status = status.ok() ? writer.u64(token.lease.value) : status;
  status = status.ok() ? encode_digest(writer, token.topology_digest) : status;
  status = status.ok() ? writer.u64(token.issued_at_unix_ms) : status;
  status = status.ok() ? writer.u64(token.expires_at_unix_ms) : status;
  return status;
}

[[nodiscard]] Outcome<AuthorityToken> decode_token(ByteReader& reader) {
  AuthorityToken token;
  const auto hi = reader.u64();
  const auto lo = reader.u64();
  const auto epoch = reader.u64();
  const auto generation = reader.u64();
  const auto lease = reader.u64();
  const auto digest = decode_digest(reader);
  const auto issued = reader.u64();
  const auto expires = reader.u64();
  if (!hi.ok() || !lo.ok() || !epoch.ok() || !generation.ok() || !lease.ok() || !digest.ok() ||
      !issued.ok() || !expires.ok()) {
    return Status(StatusCode::Truncated, "authority token is incomplete");
  }
  token.controller = ControllerIncarnation{hi.value(), lo.value()};
  token.epoch = Epoch{epoch.value()};
  token.generation = TopologyGeneration{generation.value()};
  token.lease = LeaseId{lease.value()};
  token.topology_digest = digest.value();
  token.issued_at_unix_ms = issued.value();
  token.expires_at_unix_ms = expires.value();
  return token;
}

[[nodiscard]] Status encode_command_result(ByteWriter& writer, const CommandResult& result) {
  Status status = writer.u16(static_cast<std::uint16_t>(result.code));
  status = status.ok() ? writer.string(clamp_detail(result.detail)) : status;
  status = status.ok() ? writer.u64(result.request_id.value) : status;
  status = status.ok() ? writer.u64(result.generation.value) : status;
  status = status.ok() ? writer.u64(result.epoch.value) : status;
  status = status.ok() ? encode_digest(writer, result.topology_digest) : status;
  status = status.ok() ? writer.u8(result.deduplicated ? 1U : 0U) : status;
  status = status.ok() ? writer.u8(result.mutated ? 1U : 0U) : status;
  status = status.ok() ? encode_token(writer, result.refreshed_token) : status;
  return status;
}

[[nodiscard]] Outcome<CommandResult> decode_command_result(ByteReader& reader) {
  CommandResult result;
  const auto code = reader.u16();
  const auto detail = reader.string();
  const auto request = reader.u64();
  const auto generation = reader.u64();
  const auto epoch = reader.u64();
  const auto digest = decode_digest(reader);
  const auto deduplicated = reader.u8();
  const auto mutated = reader.u8();
  const auto token = decode_token(reader);
  if (!code.ok() || !detail.ok() || !request.ok() || !generation.ok() || !epoch.ok() || !digest.ok() ||
      !deduplicated.ok() || !mutated.ok() || !token.ok()) {
    return Status(StatusCode::Truncated, "command result is incomplete");
  }
  result.code = static_cast<StatusCode>(code.value());
  result.detail = detail.value();
  result.request_id = RequestId{request.value()};
  result.generation = TopologyGeneration{generation.value()};
  result.epoch = Epoch{epoch.value()};
  result.topology_digest = digest.value();
  result.deduplicated = deduplicated.value() != 0;
  result.mutated = mutated.value() != 0;
  result.refreshed_token = token.value();
  return result;
}

}  // namespace

std::string_view to_string(RequestKind kind) noexcept {
  switch (kind) {
    case RequestKind::Hello: return "hello";
    case RequestKind::Status: return "status";
    case RequestKind::Paths: return "paths";
    case RequestKind::EligibleSpines: return "eligible_spines";
    case RequestKind::Health: return "health";
    case RequestKind::Command: return "command";
    case RequestKind::AuthorizePath: return "authorize_path";
    case RequestKind::ValidateAuthority: return "validate_authority";
    case RequestKind::Compact: return "compact";
    case RequestKind::Lease: return "lease";
    case RequestKind::Ping: return "ping";
    case RequestKind::Bye: return "bye";
  }
  return "invalid";
}

std::optional<RequestKind> parse_request_kind(std::string_view text) noexcept {
  if (text == "hello") return RequestKind::Hello;
  if (text == "status") return RequestKind::Status;
  if (text == "paths") return RequestKind::Paths;
  if (text == "eligible_spines") return RequestKind::EligibleSpines;
  if (text == "health") return RequestKind::Health;
  if (text == "command") return RequestKind::Command;
  if (text == "authorize_path") return RequestKind::AuthorizePath;
  if (text == "validate_authority") return RequestKind::ValidateAuthority;
  if (text == "compact") return RequestKind::Compact;
  if (text == "lease") return RequestKind::Lease;
  if (text == "ping") return RequestKind::Ping;
  if (text == "bye") return RequestKind::Bye;
  return std::nullopt;
}

std::vector<std::byte> encode_request(const Request& request) {
  ByteWriter writer(kMaxBlockBytes);
  (void)writer.string(kRequestDomain);
  (void)writer.u64(request.request_id.value);
  (void)writer.u16(static_cast<std::uint16_t>(request.kind));
  (void)writer.u16(request.protocol_version);
  (void)writer.string(clamp_detail(request.client_name, kMaxNameBytes));
  (void)encode_node(writer, request.from);
  (void)encode_node(writer, request.to);
  (void)encode_constraints(writer, request.constraints);
  const std::vector<std::byte> command = serialize_command(request.command);
  (void)writer.block(command);
  const std::vector<std::byte> authority = serialize_path_authority(request.authority);
  (void)writer.block(authority);
  (void)encode_token(writer, request.token);
  (void)writer.u64(request.ttl_ms);
  return writer.buffer();
}

Outcome<Request> decode_request(std::span<const std::byte> payload, const TransportLimits& limits) {
  ByteReader reader(payload, kMaxDetailBytes);
  Request request;
  const auto domain = reader.string();
  const auto request_id = reader.u64();
  const auto kind = reader.u16();
  const auto version = reader.u16();
  const auto client_name = reader.string();
  const auto from = decode_node(reader);
  const auto to = decode_node(reader);
  const auto constraints = decode_constraints(reader);
  const auto command_bytes = reader.block(limits.max_frame_payload);
  const auto authority_bytes = reader.block(limits.max_frame_payload);
  const auto token = decode_token(reader);
  const auto ttl = reader.u64();
  if (!domain.ok() || !request_id.ok() || !kind.ok() || !version.ok() || !client_name.ok() || !from.ok() ||
      !to.ok() || !constraints.ok() || !command_bytes.ok() || !authority_bytes.ok() || !token.ok() ||
      !ttl.ok()) {
    return Status(StatusCode::Truncated, "request payload is incomplete");
  }
  if (domain.value() != kRequestDomain) {
    return Status(StatusCode::Invalid, "request payload has an unexpected domain tag");
  }
  if (kind.value() < static_cast<std::uint16_t>(RequestKind::Hello) ||
      kind.value() > static_cast<std::uint16_t>(RequestKind::Bye)) {
    return Status(StatusCode::Invalid, "request carries an unknown request kind");
  }
  const Status end = reader.expect_end();
  if (!end.ok()) {
    return end;
  }
  const auto command = deserialize_command(command_bytes.value());
  if (!command.ok()) {
    return command.status();
  }
  const auto authority = deserialize_path_authority(authority_bytes.value());
  if (!authority.ok()) {
    return authority.status();
  }
  request.request_id = RequestId{request_id.value()};
  request.kind = static_cast<RequestKind>(kind.value());
  request.protocol_version = version.value();
  request.client_name = client_name.value();
  request.from = from.value();
  request.to = to.value();
  request.constraints = constraints.value();
  request.command = command.value();
  request.authority = authority.value();
  request.token = token.value();
  request.ttl_ms = ttl.value();
  return request;
}

std::vector<std::byte> encode_response(const Response& response) {
  ByteWriter writer(kMaxBlockBytes);
  (void)writer.string(kResponseDomain);
  (void)writer.u64(response.request_id.value);
  (void)writer.u16(static_cast<std::uint16_t>(response.code));
  (void)writer.string(clamp_detail(response.detail));
  (void)writer.u16(response.protocol_version);
  (void)writer.u64(response.controller.hi);
  (void)writer.u64(response.controller.lo);
  (void)writer.u64(response.epoch.value);
  (void)writer.u64(response.generation.value);
  (void)encode_digest(writer, response.topology_digest);
  (void)writer.u32(response.max_frame_payload);
  (void)writer.string(clamp_detail(response.server_name, kMaxNameBytes));
  (void)writer.u64(response.server_uptime_ms);
  (void)encode_status(writer, response.status);
  (void)encode_assessment(writer, response.assessment);
  (void)writer.u32(static_cast<std::uint32_t>(response.spines.size()));
  for (const auto& entry : response.spines) {
    (void)encode_spine_eligibility(writer, entry);
  }
  (void)encode_health(writer, response.health);
  (void)encode_command_result(writer, response.command_result);
  const std::vector<std::byte> authority = serialize_path_authority(response.path_authority);
  (void)writer.block(authority);
  (void)writer.u16(static_cast<std::uint16_t>(response.authority_check.code));
  (void)writer.string(clamp_detail(response.authority_check.detail));
  (void)encode_token(writer, response.lease_token);
  (void)writer.string(clamp_detail(response.lease_requester, kMaxNameBytes));
  return writer.buffer();
}

Outcome<Response> decode_response(std::span<const std::byte> payload, const TransportLimits& limits) {
  ByteReader reader(payload, kMaxDetailBytes);
  Response response;
  const auto domain = reader.string();
  const auto request_id = reader.u64();
  const auto code = reader.u16();
  const auto detail = reader.string();
  const auto version = reader.u16();
  const auto controller_hi = reader.u64();
  const auto controller_lo = reader.u64();
  const auto epoch = reader.u64();
  const auto generation = reader.u64();
  const auto digest = decode_digest(reader);
  const auto max_payload = reader.u32();
  const auto server_name = reader.string();
  const auto uptime = reader.u64();
  if (!domain.ok() || !request_id.ok() || !code.ok() || !detail.ok() || !version.ok() || !controller_hi.ok() ||
      !controller_lo.ok() || !epoch.ok() || !generation.ok() || !digest.ok() || !max_payload.ok() ||
      !server_name.ok() || !uptime.ok()) {
    return Status(StatusCode::Truncated, "response header is incomplete");
  }
  if (domain.value() != kResponseDomain) {
    return Status(StatusCode::Invalid, "response payload has an unexpected domain tag");
  }
  const auto status = decode_status(reader);
  if (!status.ok()) {
    return status.status();
  }
  const auto assessment = decode_assessment(reader);
  if (!assessment.ok()) {
    return assessment.status();
  }
  const auto spine_count = reader.u32();
  if (!spine_count.ok()) {
    return Status(StatusCode::Truncated, "response spine list is incomplete");
  }
  if (spine_count.value() > kMaxWireSpines) {
    return Status(StatusCode::Exhausted, "response spine list exceeds the accepted bound");
  }
  std::vector<SpineEligibility> spines;
  spines.reserve(spine_count.value());
  for (std::uint32_t i = 0; i < spine_count.value(); ++i) {
    const auto entry = decode_spine_eligibility(reader);
    if (!entry.ok()) {
      return entry.status();
    }
    spines.push_back(entry.value());
  }
  const auto health = decode_health(reader);
  if (!health.ok()) {
    return health.status();
  }
  const auto command_result = decode_command_result(reader);
  if (!command_result.ok()) {
    return command_result.status();
  }
  const auto authority_bytes = reader.block(limits.max_frame_payload);
  if (!authority_bytes.ok()) {
    return authority_bytes.status();
  }
  const auto authority = deserialize_path_authority(authority_bytes.value());
  if (!authority.ok()) {
    return authority.status();
  }
  const auto check_code = reader.u16();
  const auto check_detail = reader.string();
  const auto lease_token = decode_token(reader);
  const auto lease_requester = reader.string();
  if (!check_code.ok() || !check_detail.ok() || !lease_token.ok() || !lease_requester.ok()) {
    return Status(StatusCode::Truncated, "response authority check is incomplete");
  }
  const Status end = reader.expect_end();
  if (!end.ok()) {
    return end;
  }
  response.request_id = RequestId{request_id.value()};
  response.code = static_cast<StatusCode>(code.value());
  response.detail = detail.value();
  response.protocol_version = version.value();
  response.controller = ControllerIncarnation{controller_hi.value(), controller_lo.value()};
  response.epoch = Epoch{epoch.value()};
  response.generation = TopologyGeneration{generation.value()};
  response.topology_digest = digest.value();
  response.max_frame_payload = max_payload.value();
  response.server_name = server_name.value();
  response.server_uptime_ms = uptime.value();
  response.status = status.value();
  response.assessment = assessment.value();
  response.spines = std::move(spines);
  response.health = health.value();
  response.command_result = command_result.value();
  response.path_authority = authority.value();
  response.authority_check.code = static_cast<StatusCode>(check_code.value());
  response.authority_check.detail = check_detail.value();
  response.lease_token = lease_token.value();
  response.lease_requester = lease_requester.value();
  return response;
}

Response RuntimeService::handle(const Request& request) {
  Response response;
  response.request_id = request.request_id;
  response.protocol_version = kProtocolVersion;
  response.max_frame_payload = 4U * 1024U * 1024U;
  response.server_name = "slf-controller";
  const RuntimeStatus status = runtime_->status();
  response.controller = status.controller;
  response.epoch = status.epoch;
  response.generation = status.generation;
  response.topology_digest = status.topology_digest;
  response.status = status;
  response.server_uptime_ms = status.uptime_ms;

  switch (request.kind) {
    case RequestKind::Hello:
    case RequestKind::Ping:
      response.code = StatusCode::Ok;
      response.detail = request.kind == RequestKind::Hello ? "hello accepted" : "pong";
      return response;
    case RequestKind::Status:
      response.code = StatusCode::Ok;
      return response;
    case RequestKind::Paths: {
      const auto assessment = runtime_->assess_paths(request.from, request.to, request.constraints);
      if (!assessment.ok()) {
        response.code = assessment.code();
        response.detail = std::string(assessment.status().detail());
        return response;
      }
      response.assessment = assessment.value();
      response.code = assessment.value().code;
      response.detail = assessment.value().summary();
      return response;
    }
    case RequestKind::EligibleSpines: {
      const auto spines = runtime_->eligible_spines(request.from, request.constraints);
      if (!spines.ok()) {
        response.code = spines.code();
        response.detail = std::string(spines.status().detail());
        return response;
      }
      response.spines = spines.value();
      response.code = StatusCode::Ok;
      std::uint32_t eligible = 0;
      for (const auto& entry : response.spines) {
        if (entry.verdict == EligibilityVerdict::Eligible) {
          ++eligible;
        }
      }
      response.detail = "eligible spines for " + to_string(request.from) + ": " +
                        std::to_string(eligible) + " of " + std::to_string(response.spines.size());
      return response;
    }
    case RequestKind::Health: {
      const auto health = runtime_->health(request.constraints);
      if (!health.ok()) {
        response.code = health.code();
        response.detail = std::string(health.status().detail());
        return response;
      }
      response.health = health.value();
      response.code = health.value().code;
      response.detail = health.value().summary;
      return response;
    }
    case RequestKind::Command: {
      response.command_result = runtime_->apply(request.command);
      response.code = response.command_result.code;
      response.detail = response.command_result.detail;
      response.generation = response.command_result.generation;
      response.topology_digest = response.command_result.topology_digest;
      return response;
    }
    case RequestKind::AuthorizePath: {
      const auto authority = runtime_->authorize_path(request.from, request.to, request.constraints, request.ttl_ms);
      if (!authority.ok()) {
        response.code = authority.code();
        response.detail = std::string(authority.status().detail());
        return response;
      }
      response.path_authority = authority.value();
      response.code = StatusCode::Ok;
      response.detail = "path authority minted";
      return response;
    }
    case RequestKind::ValidateAuthority: {
      response.authority_check = runtime_->validate_path_authority_ref(request.authority);
      response.code = response.authority_check.code;
      response.detail = response.authority_check.detail;
      return response;
    }
    case RequestKind::Lease: {
      const auto lease = runtime_->acquire_authority("protocol-client", request.ttl_ms);
      if (!lease.ok()) {
        response.code = lease.code();
        response.detail = std::string(lease.status().detail());
        return response;
      }
      response.lease_token = lease.value().token;
      response.lease_requester = lease.value().requester;
      response.code = StatusCode::Ok;
      response.detail = "lease minted";
      return response;
    }
    case RequestKind::Compact: {
      const Status compacted = runtime_->compact();
      response.code = compacted.code();
      response.detail = compacted.ok() ? "snapshot written" : std::string(compacted.detail());
      return response;
    }
    case RequestKind::Bye:
      response.code = StatusCode::Ok;
      response.detail = "bye";
      return response;
  }
  response.code = StatusCode::Unsupported;
  response.detail = "unhandled request kind";
  return response;
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

struct FabricServer::Impl {
  ServerConfig config;
  RequestHandler* handler{nullptr};
  TcpListener listener;
  std::atomic<bool> running{false};
  std::mutex lifecycle_mutex;
  bool lifecycle_started{false};
  std::jthread accept_thread;

  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::deque<Connection> queue;
  std::vector<std::jthread> workers;

  std::mutex active_mutex;
  std::vector<std::intptr_t> active_sockets;

  mutable std::mutex stats_mutex;
  ServerStats stats;

  void register_active(std::intptr_t socket) {
    std::lock_guard<std::mutex> guard(active_mutex);
    active_sockets.push_back(socket);
  }

  void unregister_active(std::intptr_t socket) {
    std::lock_guard<std::mutex> guard(active_mutex);
    const auto it = std::find(active_sockets.begin(), active_sockets.end(), socket);
    if (it != active_sockets.end()) {
      active_sockets.erase(it);
    }
  }

  void shutdown_active() {
    std::lock_guard<std::mutex> guard(active_mutex);
    for (const auto socket : active_sockets) {
      // Shutting the socket down (not closing it) makes the owning thread's
      // blocking receive return immediately without racing the file handle.
      shutdown_socket(socket);
    }
  }

  void count_accepted(bool accepted) {
    std::lock_guard<std::mutex> guard(stats_mutex);
    if (accepted) {
      ++stats.accepted_connections;
    } else {
      ++stats.rejected_connections;
    }
  }
};

FabricServer::~FabricServer() {
  if (impl_ != nullptr) {
    const Status status = stop();
    (void)status;
  }
}

Outcome<std::unique_ptr<FabricServer>> FabricServer::create(ServerConfig config, RequestHandler* handler) {
  if (handler == nullptr) {
    return Status(StatusCode::Invalid, "server requires a request handler");
  }
  if (config.limits.max_connections == 0 || config.limits.max_connections > 4096) {
    return Status(StatusCode::Invalid, "max_connections must be in 1..4096");
  }
  auto server = std::unique_ptr<FabricServer>(new FabricServer());
  server->impl_ = std::make_unique<Impl>();
  server->impl_->config = std::move(config);
  server->impl_->handler = handler;
  server->port_ = 0;
  return server;
}

Status FabricServer::start() {
  if (impl_ == nullptr) {
    return Status(StatusCode::Invalid, "server was not created");
  }
  if (impl_->running.load(std::memory_order_acquire)) {
    return Status::success();
  }
  Outcome<TcpListener> listener = TcpListener::bind(impl_->config.endpoint);
  if (!listener.ok()) {
    return listener.status();
  }
  impl_->listener = std::move(listener.value());
  port_ = impl_->listener.port();
  impl_->running.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> guard(impl_->lifecycle_mutex);
    impl_->lifecycle_started = true;
  }

  const std::uint32_t worker_count =
      std::max<std::uint32_t>(1, std::min<std::uint32_t>(impl_->config.limits.max_connections, 16));
  for (std::uint32_t i = 0; i < worker_count; ++i) {
    impl_->workers.emplace_back([this](std::stop_token token) {
      Impl& impl = *impl_;
      while (!token.stop_requested()) {
        Connection connection;
        {
          std::unique_lock<std::mutex> lock(impl.queue_mutex);
          impl.queue_cv.wait_for(lock, std::chrono::milliseconds(20), [&impl]() {
            return !impl.queue.empty() || !impl.running.load(std::memory_order_acquire);
          });
          if (!impl.queue.empty()) {
            connection = std::move(impl.queue.front());
            impl.queue.pop_front();
          } else if (!impl.running.load(std::memory_order_acquire)) {
            return;
          } else {
            continue;
          }
        }
        if (!connection.valid()) {
          continue;
        }
        const std::intptr_t handle = connection.native_handle();
        impl.register_active(handle);
        // Bounded IO on the accepted socket: a peer that stops sending must not
        // park this worker until shutdown.
        (void)connection.set_deadlines(impl.config.limits.io_timeout_ms);
        std::uint32_t frames = 0;
        while (impl.running.load(std::memory_order_acquire) &&
               frames < impl.config.limits.max_frames_per_connection) {
          FrameHeader header{};
          const auto payload = connection.read_frame(&header, impl.config.limits);
          if (!payload.ok()) {
            if (payload.code() == StatusCode::IncompatibleVersion ||
                payload.code() == StatusCode::Corrupt || payload.code() == StatusCode::IntegrityError) {
              std::lock_guard<std::mutex> guard(impl.stats_mutex);
              ++impl.stats.protocol_errors;
              // A hostile or corrupt frame gets a typed error reply, then the
              // connection is closed: the stream framing can no longer be
              // trusted after a bad length or digest.
              Response error;
              error.request_id = RequestId{header.request_id};
              error.code = payload.code();
              error.detail = std::string(payload.status().detail());
              const std::vector<std::byte> encoded = encode_response(error);
              FrameHeader reply{};
              reply.type = static_cast<std::uint16_t>(RequestKind::Bye);
              reply.flags = FrameHeader::kFlagReply;
              reply.request_id = header.request_id;
              (void)connection.write_frame(reply, encoded, impl.config.limits);
            }
            break;
          }
          ++frames;
          {
            std::lock_guard<std::mutex> guard(impl.stats_mutex);
            ++impl.stats.frames_read;
            impl.stats.bytes_in += payload.value().size() + FrameHeader::kHeaderBytes;
          }
          const auto request = decode_request(payload.value(), impl.config.limits);
          Response response;
          if (!request.ok()) {
            response.request_id = RequestId{header.request_id};
            response.code = request.code();
            response.detail = std::string(request.status().detail());
            std::lock_guard<std::mutex> guard(impl.stats_mutex);
            ++impl.stats.protocol_errors;
          } else if (request.value().protocol_version != kProtocolVersion) {
            response.request_id = request.value().request_id;
            response.code = StatusCode::IncompatibleVersion;
            response.detail = "client protocol version is not supported";
            std::lock_guard<std::mutex> guard(impl.stats_mutex);
            ++impl.stats.protocol_errors;
          } else {
            response = impl.handler->handle(request.value());
          }
          const std::vector<std::byte> encoded = encode_response(response);
          FrameHeader reply{};
          reply.type = static_cast<std::uint16_t>(request.ok() ? request.value().kind : RequestKind::Bye);
          reply.flags = FrameHeader::kFlagReply;
          reply.request_id = response.request_id.value;
          // The frame counters are updated after the send: holding the stats
          // lock across a blocking write would stall every other worker and
          // every stats() reader behind a stalled peer.
          const Status written = connection.write_frame(reply, encoded, impl.config.limits);
          {
            std::lock_guard<std::mutex> guard(impl.stats_mutex);
            if (written.ok()) {
              ++impl.stats.frames_written;
              impl.stats.bytes_out += encoded.size() + FrameHeader::kHeaderBytes;
            } else {
              ++impl.stats.protocol_errors;
            }
          }
          if (!written.ok()) {
            break;
          }
          if (request.ok() && request.value().kind == RequestKind::Bye) {
            break;
          }
        }
        impl.unregister_active(handle);
        connection.close();
      }
    });
  }

  impl_->accept_thread = std::jthread([this](std::stop_token token) {
    Impl& impl = *impl_;
    while (!token.stop_requested() && impl.running.load(std::memory_order_acquire)) {
      auto connection = impl.listener.accept(50);
      if (!connection.ok()) {
        if (connection.code() == StatusCode::DeadlineExpired) {
          continue;
        }
        break;
      }
      bool accepted = false;
      {
        std::lock_guard<std::mutex> guard(impl.queue_mutex);
        if (impl.queue.size() < impl.config.limits.max_connections) {
          impl.queue.push_back(std::move(connection.value()));
          accepted = true;
        }
      }
      impl.count_accepted(accepted);
      if (accepted) {
        impl.queue_cv.notify_one();
      }
    }
  });
  return Status::success();
}

Status FabricServer::run_until_stopped() {
  if (impl_ == nullptr) {
    return Status(StatusCode::Invalid, "server was not created");
  }
  while (impl_->running.load(std::memory_order_acquire)) {
    platform::sleep_ms(5);
  }
  return Status::success();
}

Status FabricServer::stop() {
  if (impl_ == nullptr) {
    return Status::success();
  }
  // One stop at a time: the accept thread and the worker pool are joined and
  // cleared here, so two concurrent stops would join the same threads twice.
  std::lock_guard<std::mutex> stop_guard(impl_->lifecycle_mutex);
  if (!impl_->lifecycle_started) {
    return Status::success();
  }
  impl_->lifecycle_started = false;
  const bool was_running = impl_->running.exchange(false, std::memory_order_acq_rel);
  impl_->accept_thread.request_stop();
  impl_->listener.close();
  if (impl_->accept_thread.joinable()) {
    impl_->accept_thread.join();
  }
  impl_->queue_cv.notify_all();
  impl_->shutdown_active();
  for (auto& worker : impl_->workers) {
    worker.request_stop();
  }
  for (auto& worker : impl_->workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  impl_->workers.clear();
  {
    std::lock_guard<std::mutex> guard(impl_->queue_mutex);
    impl_->queue.clear();
  }
  (void)was_running;
  return Status::success();
}

bool FabricServer::running() const noexcept {
  return impl_ != nullptr && impl_->running.load(std::memory_order_acquire);
}

ServerStats FabricServer::stats() const {
  if (impl_ == nullptr) {
    return ServerStats{};
  }
  std::lock_guard<std::mutex> guard(impl_->stats_mutex);
  ServerStats snapshot = impl_->stats;
  {
    std::lock_guard<std::mutex> active_guard(impl_->active_mutex);
    snapshot.active_connections = static_cast<std::uint32_t>(impl_->active_sockets.size());
  }
  return snapshot;
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

struct FabricClient::Impl {
  ClientConfig config;
  Connection connection;
  std::mutex mutex;
  std::atomic<std::uint64_t> next_request_id{1};
};

FabricClient::~FabricClient() {
  if (impl_ != nullptr) {
    const Status status = close();
    (void)status;
  }
}

Outcome<std::unique_ptr<FabricClient>> FabricClient::connect(const ClientConfig& config) {
  auto connection = Connection::connect(config.endpoint, config.limits);
  if (!connection.ok()) {
    return connection.status();
  }
  auto client = std::unique_ptr<FabricClient>(new FabricClient());
  client->impl_ = std::make_unique<Impl>();
  client->impl_->config = config;
  client->impl_->connection = std::move(connection.value());
  return client;
}

Outcome<Response> FabricClient::call(const Request& request, std::uint64_t timeout_ms) {
  if (impl_ == nullptr) {
    return Status(StatusCode::Invalid, "client was not connected");
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (timeout_ms != 0) {
    const Status deadlines = impl_->connection.set_deadlines(timeout_ms);
    if (!deadlines.ok()) {
      return deadlines;
    }
  }
  const std::vector<std::byte> payload = encode_request(request);
  FrameHeader header{};
  header.type = static_cast<std::uint16_t>(request.kind);
  header.request_id = request.request_id.value;
  const Status written = impl_->connection.write_frame(header, payload, impl_->config.limits);
  if (!written.ok()) {
    return written;
  }
  FrameHeader reply{};
  const auto reply_payload = impl_->connection.read_frame(&reply, impl_->config.limits);
  if (!reply_payload.ok()) {
    return reply_payload.status();
  }
  if ((reply.flags & FrameHeader::kFlagReply) == 0U) {
    return Status(StatusCode::Invalid, "server sent a request frame where a reply was expected");
  }
  return decode_response(reply_payload.value(), impl_->config.limits);
}

Status FabricClient::close() {
  if (impl_ == nullptr) {
    return Status::success();
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  Status result = Status::success();
  if (impl_->connection.valid()) {
    Request bye;
    bye.kind = RequestKind::Bye;
    bye.request_id = RequestId{impl_->next_request_id.fetch_add(1, std::memory_order_relaxed)};
    const std::vector<std::byte> payload = encode_request(bye);
    FrameHeader header{};
    header.type = static_cast<std::uint16_t>(RequestKind::Bye);
    header.request_id = bye.request_id.value;
    const Status written = impl_->connection.write_frame(header, payload, impl_->config.limits);
    if (!written.ok()) {
      result = written;
    }
    const auto reply = impl_->connection.read_frame(nullptr, impl_->config.limits);
    if (!reply.ok() && result.ok()) {
      result = reply.status();
    }
  }
  impl_->connection.close();
  return result;
}

bool FabricClient::connected() const noexcept {
  return impl_ != nullptr && impl_->connection.valid();
}

Outcome<Response> handshake(FabricClient& client, std::string_view client_name) {
  Request hello;
  hello.kind = RequestKind::Hello;
  hello.protocol_version = kProtocolVersion;
  hello.client_name = std::string(client_name);
  hello.request_id = RequestId{1};
  auto response = client.call(hello);
  if (!response.ok()) {
    return response.status();
  }
  if (response.value().code != StatusCode::Ok) {
    return Status(response.value().code, response.value().detail);
  }
  if (response.value().protocol_version != kProtocolVersion) {
    return Status(StatusCode::IncompatibleVersion, "server protocol version is not supported");
  }
  return response;
}

}  // namespace slf::net
