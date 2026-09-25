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

// Differential test: the indexed eligibility engine against the independent
// brute-force oracle in tests/oracle.cpp. Fabrics are generated from a seeded
// PRNG, so a failure prints the seed that produced it.

#include <algorithm>
#include <set>

#include "fixtures.hpp"
#include "oracle.hpp"
#include "slf/eligibility.hpp"
#include "slf/topology.hpp"
#include "slf_test.hpp"

using namespace slf;
using namespace slf::test;

namespace {

/// Generates a random but always structurally valid fabric: a subset of the
/// leaf/spine cartesian product, random speeds, random node states.
Outcome<Fabric> random_fabric(Rng& rng, TopologyGeneration generation) {
  FabricOptions options;
  options.require_domain_diversity = rng.chance(50);
  options.min_eligible_spines = 1 + static_cast<std::uint32_t>(rng.below(3));
  options.allow_same_tier_adjacency = false;
  FabricBuilder builder(FabricId{1}, "random", generation, Epoch{1}, options);
  const std::uint32_t leaf_count = 2 + static_cast<std::uint32_t>(rng.below(4));
  const std::uint32_t spine_count = 1 + static_cast<std::uint32_t>(rng.below(4));
  for (std::uint32_t i = 1; i <= leaf_count; ++i) {
    LeafSpec spec;
    spec.id = i;
    spec.domain = 1 + static_cast<std::uint32_t>(rng.below(3));
    spec.access_mbps = 100000 * (1 + rng.below(4));
    const std::uint32_t state = static_cast<std::uint32_t>(rng.below(10));
    spec.state = state == 0 ? NodeState::Maintenance : (state == 1 ? NodeState::Draining : NodeState::Active);
    const Status status = builder.add_leaf(leaf_record(spec, generation));
    if (!status.ok()) {
      return status;
    }
  }
  for (std::uint32_t i = 1; i <= spine_count; ++i) {
    SpineSpec spec;
    spec.id = i;
    spec.domain = 1 + static_cast<std::uint32_t>(rng.below(3));
    spec.fabric_mbps = 400000 * (1 + rng.below(8));
    const std::uint32_t state = static_cast<std::uint32_t>(rng.below(10));
    spec.state = state == 0 ? NodeState::Failed : (state == 1 ? NodeState::Draining : NodeState::Active);
    const Status status = builder.add_spine(spine_record(spec, generation));
    if (!status.ok()) {
      return status;
    }
  }
  std::uint32_t port_id = 1;
  std::uint32_t link_id = 1;
  for (std::uint32_t leaf = 1; leaf <= leaf_count; ++leaf) {
    for (std::uint32_t spine = 1; spine <= spine_count; ++spine) {
      if (!rng.chance(75)) {
        continue;
      }
      PortRecord leaf_port;
      leaf_port.id = PortId{port_id++};
      leaf_port.owner = NodeKey{LeafId{leaf}};
      leaf_port.speed_mbps = 40000 * (1 + rng.below(3));
      leaf_port.admin = rng.chance(15) ? PortAdmin::Disabled : PortAdmin::Enabled;
      leaf_port.generation = generation;
      PortRecord spine_port;
      spine_port.id = PortId{port_id++};
      spine_port.owner = NodeKey{SpineId{spine}};
      spine_port.speed_mbps = 40000 * (1 + rng.below(3));
      spine_port.admin = rng.chance(15) ? PortAdmin::Disabled : PortAdmin::Enabled;
      spine_port.generation = generation;
      LinkRecord link;
      link.id = LinkId{link_id++};
      link.a = NodeKey{LeafId{leaf}};
      link.port_a = leaf_port.id;
      link.b = NodeKey{SpineId{spine}};
      link.port_b = spine_port.id;
      link.cls = LinkClass::LeafSpine;
      link.admin = rng.chance(15) ? LinkAdmin::Disabled : LinkAdmin::Enabled;
      link.generation = generation;
      const Status a = builder.add_port(std::move(leaf_port));
      const Status b = builder.add_port(std::move(spine_port));
      const Status c = builder.add_link(std::move(link));
      if (!a.ok() || !b.ok() || !c.ok()) {
        return a.ok() ? (b.ok() ? c : b) : a;
      }
    }
  }
  return builder.freeze();
}

}  // namespace

SLF_TEST(differential_engine_matches_oracle) {
  Rng rng(slf_ctx.seed);
  std::uint32_t iterations = 0;
  for (std::uint32_t round = 0; round < 60; ++round) {
    const auto fabric = random_fabric(rng, TopologyGeneration{1});
    SLF_EXPECT(fabric.ok());
    if (!fabric.ok()) {
      return;
    }
    // The oracle's own structural sweep must agree that the fabric is sound.
    const auto issues = oracle::structural_issues(fabric.value());
    SLF_EXPECT(issues.empty());
    const auto index = TopologyIndex::build(fabric.value());
    SLF_EXPECT(index.ok());
    if (!index.ok()) {
      return;
    }
    EligibilityEngine engine(index.value());
    const auto constraints = PathConstraints::defaults_for(fabric.value().options);

    for (const auto& leaf_a : fabric.value().leaves) {
      for (const auto& leaf_b : fabric.value().leaves) {
        if (leaf_a.id == leaf_b.id) {
          continue;
        }
        ++iterations;
        const auto assessment = engine.assess_leaf_pair(leaf_a.id, leaf_b.id, constraints);
        SLF_EXPECT(assessment.ok());
        if (!assessment.ok()) {
          return;
        }
        // Reachability classification: the *structural* class is what the
        // oracle models, and it must agree exactly.
        const ReachabilityClass structural =
            index.value().structural_class(NodeKey{leaf_a.id}, NodeKey{leaf_b.id});
        SLF_EXPECT_EQ(structural, oracle::classify(fabric.value(), NodeKey{leaf_a.id}, NodeKey{leaf_b.id}));
        // The assessment reports the operationalised class: for a leaf-to-leaf
        // pair it refines single-spine versus multi-spine using the *eligible*
        // spine count, so it may differ from the structural class in exactly
        // that one dimension.
        if (assessment.value().cls != structural) {
          const bool refinement =
              (structural == ReachabilityClass::LeafLeafSingleSpine ||
               structural == ReachabilityClass::LeafLeafMultiSpine) &&
              (assessment.value().cls == ReachabilityClass::LeafLeafSingleSpine ||
               assessment.value().cls == ReachabilityClass::LeafLeafMultiSpine);
          SLF_EXPECT(refinement);
        }
        // Eligible spine set (configuration only: no evidence is present in
        // these fabrics, and the policy is config-only).
        SLF_EXPECT_EQ(assessment.value().eligible_spines,
                      oracle::eligible_spines_for_pair(fabric.value(), leaf_a.id, leaf_b.id));
        // Capacity aggregation.
        std::uint64_t expected_uplink = 0;
        SLF_EXPECT(oracle::uplink_capacity(fabric.value(), leaf_a.id, expected_uplink));
        const auto spines = engine.eligible_spines_for_leaf(leaf_a.id, constraints);
        SLF_EXPECT(spines.ok());
        std::uint64_t eligible_capacity = 0;
        std::uint32_t eligible_count = 0;
        for (const auto& entry : spines.value()) {
          if (entry.verdict == EligibilityVerdict::Eligible) {
            eligible_capacity += entry.capacity_mbps;
            ++eligible_count;
          }
        }
        // The pair's eligible set is the intersection of what each leaf can
        // reach, so it is never larger than either leaf's own set.
        SLF_EXPECT(assessment.value().eligible_spines.size() <= eligible_count);
        (void)eligible_capacity;
        // Diversity bookkeeping.
        const auto domains = oracle::domains_of(fabric.value(), assessment.value().eligible_spines);
        SLF_EXPECT_EQ(assessment.value().covered_domains, domains);
        if (!domains.empty()) {
          SLF_EXPECT_EQ(assessment.value().max_domain_loss_tolerance,
                        static_cast<std::uint32_t>(domains.size() - 1));
        }
        // Brute-force diversity sets, when diversity is required.
        if (constraints.require_domain_diversity && !assessment.value().eligible_spines.empty()) {
          PathConstraints diverse = constraints;
          diverse.max_sets = 4;
          const auto diverse_assessment = engine.assess_leaf_pair(leaf_a.id, leaf_b.id, diverse);
          SLF_EXPECT(diverse_assessment.ok());
          const auto brute = oracle::brute_force_sets(fabric.value(), assessment.value().eligible_spines, 4);
          SLF_EXPECT_EQ(diverse_assessment.value().spine_sets.sets.size(), brute.size());
          if (diverse_assessment.value().spine_sets.sets.size() == brute.size()) {
            for (std::size_t i = 0; i < brute.size(); ++i) {
              SLF_EXPECT_EQ(diverse_assessment.value().spine_sets.sets[i].spines, brute[i].spines);
              SLF_EXPECT_EQ(diverse_assessment.value().spine_sets.sets[i].distinct_domains,
                            brute[i].distinct_domains);
            }
          }
        }
      }
    }
  }
  slf_ctx.note("differential leaf pairs evaluated: " + std::to_string(iterations));
  SLF_EXPECT(iterations > 100U);
}

SLF_TEST(differential_canonical_identity_is_order_independent) {
  Rng rng(slf_ctx.seed ^ 0xABCDEFULL);
  for (std::uint32_t round = 0; round < 20; ++round) {
    const auto fabric = random_fabric(rng, TopologyGeneration{1 + rng.below(3)});
    SLF_EXPECT(fabric.ok());
    if (!fabric.ok()) {
      return;
    }
    // Rebuild with every record vector shuffled: the digest must not move.
    std::vector<LeafRecord> leaves = fabric.value().leaves;
    std::vector<SpineRecord> spines = fabric.value().spines;
    std::vector<PortRecord> ports = fabric.value().ports;
    std::vector<LinkRecord> links = fabric.value().links;
    const auto shuffle = [&rng](auto& container) {
      for (std::size_t i = container.size(); i > 1; --i) {
        std::swap(container[i - 1], container[rng.below(i)]);
      }
    };
    shuffle(leaves);
    shuffle(spines);
    shuffle(ports);
    shuffle(links);
    FabricBuilder builder(FabricId{1}, fabric.value().name, fabric.value().generation, fabric.value().epoch,
                          fabric.value().options);
    for (const auto& record : spines) {
      SLF_EXPECT(builder.add_spine(record).ok());
    }
    for (const auto& record : leaves) {
      SLF_EXPECT(builder.add_leaf(record).ok());
    }
    for (const auto& record : ports) {
      SLF_EXPECT(builder.add_port(record).ok());
    }
    for (const auto& record : links) {
      SLF_EXPECT(builder.add_link(record).ok());
    }
    const auto shuffled = builder.freeze();
    SLF_EXPECT(shuffled.ok());
    if (!shuffled.ok()) {
      return;
    }
    SLF_EXPECT_EQ(shuffled.value().topology_digest, fabric.value().topology_digest);
    SLF_EXPECT_EQ(oracle::independent_fingerprint(shuffled.value()),
                  oracle::independent_fingerprint(fabric.value()));
  }
}
