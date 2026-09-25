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

// Independent reference model ("oracle") used by the differential tests.
//
// The oracle deliberately shares no data structure with the implementation:
//   * adjacency is a plain std::multimap rebuilt from the record vectors,
//   * reachability is a breadth-first search with an explicit queue,
//   * eligible spine sets are found by brute force over subsets,
//   * capacity is summed with an explicit overflow check.
//
// It is slower and simpler than the indexed engine. Agreement between the two is
// evidence that the indexed implementation is not losing or inventing structure;
// disagreement is a defect in one of them.

#include <cstdint>
#include <string>
#include <vector>

#include "slf/eligibility.hpp"
#include "slf/model.hpp"
#include "slf/topology.hpp"

namespace slf::oracle {

struct StructuralIssue {
  std::string where;
  std::string detail;
};

/// Independent structural validation: dangling endpoints, duplicate port use,
/// tier/class contradictions, same-tier policy violations, generation drift.
[[nodiscard]] std::vector<StructuralIssue> structural_issues(const Fabric& fabric);

/// BFS reachability classification over enabled links.
[[nodiscard]] ReachabilityClass classify(const Fabric& fabric, NodeKey from, NodeKey to);

/// Configuration-only eligibility of one leaf-to-spine link (no evidence).
[[nodiscard]] bool link_eligible_config_only(const Fabric& fabric, LeafId leaf, SpineId spine);

/// Eligible spines for a leaf pair, configuration only, computed by scanning
/// every spine in the fabric.
[[nodiscard]] std::vector<SpineId> eligible_spines_for_pair(const Fabric& fabric, LeafId from, LeafId to);

/// Naive uplink capacity summation with an explicit overflow check.
[[nodiscard]] bool uplink_capacity(const Fabric& fabric, LeafId leaf, std::uint64_t& out);

/// Distinct failure domains of a spine set.
[[nodiscard]] std::vector<FailureDomainId> domains_of(const Fabric& fabric,
                                                      const std::vector<SpineId>& spines);

/// Brute-force maximum-cardinality diversity-respecting spine sets.
[[nodiscard]] std::vector<EligibleSpineSet> brute_force_sets(const Fabric& fabric,
                                                             const std::vector<SpineId>& eligible,
                                                             std::uint32_t max_sets);

/// Digest of the fabric computed by an independent sorted textual rendering, so
/// a mismatch points at the canonical encoder rather than at the hash function.
[[nodiscard]] std::string independent_fingerprint(const Fabric& fabric);

}  // namespace slf::oracle
