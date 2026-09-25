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

// Seeded property tests over generated fabrics. Every property is a statement
// that must hold for arbitrary input, not for one hand-written example, and the
// seed is printed with any failure so the exact case can be re-run.

#include <algorithm>
#include <set>

#include "fixtures.hpp"
#include "slf/eligibility.hpp"
#include "slf/topology.hpp"
#include "slf_test.hpp"

using namespace slf;
using namespace slf::test;

namespace {

Outcome<Fabric> generate(Rng& rng, FabricOptions options = {}) {
  FabricBuilder builder(FabricId{1}, "property", TopologyGeneration{1}, Epoch{1}, options);
  const std::uint32_t leaf_count = 2 + static_cast<std::uint32_t>(rng.below(5));
  const std::uint32_t spine_count = 1 + static_cast<std::uint32_t>(rng.below(5));
  for (std::uint32_t i = 1; i <= leaf_count; ++i) {
    LeafSpec spec;
    spec.id = i;
    spec.domain = 1 + static_cast<std::uint32_t>(rng.below(4));
    spec.access_mbps = 50000 * (1 + rng.below(8));
    const Status status = builder.add_leaf(leaf_record(spec, TopologyGeneration{1}));
    if (!status.ok()) {
      return status;
    }
  }
  for (std::uint32_t i = 1; i <= spine_count; ++i) {
    SpineSpec spec;
    spec.id = i;
    spec.domain = 1 + static_cast<std::uint32_t>(rng.below(4));
    spec.fabric_mbps = 200000 * (1 + rng.below(8));
    const Status status = builder.add_spine(spine_record(spec, TopologyGeneration{1}));
    if (!status.ok()) {
      return status;
    }
  }
  std::uint32_t port_id = 1;
  std::uint32_t link_id = 1;
  for (std::uint32_t leaf = 1; leaf <= leaf_count; ++leaf) {
    for (std::uint32_t spine = 1; spine <= spine_count; ++spine) {
      if (!rng.chance(70)) {
        continue;
      }
      PortRecord leaf_port;
      leaf_port.id = PortId{port_id++};
      leaf_port.owner = NodeKey{LeafId{leaf}};
      leaf_port.speed_mbps = 10000 * (1 + rng.below(8));
      leaf_port.generation = TopologyGeneration{1};
      PortRecord spine_port;
      spine_port.id = PortId{port_id++};
      spine_port.owner = NodeKey{SpineId{spine}};
      spine_port.speed_mbps = 10000 * (1 + rng.below(8));
      spine_port.generation = TopologyGeneration{1};
      LinkRecord link;
      link.id = LinkId{link_id++};
      link.a = NodeKey{LeafId{leaf}};
      link.port_a = leaf_port.id;
      link.b = NodeKey{SpineId{spine}};
      link.port_b = spine_port.id;
      link.cls = LinkClass::LeafSpine;
      link.generation = TopologyGeneration{1};
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

SLF_TEST(property_digest_is_a_function_of_content) {
  Rng rng(slf_ctx.seed);
  for (std::uint32_t round = 0; round < 30; ++round) {
    const auto first = generate(rng);
    SLF_EXPECT(first.ok());
    if (!first.ok()) {
      return;
    }
    const auto second = generate(rng);
    SLF_EXPECT(second.ok());
    if (!second.ok()) {
      return;
    }
    // Same content implies the same digest; different content implies a
    // different digest (collision resistance is not claimed, but the encoder
    // must not ignore a field).
    SLF_EXPECT_EQ(canonical_topology_bytes(first.value()) == canonical_topology_bytes(second.value()),
                  first.value().topology_digest == second.value().topology_digest);
    const auto reloaded = deserialize_fabric(
        std::span<const std::byte>(serialize_fabric(first.value())), BuilderLimits{});
    SLF_EXPECT(reloaded.ok());
    if (reloaded.ok()) {
      SLF_EXPECT_EQ(reloaded.value().topology_digest, first.value().topology_digest);
      SLF_EXPECT_EQ(reloaded.value().evidence_digest, first.value().evidence_digest);
    }
  }
}

SLF_TEST(property_eligibility_invariants) {
  Rng rng(slf_ctx.seed ^ 0x1234ULL);
  std::uint32_t evaluated = 0;
  for (std::uint32_t round = 0; round < 40; ++round) {
    FabricOptions options;
    options.require_domain_diversity = rng.chance(50);
    const auto fabric = generate(rng, options);
    SLF_EXPECT(fabric.ok());
    if (!fabric.ok()) {
      return;
    }
    const auto index = TopologyIndex::build(fabric.value());
    SLF_EXPECT(index.ok());
    if (!index.ok()) {
      return;
    }
    EligibilityEngine engine(index.value());
    const auto constraints = PathConstraints::defaults_for(fabric.value().options);
    for (const auto& leaf : fabric.value().leaves) {
      const auto spines = engine.eligible_spines_for_leaf(leaf.id, constraints);
      SLF_EXPECT(spines.ok());
      if (!spines.ok()) {
        return;
      }
      std::set<SpineId> seen;
      for (const auto& entry : spines.value()) {
        ++evaluated;
        // Every spine appears at most once.
        SLF_EXPECT(seen.insert(entry.spine).second);
        // An eligible spine must be structurally sound and carry capacity.
        if (entry.verdict == EligibilityVerdict::Eligible) {
          SLF_EXPECT(entry.structural_ok);
          SLF_EXPECT(entry.capacity_mbps > 0U);
          SLF_EXPECT_EQ(entry.code, StatusCode::Ok);
        } else {
          SLF_EXPECT(entry.code != StatusCode::Ok);
        }
        // Freshness and verdict never contradict each other.
        if (entry.freshness == EvidenceFreshness::Absent ||
            entry.freshness == EvidenceFreshness::Historical) {
          SLF_EXPECT(engine.requires_evidence(constraints)
                         ? entry.verdict != EligibilityVerdict::Eligible
                         : true);
        }
      }
      // Evaluation is deterministic: a second pass gives the same answer.
      const auto again = engine.eligible_spines_for_leaf(leaf.id, constraints);
      SLF_EXPECT(again.ok());
      if (again.ok()) {
        SLF_EXPECT_EQ(again.value().size(), spines.value().size());
        for (std::size_t i = 0; i < again.value().size(); ++i) {
          SLF_EXPECT_EQ(again.value()[i].spine, spines.value()[i].spine);
          SLF_EXPECT_EQ(again.value()[i].verdict, spines.value()[i].verdict);
          SLF_EXPECT_EQ(again.value()[i].code, spines.value()[i].code);
        }
      }
    }
  }
  slf_ctx.note("eligible-spine evaluations: " + std::to_string(evaluated));
  SLF_EXPECT(evaluated > 50U);
}

SLF_TEST(property_health_and_verdict_are_monotone_in_structure) {
  Rng rng(slf_ctx.seed ^ 0x9999ULL);
  for (std::uint32_t round = 0; round < 25; ++round) {
    const auto fabric = generate(rng);
    SLF_EXPECT(fabric.ok());
    if (!fabric.ok()) {
      return;
    }
    const auto index = TopologyIndex::build(fabric.value());
    SLF_EXPECT(index.ok());
    if (!index.ok()) {
      return;
    }
    EligibilityEngine engine(index.value());
    const auto constraints = PathConstraints::defaults_for(fabric.value().options);
    const auto health = engine.health(constraints, ControllerIncarnation{}, Epoch{1});
    SLF_EXPECT(health.ok());
    if (!health.ok()) {
      return;
    }
    // Counted leaves partition the leaf population.
    SLF_EXPECT_EQ(health.value().leaves_serving + health.value().leaves_isolated +
                      health.value().leaves_indeterminate,
                  health.value().leaves_total);
    SLF_EXPECT(health.value().spines_serving + health.value().spines_failed +
                   health.value().spines_draining <=
               health.value().spines_total);
    // A fabric with isolated leaves is never reported as fully operational.
    if (health.value().leaves_isolated > 0) {
      SLF_EXPECT(health.value().health != FabricHealth::Operational);
    }
    // Health is deterministic and bound to the fabric digest it was computed at.
    const auto again = engine.health(constraints, ControllerIncarnation{}, Epoch{1});
    SLF_EXPECT(again.ok());
    if (again.ok()) {
      SLF_EXPECT_EQ(again.value().health, health.value().health);
      SLF_EXPECT_EQ(again.value().topology_digest, fabric.value().topology_digest);
    }
  }
}

SLF_TEST(property_permutation_independence_of_queries) {
  Rng rng(slf_ctx.seed ^ 0x5555ULL);
  for (std::uint32_t round = 0; round < 20; ++round) {
    const auto fabric = generate(rng);
    SLF_EXPECT(fabric.ok());
    if (!fabric.ok()) {
      return;
    }
    const auto index = TopologyIndex::build(fabric.value());
    SLF_EXPECT(index.ok());
    if (!index.ok()) {
      return;
    }
    EligibilityEngine engine(index.value());
    const auto constraints = PathConstraints::defaults_for(fabric.value().options);
    for (const auto& leaf : fabric.value().leaves) {
      for (const auto& other : fabric.value().leaves) {
        if (leaf.id == other.id) {
          continue;
        }
        const auto first = engine.assess_leaf_pair(leaf.id, other.id, constraints);
        const auto second = engine.assess_leaf_pair(leaf.id, other.id, constraints);
        SLF_EXPECT(first.ok() && second.ok());
        if (!first.ok() || !second.ok()) {
          return;
        }
        SLF_EXPECT_EQ(first.value().summary(), second.value().summary());
        SLF_EXPECT_EQ(first.value().verdict, second.value().verdict);
        SLF_EXPECT_EQ(first.value().eligible_spines, second.value().eligible_spines);
        // Reachability is symmetric in class (the class describes the shape).
        const auto reverse = engine.assess_leaf_pair(other.id, leaf.id, constraints);
        SLF_EXPECT(reverse.ok());
        if (reverse.ok()) {
          SLF_EXPECT_EQ(reverse.value().cls, first.value().cls);
          SLF_EXPECT_EQ(reverse.value().eligible_spines, first.value().eligible_spines);
          SLF_EXPECT_EQ(reverse.value().verdict, first.value().verdict);
        }
      }
    }
  }
}
