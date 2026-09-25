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
#include <string>
#include <vector>

#include "slf/cancellation.hpp"
#include "slf/capacity.hpp"
#include "slf/model.hpp"
#include "slf/topology.hpp"

namespace slf {

/// Verdict of an operational eligibility question. Three valued on purpose:
/// c Indeterminate is the honest answer when required evidence is missing,
/// stale, or contradictory, and is never folded into c Ineligible or
/// c Eligible.
enum class EligibilityVerdict : std::uint8_t {
  Eligible = 0,
  Ineligible = 1,
  Indeterminate = 2,
};

[[nodiscard]] std::string_view to_string(EligibilityVerdict verdict) noexcept;

/// Constraints applied to an eligibility query. Defaults come from the fabric
/// policy; a query may tighten them.
struct PathConstraints {
  /// Require the selected spine set to span distinct failure domains.
  bool require_domain_diversity{false};
  /// Minimum number of distinct eligible spines.
  std::uint32_t min_eligible_spines{1};
  /// Minimum total uplink capacity of the eligible spine set.
  std::uint64_t min_total_capacity_mbps{0};
  /// Maximum allowed oversubscription (access/uplink). Absent means unchecked.
  std::optional<Rational> max_oversubscription{};
  /// Consider same-tier peer links as path segments.
  bool allow_peer_paths{true};
  /// Require fresh observed evidence for every link in the path. When false the
  /// fabric policy decides (see c FabricOptions::evidence_policy).
  bool require_evidence{false};
  /// Upper bound on the number of diverse spine sets reported.
  std::uint32_t max_sets{16};
  /// Upper bound on the number of explanation lines.
  std::uint32_t max_explanations{32};

  [[nodiscard]] static PathConstraints defaults_for(const FabricOptions& options) noexcept;
};

/// Per-spine eligibility decision with the reason attached.
struct SpineEligibility {
  SpineId spine{};
  EligibilityVerdict verdict{EligibilityVerdict::Ineligible};
  StatusCode code{StatusCode::Ok};
  bool structural_ok{false};
  EvidenceFreshness freshness{EvidenceFreshness::Absent};
  std::uint64_t capacity_mbps{0};
  FailureDomainId domain{};
  std::string detail;
};

/// A set of spines usable together, with its aggregate capacity and diversity.
struct EligibleSpineSet {
  std::vector<SpineId> spines{};
  std::vector<FailureDomainId> domains{};
  std::uint64_t total_capacity_mbps{0};
  std::uint32_t distinct_domains{0};

  friend bool operator==(const EligibleSpineSet&, const EligibleSpineSet&) = default;
};

struct EligibleSpineSets {
  std::vector<EligibleSpineSet> sets{};
  /// True when the candidate combinations exceeded the requested bound and only
  /// the best c max_sets sets are reported.
  bool truncated{false};
  /// Number of candidate maximal-diversity combinations examined (exact when
  /// not truncated, a lower bound when truncated).
  std::uint64_t candidate_combinations{0};
};

/// Generation-bound explanation of one leaf-to-leaf (or leaf-to-spine) path
/// question. Every assessment carries the coordinate it was computed at, so a
/// stored assessment can be fenced when the coordinate moves on.
struct PathAssessment {
  NodeKey from{};
  NodeKey to{};
  ReachabilityClass cls{ReachabilityClass::Unreachable};
  EligibilityVerdict verdict{EligibilityVerdict::Ineligible};
  /// Dominant reason: c Ok when eligible, otherwise the code that decides the
  /// verdict.
  StatusCode code{StatusCode::Ok};
  TopologyGeneration generation{};
  Epoch epoch{};
  ControllerIncarnation controller{};
  Digest topology_digest{};
  std::vector<SpineId> eligible_spines{};
  std::vector<FailureDomainId> covered_domains{};
  std::uint32_t max_domain_loss_tolerance{0};
  EligibleSpineSets spine_sets{};
  CapacitySummary capacity{};
  std::vector<std::string> explanation{};
  std::uint64_t links_examined{0};
  bool evidence_required{false};

  [[nodiscard]] std::string summary() const;
  [[nodiscard]] std::string to_string() const;
};

/// Aggregate health of the fabric at one generation.
struct FabricHealthReport {
  FabricHealth health{FabricHealth::Unformed};
  StatusCode code{StatusCode::Ok};
  std::string summary;
  std::vector<std::string> reasons;
  std::uint32_t leaves_total{0};
  std::uint32_t leaves_serving{0};
  std::uint32_t leaves_isolated{0};
  std::uint32_t leaves_indeterminate{0};
  std::uint32_t spines_total{0};
  std::uint32_t spines_serving{0};
  std::uint32_t spines_failed{0};
  std::uint32_t spines_draining{0};
  std::uint32_t disabled_link_count{0};
  TopologyGeneration generation{};
  Epoch epoch{};
  ControllerIncarnation controller{};
  Digest topology_digest{};

  [[nodiscard]] std::string to_string() const;
};

/// Eligibility engine over an immutable topology index.
///
/// The engine is stateless with respect to mutable data: it holds a reference
/// to an index that is never mutated, so queries from any number of threads are
/// safe without locking, and a query can never observe a half-applied mutation.
class EligibilityEngine {
 public:
  /// p controller is the incarnation whose observations count as fresh. The
  /// default (zero) incarnation means "no live observer", so under
  /// c EvidencePolicy::RequireFresh every evidence record is historical and no
  /// path is reported eligible without re-observation.
  explicit EligibilityEngine(const TopologyIndex& index, ControllerIncarnation controller = {})
      : index_(&index), controller_(controller) {}

  [[nodiscard]] const TopologyIndex& index() const noexcept { return *index_; }
  [[nodiscard]] ControllerIncarnation controller() const noexcept { return controller_; }

  /// Eligibility of every spine with respect to one leaf.
  [[nodiscard]] Outcome<std::vector<SpineEligibility>> eligible_spines_for_leaf(
      LeafId leaf, const PathConstraints& constraints, CancellationToken token = {}) const;

  /// Full assessment of a leaf-to-leaf question.
  [[nodiscard]] Outcome<PathAssessment> assess_leaf_pair(LeafId from, LeafId to,
                                                         const PathConstraints& constraints,
                                                         CancellationToken token = {}) const;

  /// Assessment of a leaf-to-spine question.
  [[nodiscard]] Outcome<PathAssessment> assess_leaf_to_spine(LeafId from, SpineId to,
                                                             const PathConstraints& constraints) const;

  /// Fabric-level health classification.
  [[nodiscard]] Outcome<FabricHealthReport> health(const PathConstraints& constraints,
                                                   ControllerIncarnation controller, Epoch epoch,
                                                   CancellationToken token = {}) const;

  /// Evaluates a single link: structural admissibility plus evidence freshness.
  [[nodiscard]] Outcome<SpineEligibility> evaluate_uplink(LeafId leaf, const Adjacency& uplink,
                                                          const PathConstraints& constraints) const;

  /// True when the fabric policy (or an explicit constraint) requires fresh
  /// evidence.
  [[nodiscard]] bool requires_evidence(const PathConstraints& constraints) const noexcept;

 private:
  [[nodiscard]] Outcome<std::vector<SpineEligibility>> evaluate_uplinks(LeafId leaf,
                                                                        const PathConstraints& constraints,
                                                                        CancellationToken token) const;
  [[nodiscard]] EligibleSpineSets select_spine_sets(const std::vector<SpineEligibility>& eligible,
                                                    const PathConstraints& constraints,
                                                    CancellationToken token) const;

  const TopologyIndex* index_;
  ControllerIncarnation controller_{};
};

}  // namespace slf
