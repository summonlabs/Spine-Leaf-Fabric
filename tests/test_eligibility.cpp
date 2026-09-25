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

#include "slf/eligibility.hpp"

#include <algorithm>

#include "fixtures.hpp"
#include "slf/cancellation.hpp"
#include "slf_test.hpp"

using namespace slf;
using namespace slf::test;

namespace {

struct Grid {
  FabricBuilder builder;
  Grid(FabricOptions options, TopologyGeneration generation = TopologyGeneration{1})
      : builder(FabricId{1}, "eligibility", generation, Epoch{1}, options) {}
};

/// 2 leaves x 3 spines, one link per pair, spines in distinct domains.
Outcome<Fabric> three_spine_fabric(FabricOptions options = {}) {
  return make_grid(TopologyGeneration{1},
                   {{1, "", LeafRole::Tor, 1, NodeState::Active, 400000, 11},
                    {2, "", LeafRole::Tor, 2, NodeState::Active, 400000, 12}},
                   {{1, "", SpineRole::FabricSpine, 1, NodeState::Active, 3200000, 21},
                    {2, "", SpineRole::FabricSpine, 2, NodeState::Active, 3200000, 22},
                    {3, "", SpineRole::FabricSpine, 3, NodeState::Active, 3200000, 23}},
                   {{1, 1, 100000}, {1, 2, 100000}, {1, 3, 100000},
                    {2, 1, 100000}, {2, 2, 100000}, {2, 3, 100000}},
                   options);
}

}  // namespace

SLF_TEST(eligibility_config_only_verdicts) {
  const auto fabric = three_spine_fabric();
  SLF_EXPECT(fabric.ok());
  const auto index = TopologyIndex::build(fabric.value());
  SLF_EXPECT(index.ok());
  EligibilityEngine engine(index.value());
  const auto constraints = PathConstraints::defaults_for(fabric.value().options);
  const auto assessment = engine.assess_leaf_pair(LeafId{1}, LeafId{2}, constraints);
  SLF_EXPECT(assessment.ok());
  SLF_EXPECT_EQ(assessment.value().verdict, EligibilityVerdict::Eligible);
  SLF_EXPECT_EQ(assessment.value().code, StatusCode::Ok);
  SLF_EXPECT_EQ(assessment.value().cls, ReachabilityClass::LeafLeafMultiSpine);
  SLF_EXPECT_EQ(assessment.value().eligible_spines.size(), 3U);
  SLF_EXPECT_EQ(assessment.value().covered_domains.size(), 3U);
  SLF_EXPECT_EQ(assessment.value().max_domain_loss_tolerance, 2U);
  SLF_EXPECT_EQ(assessment.value().capacity.uplink_capacity_mbps, 300000U);
  SLF_EXPECT_EQ(assessment.value().capacity.access_capacity_mbps, 800000U);
  SLF_EXPECT_EQ(assessment.value().capacity.classification, OversubscriptionClass::Oversubscribed);
  SLF_EXPECT_EQ(assessment.value().generation, TopologyGeneration{1});
  SLF_EXPECT_EQ(assessment.value().topology_digest, fabric.value().topology_digest);
  SLF_EXPECT(!assessment.value().explanation.empty());
  SLF_EXPECT(assessment.value().summary().find("eligible") != std::string::npos);
  SLF_EXPECT(assessment.value().to_string().find("spine set") != std::string::npos);

  // A path to itself is not a path.
  const auto self = engine.assess_leaf_pair(LeafId{1}, LeafId{1}, constraints);
  SLF_EXPECT(self.ok());
  SLF_EXPECT_EQ(self.value().cls, ReachabilityClass::SameNode);
  SLF_EXPECT_EQ(self.value().verdict, EligibilityVerdict::Ineligible);
  SLF_EXPECT_EQ(self.value().code, StatusCode::Invalid);

  // Unknown leaves are refused, not silently treated as unreachable.
  SLF_EXPECT_CODE(engine.assess_leaf_pair(LeafId{1}, LeafId{9}, constraints), StatusCode::NotFound);

  // Leaf to spine.
  const auto to_spine = engine.assess_leaf_to_spine(LeafId{1}, SpineId{2}, constraints);
  SLF_EXPECT(to_spine.ok());
  SLF_EXPECT_EQ(to_spine.value().verdict, EligibilityVerdict::Eligible);
  SLF_EXPECT_EQ(to_spine.value().capacity.uplink_capacity_mbps, 100000U);
}

SLF_TEST(eligibility_structural_failures) {
  const auto fabric = three_spine_fabric();
  SLF_EXPECT(fabric.ok());

  // Drain spine 3 and fail spine 2: only spine 1 remains eligible.
  FabricBuilder builder(FabricId{1}, fabric.value().name, TopologyGeneration{2}, Epoch{1});
  for (const auto& leaf : fabric.value().leaves) {
    LeafRecord record = leaf;
    record.generation = TopologyGeneration{2};
    SLF_EXPECT(builder.add_leaf(record).ok());
  }
  for (const auto& spine : fabric.value().spines) {
    SpineRecord record = spine;
    record.generation = TopologyGeneration{2};
    if (record.id == SpineId{2}) {
      record.state = NodeState::Failed;
    }
    if (record.id == SpineId{3}) {
      record.state = NodeState::Draining;
    }
    SLF_EXPECT(builder.add_spine(record).ok());
  }
  for (const auto& port : fabric.value().ports) {
    PortRecord record = port;
    record.generation = TopologyGeneration{2};
    SLF_EXPECT(builder.add_port(record).ok());
  }
  for (const auto& link : fabric.value().links) {
    LinkRecord record = link;
    record.generation = TopologyGeneration{2};
    SLF_EXPECT(builder.add_link(record).ok());
  }
  const auto degraded = builder.freeze();
  SLF_EXPECT(degraded.ok());
  const auto index = TopologyIndex::build(degraded.value());
  SLF_EXPECT(index.ok());
  EligibilityEngine engine(index.value());
  const auto constraints = PathConstraints::defaults_for(degraded.value().options);
  const auto assessment = engine.assess_leaf_pair(LeafId{1}, LeafId{2}, constraints);
  SLF_EXPECT(assessment.ok());
  // One spine survives, which satisfies the default minimum of one.
  SLF_EXPECT_EQ(assessment.value().verdict, EligibilityVerdict::Eligible);
  SLF_EXPECT_EQ(assessment.value().code, StatusCode::Ok);
  SLF_EXPECT_EQ(assessment.value().eligible_spines.size(), 1U);
  SLF_EXPECT_EQ(assessment.value().eligible_spines.front(), SpineId{1});
  SLF_EXPECT_EQ(assessment.value().covered_domains.size(), 1U);
  SLF_EXPECT_EQ(assessment.value().max_domain_loss_tolerance, 0U);
  SLF_EXPECT(assessment.value().to_string().find("excluded") != std::string::npos);

  // Requiring two spines makes the same fabric ineligible, with the refusal
  // code rather than an undecided verdict: the exclusions are decided facts.
  PathConstraints strict = constraints;
  strict.min_eligible_spines = 2;
  const auto refused = engine.assess_leaf_pair(LeafId{1}, LeafId{2}, strict);
  SLF_EXPECT(refused.ok());
  SLF_EXPECT_EQ(refused.value().verdict, EligibilityVerdict::Ineligible);
  SLF_EXPECT_EQ(refused.value().code, StatusCode::Refused);

  // The per-spine view names the reason for each exclusion.
  const auto spines = engine.eligible_spines_for_leaf(LeafId{1}, constraints);
  SLF_EXPECT(spines.ok());
  SLF_EXPECT_EQ(spines.value().size(), 3U);
  SLF_EXPECT_EQ(spines.value()[0].verdict, EligibilityVerdict::Eligible);
  SLF_EXPECT_EQ(spines.value()[1].code, StatusCode::Refused);
  SLF_EXPECT(spines.value()[1].detail.find("failed") != std::string::npos);
  SLF_EXPECT(spines.value()[2].detail.find("draining") != std::string::npos);
}

SLF_TEST(eligibility_constraints_filter_spines) {
  const auto fabric = three_spine_fabric();
  SLF_EXPECT(fabric.ok());
  const auto index = TopologyIndex::build(fabric.value());
  SLF_EXPECT(index.ok());
  EligibilityEngine engine(index.value());
  PathConstraints constraints = PathConstraints::defaults_for(fabric.value().options);

  constraints.min_eligible_spines = 4;
  const auto too_few = engine.assess_leaf_pair(LeafId{1}, LeafId{2}, constraints);
  SLF_EXPECT(too_few.ok());
  SLF_EXPECT_EQ(too_few.value().verdict, EligibilityVerdict::Ineligible);
  SLF_EXPECT_EQ(too_few.value().code, StatusCode::Refused);

  constraints = PathConstraints::defaults_for(fabric.value().options);
  constraints.min_total_capacity_mbps = 400000;
  const auto too_small = engine.assess_leaf_pair(LeafId{1}, LeafId{2}, constraints);
  SLF_EXPECT(too_small.ok());
  SLF_EXPECT_EQ(too_small.value().verdict, EligibilityVerdict::Ineligible);

  constraints = PathConstraints::defaults_for(fabric.value().options);
  constraints.max_oversubscription = Rational{1, 1};
  const auto over = engine.assess_leaf_pair(LeafId{1}, LeafId{2}, constraints);
  SLF_EXPECT(over.ok());
  // 800000 access over 300000 uplink exceeds 1:1.
  SLF_EXPECT_EQ(over.value().verdict, EligibilityVerdict::Ineligible);

  constraints = PathConstraints::defaults_for(fabric.value().options);
  constraints.max_oversubscription = Rational{4, 1};
  const auto within = engine.assess_leaf_pair(LeafId{1}, LeafId{2}, constraints);
  SLF_EXPECT(within.ok());
  SLF_EXPECT_EQ(within.value().verdict, EligibilityVerdict::Eligible);
}

SLF_TEST(eligibility_diversity_sets_are_bounded_and_deterministic) {
  const auto fabric = three_spine_fabric();
  SLF_EXPECT(fabric.ok());
  const auto index = TopologyIndex::build(fabric.value());
  SLF_EXPECT(index.ok());
  EligibilityEngine engine(index.value());
  PathConstraints constraints = PathConstraints::defaults_for(fabric.value().options);
  constraints.require_domain_diversity = true;
  constraints.max_sets = 4;
  const auto first = engine.assess_leaf_pair(LeafId{1}, LeafId{2}, constraints);
  SLF_EXPECT(first.ok());
  SLF_EXPECT_EQ(first.value().verdict, EligibilityVerdict::Eligible);
  SLF_EXPECT(!first.value().spine_sets.sets.empty());
  SLF_EXPECT_EQ(first.value().spine_sets.sets.front().distinct_domains, 3U);
  SLF_EXPECT_EQ(first.value().spine_sets.sets.front().spines.size(), 3U);

  const auto second = engine.assess_leaf_pair(LeafId{1}, LeafId{2}, constraints);
  SLF_EXPECT(second.ok());
  SLF_EXPECT_EQ(first.value().spine_sets.sets, second.value().spine_sets.sets);
  SLF_EXPECT_EQ(first.value().summary(), second.value().summary());
}

SLF_TEST(eligibility_evidence_policy) {
  const auto base = three_spine_fabric(FabricOptions{.evidence_policy = EvidencePolicy::RequireFresh});
  SLF_EXPECT(base.ok());
  const ControllerIncarnation observer = mint_incarnation();

  // Rebuild with evidence observed by a previous incarnation.
  FabricBuilder builder(FabricId{1}, base.value().name, base.value().generation, Epoch{1},
                        base.value().options);
  for (const auto& leaf : base.value().leaves) {
    SLF_EXPECT(builder.add_leaf(leaf).ok());
  }
  for (const auto& spine : base.value().spines) {
    SLF_EXPECT(builder.add_spine(spine).ok());
  }
  for (const auto& port : base.value().ports) {
    SLF_EXPECT(builder.add_port(port).ok());
  }
  for (const auto& link : base.value().links) {
    SLF_EXPECT(builder.add_link(link).ok());
  }
  for (const auto& evidence : observe_all_links(base.value(), observer, Epoch{1}, LinkObservation::Up)) {
    SLF_EXPECT(builder.add_evidence(evidence).ok());
  }
  const auto observed = builder.freeze();
  SLF_EXPECT(observed.ok());
  const auto index = TopologyIndex::build(observed.value());
  SLF_EXPECT(index.ok());

  // Same incarnation: the evidence is fresh and the path is eligible.
  EligibilityEngine fresh(index.value(), observer);
  const auto constraints = PathConstraints::defaults_for(observed.value().options);
  const auto fresh_assessment = fresh.assess_leaf_pair(LeafId{1}, LeafId{2}, constraints);
  SLF_EXPECT(fresh_assessment.ok());
  SLF_EXPECT_EQ(fresh_assessment.value().verdict, EligibilityVerdict::Eligible);
  SLF_EXPECT(fresh_assessment.value().evidence_required);

  // A different incarnation (a restart): every observation is historical, and
  // the honest answer is indeterminate - never eligible.
  EligibilityEngine restarted(index.value(), mint_incarnation());
  const auto restarted_assessment = restarted.assess_leaf_pair(LeafId{1}, LeafId{2}, constraints);
  SLF_EXPECT(restarted_assessment.ok());
  SLF_EXPECT_EQ(restarted_assessment.value().verdict, EligibilityVerdict::Indeterminate);
  SLF_EXPECT_EQ(restarted_assessment.value().code, StatusCode::Incomplete);
  SLF_EXPECT_EQ(restarted_assessment.value().eligible_spines.size(), 0U);

  // With no observer at all, the same conservative answer applies.
  EligibilityEngine anonymous(index.value());
  const auto anonymous_assessment = anonymous.assess_leaf_pair(LeafId{1}, LeafId{2}, constraints);
  SLF_EXPECT(anonymous_assessment.ok());
  SLF_EXPECT_EQ(anonymous_assessment.value().verdict, EligibilityVerdict::Indeterminate);

  // Config-only policy ignores evidence entirely.
  const auto config_only = three_spine_fabric();
  SLF_EXPECT(config_only.ok());
  const auto config_index = TopologyIndex::build(config_only.value());
  SLF_EXPECT(config_index.ok());
  EligibilityEngine plain(config_index.value(), observer);
  const auto plain_assessment =
      plain.assess_leaf_pair(LeafId{1}, LeafId{2}, PathConstraints::defaults_for(config_only.value().options));
  SLF_EXPECT(plain_assessment.ok());
  SLF_EXPECT_EQ(plain_assessment.value().verdict, EligibilityVerdict::Eligible);
}

SLF_TEST(eligibility_evidence_observations_are_respected) {
  const auto base = three_spine_fabric(FabricOptions{.evidence_policy = EvidencePolicy::RequireFresh});
  SLF_EXPECT(base.ok());
  const ControllerIncarnation observer = mint_incarnation();
  FabricBuilder builder(FabricId{1}, base.value().name, base.value().generation, Epoch{1},
                        base.value().options);
  for (const auto& leaf : base.value().leaves) {
    SLF_EXPECT(builder.add_leaf(leaf).ok());
  }
  for (const auto& spine : base.value().spines) {
    SLF_EXPECT(builder.add_spine(spine).ok());
  }
  for (const auto& port : base.value().ports) {
    SLF_EXPECT(builder.add_port(port).ok());
  }
  for (const auto& link : base.value().links) {
    SLF_EXPECT(builder.add_link(link).ok());
  }
  std::uint64_t sequence = 1;
  for (const auto& link : base.value().links) {
    LinkEvidence evidence;
    evidence.link = link.id;
    evidence.generation = base.value().generation;
    evidence.observer = observer;
    evidence.epoch = Epoch{1};
    evidence.observed_seq = SequenceNumber{sequence++};
    // Spines 2 and 3 are observed up on both sides. Both links of spine 1 are
    // deliberately left unobserved, so spine 1 is undecided rather than down.
    if (link.id == LinkId{1} || link.id == LinkId{4}) {
      continue;
    }
    evidence.observed = LinkObservation::Up;
    SLF_EXPECT(builder.add_evidence(evidence).ok());
  }
  const auto observed = builder.freeze();
  SLF_EXPECT(observed.ok());
  const auto index = TopologyIndex::build(observed.value());
  SLF_EXPECT(index.ok());
  EligibilityEngine engine(index.value(), observer);
  const auto constraints = PathConstraints::defaults_for(observed.value().options);
  const auto assessment = engine.assess_leaf_pair(LeafId{1}, LeafId{2}, constraints);
  SLF_EXPECT(assessment.ok());
  // Spines 2 and 3 are observed up on both sides; spine 1 is unusable.
  SLF_EXPECT_EQ(assessment.value().eligible_spines.size(), 2U);
  SLF_EXPECT_EQ(assessment.value().eligible_spines[0], SpineId{2});
  SLF_EXPECT_EQ(assessment.value().code, StatusCode::Incomplete);
  // Some spines are undecided (missing or down evidence), so the verdict is
  // indeterminate even though the constraint is satisfied: this runtime does
  // not turn a partially observed fabric into a clean success.
  SLF_EXPECT_EQ(assessment.value().verdict, EligibilityVerdict::Indeterminate);
}

SLF_TEST(eligibility_health_classification) {
  const auto fabric = three_spine_fabric();
  SLF_EXPECT(fabric.ok());
  const auto index = TopologyIndex::build(fabric.value());
  SLF_EXPECT(index.ok());
  EligibilityEngine engine(index.value());
  const auto constraints = PathConstraints::defaults_for(fabric.value().options);
  const auto report = engine.health(constraints, ControllerIncarnation{}, Epoch{1});
  SLF_EXPECT(report.ok());
  SLF_EXPECT_EQ(report.value().health, FabricHealth::Operational);
  SLF_EXPECT_EQ(report.value().leaves_total, 2U);
  SLF_EXPECT_EQ(report.value().leaves_serving, 2U);
  SLF_EXPECT_EQ(report.value().spines_serving, 3U);
  SLF_EXPECT_EQ(report.value().topology_digest, fabric.value().topology_digest);
  SLF_EXPECT(report.value().to_string().find("operational") != std::string::npos);

  // Unformed fabric: nothing has been formed yet.
  FabricBuilder empty(FabricId{1}, "empty", TopologyGeneration{1}, Epoch{1});
  const auto unformed = empty.freeze();
  SLF_EXPECT(unformed.ok());
  const auto empty_index = TopologyIndex::build(unformed.value());
  SLF_EXPECT(empty_index.ok());
  EligibilityEngine empty_engine(empty_index.value());
  const auto empty_report = empty_engine.health(constraints, ControllerIncarnation{}, Epoch{1});
  SLF_EXPECT(empty_report.ok());
  SLF_EXPECT_EQ(empty_report.value().health, FabricHealth::Unformed);
}

SLF_TEST(eligibility_cancellation_is_distinct) {
  const auto fabric = three_spine_fabric();
  SLF_EXPECT(fabric.ok());
  const auto index = TopologyIndex::build(fabric.value());
  SLF_EXPECT(index.ok());
  EligibilityEngine engine(index.value());
  const auto constraints = PathConstraints::defaults_for(fabric.value().options);
  CancellationToken cancelled = CancellationToken::pre_cancelled();
  SLF_EXPECT_CODE(engine.eligible_spines_for_leaf(LeafId{1}, constraints, cancelled),
                  StatusCode::Cancelled);
  SLF_EXPECT_CODE(engine.assess_leaf_pair(LeafId{1}, LeafId{2}, constraints, cancelled),
                  StatusCode::Cancelled);
  SLF_EXPECT_CODE(engine.health(constraints, ControllerIncarnation{}, Epoch{1}, cancelled),
                  StatusCode::Cancelled);
  CancellationToken live;
  SLF_EXPECT(engine.eligible_spines_for_leaf(LeafId{1}, constraints, live).ok());
}
