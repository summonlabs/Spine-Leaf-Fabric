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
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "slf/model.hpp"

namespace slf {

/// Incrementally assembles a fabric and validates it as a whole.
///
/// The builder refuses records immediately when the refusal is local and
/// unambiguous (duplicate identity, bound violation, malformed text,
/// contradictory role, cross-generation reference). Whole-graph properties
/// (dangling edges, illegal adjacency, contradictory capacity, evidence for an
/// unknown link) are validated by c freeze(), which produces a
/// c ValidationReport plus the immutable c Fabric.
///
/// c freeze() never repairs silently. A fabric with error-level issues is
/// refused with the corresponding status code; warnings and info issues are
/// reported alongside a successful freeze.
class FabricBuilder {
 public:
  FabricBuilder(FabricId id, std::string name, TopologyGeneration generation, Epoch epoch,
                FabricOptions options = {}, BuilderLimits limits = {});

  [[nodiscard]] Status set_site(std::string site);
  [[nodiscard]] Status set_state(FabricState state);

  [[nodiscard]] Status add_failure_domain(FailureDomainRecord record);
  [[nodiscard]] Status add_leaf(LeafRecord record);
  [[nodiscard]] Status add_spine(SpineRecord record);
  [[nodiscard]] Status add_port(PortRecord record);
  [[nodiscard]] Status add_link(LinkRecord record);
  [[nodiscard]] Status add_evidence(LinkEvidence record);

  /// Retains a link from an older generation as evidence. Older-generation
  /// links never participate in eligibility.
  [[nodiscard]] Status add_history_link(LinkRecord record);

  /// Validates and freezes the fabric. The returned fabric is sorted and
  /// digested; iteration order is independent of insertion order.
  [[nodiscard]] Outcome<Fabric> freeze();

  /// Report from the most recent c freeze() (or the issues collected so far).
  [[nodiscard]] const ValidationReport& report() const noexcept { return report_; }

  /// Issues collected so far, in insertion order.
  [[nodiscard]] const std::vector<ValidationIssue>& issues() const noexcept { return report_.issues; }

  [[nodiscard]] const BuilderLimits& limits() const noexcept { return limits_; }
  [[nodiscard]] TopologyGeneration generation() const noexcept { return generation_; }

 private:
  [[nodiscard]] Status note(IssueSeverity severity, StatusCode code, std::string where, std::string detail);
  [[nodiscard]] Status validate_node_common(NodeKey key, std::string_view name, TopologyGeneration generation,
                                            FailureDomainId domain, std::uint64_t capacity_mbps);
  void validate_domains();
  void validate_ports();
  void validate_links();
  void validate_evidence();
  [[nodiscard]] Status finish_report();

  FabricId id_;
  std::string name_;
  std::string site_;
  FabricState fabric_state_{FabricState::Operational};
  TopologyGeneration generation_{};
  Epoch epoch_{};
  FabricOptions options_{};
  BuilderLimits limits_{};

  std::map<FailureDomainId, FailureDomainRecord> domains_;
  std::map<LeafId, LeafRecord> leaves_;
  std::map<SpineId, SpineRecord> spines_;
  std::map<PortId, PortRecord> ports_;
  std::map<LinkId, LinkRecord> links_;
  std::map<LinkId, LinkRecord> history_links_;
  std::vector<LinkEvidence> evidence_;

  ValidationReport report_{};
  bool frozen_{false};
};

/// Convenience helper: builds a fabric from records and returns the report.
[[nodiscard]] Outcome<Fabric> build_fabric(FabricId id, std::string name, TopologyGeneration generation,
                                           Epoch epoch, FabricOptions options,
                                           const std::vector<LeafRecord>& leaves,
                                           const std::vector<SpineRecord>& spines,
                                           const std::vector<PortRecord>& ports,
                                           const std::vector<LinkRecord>& links,
                                           ValidationReport* report_out = nullptr,
                                           BuilderLimits limits = {});

}  // namespace slf
