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
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <utility>

#include "slf/checked.hpp"

namespace slf {
namespace {

/// Hard bound on the number of maximal-diversity combinations examined before
/// the engine switches to a greedy selection. Keeps a hostile fabric from
/// turning a query into a combinatorial explosion.
constexpr std::uint64_t kMaxExaminedCombinations = 4096;

[[nodiscard]] bool set_is_better(const EligibleSpineSet& candidate, const EligibleSpineSet& incumbent) {
  if (candidate.total_capacity_mbps != incumbent.total_capacity_mbps) {
    return candidate.total_capacity_mbps > incumbent.total_capacity_mbps;
  }
  return candidate.spines < incumbent.spines;
}

[[nodiscard]] std::string describe_spine(const SpineEligibility& entry) {
  std::ostringstream out;
  out << to_string(entry.spine) << " domain=" << entry.domain.value << " capacity_mbps="
      << entry.capacity_mbps << " verdict=" << slf::to_string(entry.verdict)
      << " code=" << slf::to_string(entry.code) << " evidence=" << slf::to_string(entry.freshness);
  if (!entry.detail.empty()) {
    out << " (" << entry.detail << ")";
  }
  return out.str();
}

}  // namespace

std::string_view to_string(EligibilityVerdict verdict) noexcept {
  switch (verdict) {
    case EligibilityVerdict::Eligible: return "eligible";
    case EligibilityVerdict::Ineligible: return "ineligible";
    case EligibilityVerdict::Indeterminate: return "indeterminate";
  }
  return "invalid";
}

PathConstraints PathConstraints::defaults_for(const FabricOptions& options) noexcept {
  PathConstraints constraints;
  constraints.require_domain_diversity = options.require_domain_diversity;
  constraints.min_eligible_spines = options.min_eligible_spines;
  constraints.require_evidence = options.evidence_policy == EvidencePolicy::RequireFresh;
  return constraints;
}

bool EligibilityEngine::requires_evidence(const PathConstraints& constraints) const noexcept {
  return constraints.require_evidence ||
         index_->fabric().options.evidence_policy == EvidencePolicy::RequireFresh;
}

Outcome<SpineEligibility> EligibilityEngine::evaluate_uplink(LeafId leaf, const Adjacency& uplink,
                                                             const PathConstraints& constraints) const {
  const Fabric& fabric = index_->fabric();
  SpineEligibility result;
  result.spine = SpineId{uplink.peer.index};
  result.capacity_mbps = uplink.speed_mbps;
  result.structural_ok = true;
  result.verdict = EligibilityVerdict::Eligible;
  result.code = StatusCode::Ok;

  const SpineRecord* spine = fabric.find_spine(result.spine);
  if (spine == nullptr) {
    result.structural_ok = false;
    result.verdict = EligibilityVerdict::Ineligible;
    result.code = StatusCode::DanglingEdge;
    result.detail = "spine does not exist in this generation";
    return result;
  }
  result.domain = spine->domain;

  const LeafRecord* leaf_record = fabric.find_leaf(leaf);
  if (leaf_record == nullptr) {
    result.structural_ok = false;
    result.verdict = EligibilityVerdict::Ineligible;
    result.code = StatusCode::NotFound;
    result.detail = "leaf does not exist in this generation";
    return result;
  }
  if (!state_is_serving(leaf_record->state)) {
    result.verdict = EligibilityVerdict::Ineligible;
    result.code = StatusCode::Refused;
    result.detail = std::string("leaf is in state ") + std::string(to_string(leaf_record->state));
    return result;
  }
  if (spine->state == NodeState::Draining) {
    result.verdict = EligibilityVerdict::Ineligible;
    result.code = StatusCode::Refused;
    result.detail = "spine is draining";
    return result;
  }
  if (!state_is_serving(spine->state)) {
    result.verdict = EligibilityVerdict::Ineligible;
    result.code = StatusCode::Refused;
    result.detail = std::string("spine is in state ") + std::string(to_string(spine->state));
    return result;
  }
  const PortRecord* local_port = fabric.find_port(uplink.local_port);
  const PortRecord* peer_port = fabric.find_port(uplink.peer_port);
  if (local_port == nullptr || peer_port == nullptr) {
    result.structural_ok = false;
    result.verdict = EligibilityVerdict::Ineligible;
    result.code = StatusCode::DanglingEdge;
    result.detail = "link endpoint port does not exist";
    return result;
  }
  if (local_port->admin != PortAdmin::Enabled || peer_port->admin != PortAdmin::Enabled) {
    result.verdict = EligibilityVerdict::Ineligible;
    result.code = StatusCode::Refused;
    result.detail = "an endpoint port is administratively disabled";
    return result;
  }

  const bool evidence_required = requires_evidence(constraints);
  const auto evidence = fabric.evidence_for(uplink.link);
  if (!evidence.ok()) {
    result.freshness = EvidenceFreshness::Absent;
    if (evidence.code() == StatusCode::Conflicting) {
      result.verdict = EligibilityVerdict::Indeterminate;
      result.code = StatusCode::Conflicting;
      result.detail = std::string(evidence.status().detail());
      return result;
    }
    if (evidence_required) {
      result.verdict = EligibilityVerdict::Indeterminate;
      result.code = StatusCode::Incomplete;
      result.detail = "no observed evidence for this link";
      return result;
    }
    return result;
  }
  const LinkEvidence& record = *evidence.value();
  result.freshness = fabric.evidence_freshness(record, controller_, fabric.epoch);
  if (result.freshness != EvidenceFreshness::Fresh) {
    if (evidence_required) {
      result.verdict = EligibilityVerdict::Indeterminate;
      result.code = StatusCode::Stale;
      result.detail = "observed evidence was produced by an earlier incarnation, epoch, or generation";
    }
    return result;
  }
  switch (record.observed) {
    case LinkObservation::Up:
      return result;
    case LinkObservation::Down:
      result.verdict = EligibilityVerdict::Ineligible;
      result.code = StatusCode::Refused;
      result.detail = "link is observed down";
      return result;
    case LinkObservation::Unknown:
      result.verdict = EligibilityVerdict::Indeterminate;
      result.code = StatusCode::Indeterminate;
      result.detail = "the observer could not determine the link state";
      return result;
  }
  return result;
}

Outcome<std::vector<SpineEligibility>> EligibilityEngine::evaluate_uplinks(LeafId leaf,
                                                                          const PathConstraints& constraints,
                                                                          CancellationToken token) const {
  std::vector<SpineEligibility> out;
  const auto uplinks = index_->uplinks(leaf);
  out.reserve(uplinks.size());
  std::uint32_t processed = 0;
  for (const auto& uplink : uplinks) {
    if ((processed & 0x3FU) == 0U && token.cancelled()) {
      return Status(StatusCode::Cancelled, "eligibility evaluation was cancelled");
    }
    ++processed;
    if (uplink.peer.tier != Tier::Spine) {
      continue;
    }
    Outcome<SpineEligibility> entry = evaluate_uplink(leaf, uplink, constraints);
    if (!entry.ok()) {
      return entry.status();
    }
    out.push_back(std::move(entry.value()));
  }
  return out;
}

Outcome<std::vector<SpineEligibility>> EligibilityEngine::eligible_spines_for_leaf(
    LeafId leaf, const PathConstraints& constraints, CancellationToken token) const {
  if (index_->fabric().find_leaf(leaf) == nullptr) {
    return Status(StatusCode::NotFound, "leaf " + to_string(leaf) + " does not exist");
  }
  return evaluate_uplinks(leaf, constraints, token);
}

EligibleSpineSets EligibilityEngine::select_spine_sets(const std::vector<SpineEligibility>& eligible,
                                                       const PathConstraints& constraints,
                                                       CancellationToken token) const {
  EligibleSpineSets out;
  std::vector<const SpineEligibility*> pool;
  pool.reserve(eligible.size());
  for (const auto& entry : eligible) {
    if (entry.verdict == EligibilityVerdict::Eligible) {
      pool.push_back(&entry);
    }
  }
  if (pool.empty()) {
    return out;
  }
  if (!constraints.require_domain_diversity) {
    EligibleSpineSet set;
    for (const auto* entry : pool) {
      set.spines.push_back(entry->spine);
      set.total_capacity_mbps += entry->capacity_mbps;
    }
    std::sort(set.spines.begin(), set.spines.end());
    std::set<FailureDomainId> domains;
    for (const auto* entry : pool) {
      domains.insert(entry->domain);
      if (std::find(set.domains.begin(), set.domains.end(), entry->domain) == set.domains.end()) {
        set.domains.push_back(entry->domain);
      }
    }
    std::sort(set.domains.begin(), set.domains.end());
    set.distinct_domains = static_cast<std::uint32_t>(domains.size());
    out.candidate_combinations = 1;
    out.sets.push_back(std::move(set));
    return out;
  }

  std::map<FailureDomainId, std::vector<const SpineEligibility*>> by_domain;
  for (const auto* entry : pool) {
    by_domain[entry->domain].push_back(entry);
  }
  std::uint64_t combinations = 1;
  bool overflow = false;
  for (const auto& [domain, entries] : by_domain) {
    (void)domain;
    std::uint64_t next = 0;
    if (!checked_mul(combinations, entries.size(), next)) {
      overflow = true;
      combinations = UINT64_MAX;
      break;
    }
    combinations = next;
  }
  out.candidate_combinations = combinations;

  std::vector<EligibleSpineSet> candidates;
  if (!overflow && combinations <= kMaxExaminedCombinations) {
    std::vector<const SpineEligibility*> current;
    const std::function<void(std::map<FailureDomainId, std::vector<const SpineEligibility*>>::const_iterator)>
        enumerate = [&](std::map<FailureDomainId, std::vector<const SpineEligibility*>>::const_iterator it) {
          if (token.cancelled()) {
            return;
          }
          if (it == by_domain.end()) {
            EligibleSpineSet set;
            for (const auto* entry : current) {
              set.spines.push_back(entry->spine);
              set.total_capacity_mbps += entry->capacity_mbps;
              set.domains.push_back(entry->domain);
            }
            std::sort(set.spines.begin(), set.spines.end());
            std::sort(set.domains.begin(), set.domains.end());
            set.distinct_domains = static_cast<std::uint32_t>(set.domains.size());
            candidates.push_back(std::move(set));
            return;
          }
          const auto next = std::next(it);
          for (const auto* entry : it->second) {
            current.push_back(entry);
            enumerate(next);
            current.pop_back();
          }
        };
    enumerate(by_domain.begin());
  } else {
    out.truncated = true;
    EligibleSpineSet greedy;
    for (const auto& [domain, entries] : by_domain) {
      const SpineEligibility* best = nullptr;
      for (const auto* entry : entries) {
        if (best == nullptr || entry->capacity_mbps > best->capacity_mbps ||
            (entry->capacity_mbps == best->capacity_mbps && entry->spine < best->spine)) {
          best = entry;
        }
      }
      if (best != nullptr) {
        greedy.spines.push_back(best->spine);
        greedy.total_capacity_mbps += best->capacity_mbps;
        greedy.domains.push_back(domain);
      }
    }
    std::sort(greedy.spines.begin(), greedy.spines.end());
    std::sort(greedy.domains.begin(), greedy.domains.end());
    greedy.distinct_domains = static_cast<std::uint32_t>(greedy.domains.size());
    candidates.push_back(std::move(greedy));
  }

  if (token.cancelled()) {
    out.sets.clear();
    out.truncated = true;
    return out;
  }
  std::sort(candidates.begin(), candidates.end(), set_is_better);
  const std::size_t limit = constraints.max_sets == 0 ? 1U : constraints.max_sets;
  if (candidates.size() > limit) {
    out.truncated = true;
  }
  for (std::size_t i = 0; i < candidates.size() && i < limit; ++i) {
    out.sets.push_back(std::move(candidates[i]));
  }
  return out;
}

Outcome<PathAssessment> EligibilityEngine::assess_leaf_pair(LeafId from, LeafId to,
                                                            const PathConstraints& constraints,
                                                            CancellationToken token) const {
  const Fabric& fabric = index_->fabric();
  if (fabric.find_leaf(from) == nullptr) {
    return Status(StatusCode::NotFound, "leaf " + to_string(from) + " does not exist");
  }
  if (fabric.find_leaf(to) == nullptr) {
    return Status(StatusCode::NotFound, "leaf " + to_string(to) + " does not exist");
  }
  PathAssessment assessment;
  assessment.from = NodeKey{from};
  assessment.to = NodeKey{to};
  assessment.generation = fabric.generation;
  assessment.epoch = fabric.epoch;
  assessment.controller = controller_;
  assessment.topology_digest = fabric.topology_digest;
  assessment.evidence_required = requires_evidence(constraints);
  assessment.cls = index_->structural_class(NodeKey{from}, NodeKey{to});

  if (from == to) {
    assessment.verdict = EligibilityVerdict::Ineligible;
    assessment.code = StatusCode::Invalid;
    assessment.explanation.push_back("a node is not a path to itself");
    return assessment;
  }

  if (assessment.cls == ReachabilityClass::DirectLeafSpine ||
      assessment.cls == ReachabilityClass::LeafSpineViaPeer ||
      assessment.cls == ReachabilityClass::SpineSpineViaLeaf) {
    assessment.verdict = EligibilityVerdict::Indeterminate;
    assessment.code = StatusCode::Unsupported;
    assessment.explanation.push_back(
        "operational eligibility of a peer-assisted path is outside the modelled scope; use "
        "assess_leaf_to_spine for direct leaf-to-spine questions");
    return assessment;
  }

  if (assessment.cls == ReachabilityClass::Unreachable) {
    assessment.verdict = EligibilityVerdict::Ineligible;
    assessment.code = StatusCode::NotFound;
    assessment.explanation.push_back("no structural path between the endpoints in this generation");
    return assessment;
  }

  // Direct peer link between the two leaves.
  if (assessment.cls == ReachabilityClass::DirectPeer) {
    if (!constraints.allow_peer_paths) {
      assessment.verdict = EligibilityVerdict::Ineligible;
      assessment.code = StatusCode::Refused;
      assessment.explanation.push_back("peer paths are excluded by the query constraints");
      return assessment;
    }
    const auto adjacency = index_->adjacency_between(NodeKey{from}, NodeKey{to});
    const PortRecord* local = fabric.find_port(adjacency->local_port);
    const PortRecord* peer = fabric.find_port(adjacency->peer_port);
    if (local == nullptr || peer == nullptr || local->admin != PortAdmin::Enabled ||
        peer->admin != PortAdmin::Enabled) {
      assessment.verdict = EligibilityVerdict::Ineligible;
      assessment.code = StatusCode::Refused;
      assessment.explanation.push_back("peer link endpoint port is administratively disabled");
      return assessment;
    }
    const auto evidence = fabric.evidence_for(adjacency->link);
    if (assessment.evidence_required) {
      if (!evidence.ok()) {
        assessment.verdict = EligibilityVerdict::Indeterminate;
        assessment.code = evidence.code();
        assessment.explanation.push_back("peer link has no usable evidence: " +
                                         std::string(evidence.status().detail()));
        return assessment;
      }
      const auto freshness = fabric.evidence_freshness(*evidence.value(), controller_, fabric.epoch);
      if (freshness != EvidenceFreshness::Fresh) {
        assessment.verdict = EligibilityVerdict::Indeterminate;
        assessment.code = StatusCode::Stale;
        assessment.explanation.push_back("peer link evidence is historical");
        return assessment;
      }
      if (evidence.value()->observed != LinkObservation::Up) {
        assessment.verdict = EligibilityVerdict::Ineligible;
        assessment.code = evidence.value()->observed == LinkObservation::Down ? StatusCode::Refused
                                                                             : StatusCode::Indeterminate;
        assessment.explanation.push_back("peer link is not observed up");
        return assessment;
      }
    }
    assessment.verdict = EligibilityVerdict::Eligible;
    assessment.code = StatusCode::Ok;
    assessment.capacity.access_capacity_mbps = 0;
    assessment.capacity.code = StatusCode::Ok;
    (void)capacity_add_downlink(assessment.capacity, adjacency->speed_mbps);
    (void)capacity_add_uplink(assessment.capacity, adjacency->speed_mbps);
    capacity_finalize(assessment.capacity);
    assessment.explanation.push_back("direct peer link " + to_string(adjacency->link) + " at " +
                                     std::to_string(adjacency->speed_mbps) + " Mbps");
    return assessment;
  }

  // Single-spine and multi-spine leaf-to-leaf paths.
  const auto from_entry = evaluate_uplinks(from, constraints, token);
  if (!from_entry.ok()) {
    return from_entry.status();
  }
  const auto to_entry = evaluate_uplinks(to, constraints, token);
  if (!to_entry.ok()) {
    return to_entry.status();
  }
  std::map<SpineId, const SpineEligibility*> from_map;
  for (const auto& entry : from_entry.value()) {
    from_map[entry.spine] = &entry;
  }
  std::vector<SpineEligibility> pair_eligible;
  std::uint32_t indeterminate_count = 0;
  std::uint32_t ineligible_count = 0;
  for (const auto& entry : to_entry.value()) {
    const auto it = from_map.find(entry.spine);
    if (it == from_map.end()) {
      continue;
    }
    ++assessment.links_examined;
    const SpineEligibility& other = *it->second;
    SpineEligibility combined = other;
    if (other.verdict == EligibilityVerdict::Eligible && entry.verdict == EligibilityVerdict::Eligible) {
      combined.capacity_mbps = std::min(other.capacity_mbps, entry.capacity_mbps);
      combined.detail = "eligible on both sides";
      pair_eligible.push_back(std::move(combined));
      continue;
    }
    if (other.verdict == EligibilityVerdict::Eligible) {
      combined = entry;
    }
    if (combined.verdict == EligibilityVerdict::Indeterminate) {
      ++indeterminate_count;
    } else {
      ++ineligible_count;
    }
    if (assessment.explanation.size() < constraints.max_explanations) {
      assessment.explanation.push_back("spine " + describe_spine(combined) + " excluded");
    }
  }

  assessment.capacity.access_capacity_mbps = 0;
  const LeafRecord* from_record = fabric.find_leaf(from);
  const LeafRecord* to_record = fabric.find_leaf(to);
  if (from_record != nullptr && to_record != nullptr) {
    std::uint64_t access = 0;
    if (checked_add(from_record->access_capacity_mbps, to_record->access_capacity_mbps, access)) {
      assessment.capacity.access_capacity_mbps = access;
    } else {
      assessment.capacity.code = StatusCode::Overflow;
      assessment.capacity.detail = "access capacity aggregate overflowed";
    }
  }
  std::set<FailureDomainId> domains;
  for (const auto& entry : pair_eligible) {
    assessment.eligible_spines.push_back(entry.spine);
    domains.insert(entry.domain);
    (void)capacity_add_uplink(assessment.capacity, entry.capacity_mbps);
    const SpineRecord* spine = fabric.find_spine(entry.spine);
    if (spine != nullptr) {
      std::uint64_t next = 0;
      if (checked_add(assessment.capacity.spine_capacity_mbps, spine->fabric_capacity_mbps, next)) {
        assessment.capacity.spine_capacity_mbps = next;
      } else {
        assessment.capacity.code = StatusCode::Overflow;
      }
    }
  }
  std::sort(assessment.eligible_spines.begin(), assessment.eligible_spines.end());
  assessment.capacity.spine_count = static_cast<std::uint32_t>(assessment.eligible_spines.size());
  capacity_finalize(assessment.capacity);
  for (const auto domain : domains) {
    assessment.covered_domains.push_back(domain);
  }
  assessment.max_domain_loss_tolerance =
      assessment.covered_domains.empty() ? 0 : static_cast<std::uint32_t>(assessment.covered_domains.size() - 1);
  assessment.spine_sets = select_spine_sets(pair_eligible, constraints, token);

  const std::uint32_t required = constraints.min_eligible_spines == 0 ? 1U : constraints.min_eligible_spines;
  const auto eligible_count = static_cast<std::uint32_t>(assessment.eligible_spines.size());
  bool constraints_met = eligible_count >= required;
  if (constraints.require_domain_diversity && assessment.covered_domains.size() < required) {
    constraints_met = false;
  }
  if (constraints.min_total_capacity_mbps != 0 &&
      assessment.capacity.uplink_capacity_mbps < constraints.min_total_capacity_mbps) {
    constraints_met = false;
  }
  if (constraints.max_oversubscription.has_value() && assessment.capacity.uplink_count > 0) {
    const auto ordering = Rational::compare(assessment.capacity.oversubscription,
                                            *constraints.max_oversubscription);
    if (!ordering.ok()) {
      assessment.verdict = EligibilityVerdict::Indeterminate;
      assessment.code = ordering.code();
      assessment.explanation.push_back("oversubscription comparison overflowed");
      return assessment;
    }
    if (ordering.value() > 0) {
      constraints_met = false;
    }
  }

  if (assessment.cls == ReachabilityClass::LeafLeafSingleSpine ||
      assessment.cls == ReachabilityClass::LeafLeafMultiSpine) {
    if (eligible_count >= 2U) {
      assessment.cls = ReachabilityClass::LeafLeafMultiSpine;
    } else if (eligible_count == 1U) {
      assessment.cls = ReachabilityClass::LeafLeafSingleSpine;
    }
  }

  if (!constraints_met && eligible_count == 0) {
    if (indeterminate_count > 0 && ineligible_count == 0) {
      assessment.verdict = EligibilityVerdict::Indeterminate;
      assessment.code = StatusCode::Incomplete;
      assessment.explanation.push_back("every shared spine is undecided: required evidence is missing or stale");
    } else if (indeterminate_count > 0) {
      assessment.verdict = EligibilityVerdict::Indeterminate;
      assessment.code = StatusCode::Conflicting;
      assessment.explanation.push_back("no shared spine is eligible, and some are undecided");
    } else {
      assessment.verdict = EligibilityVerdict::Ineligible;
      assessment.code = StatusCode::Refused;
      assessment.explanation.push_back("no shared spine satisfies the constraints");
    }
    return assessment;
  }
  if (!constraints_met) {
    assessment.verdict = EligibilityVerdict::Ineligible;
    assessment.code = StatusCode::Refused;
    assessment.explanation.push_back("the eligible spine set does not satisfy the query constraints");
    return assessment;
  }
  if (indeterminate_count > 0) {
    assessment.verdict = EligibilityVerdict::Indeterminate;
    assessment.code = StatusCode::Incomplete;
    assessment.explanation.push_back("eligible spines satisfy the constraints, but some shared spines are "
                                     "undecided because evidence is missing or stale");
    return assessment;
  }
  assessment.verdict = EligibilityVerdict::Eligible;
  assessment.code = StatusCode::Ok;
  assessment.explanation.push_back("generation=" + std::to_string(fabric.generation.value) + " digest=" +
                                   fabric.topology_digest.short_hex(8) + " epoch=" +
                                   std::to_string(fabric.epoch.value));
  for (const auto& entry : pair_eligible) {
    if (assessment.explanation.size() >= constraints.max_explanations) {
      break;
    }
    assessment.explanation.push_back("spine " + describe_spine(entry));
  }
  return assessment;
}

Outcome<PathAssessment> EligibilityEngine::assess_leaf_to_spine(LeafId from, SpineId to,
                                                                const PathConstraints& constraints) const {
  const Fabric& fabric = index_->fabric();
  const LeafRecord* leaf = fabric.find_leaf(from);
  if (leaf == nullptr) {
    return Status(StatusCode::NotFound, "leaf " + to_string(from) + " does not exist");
  }
  const SpineRecord* spine = fabric.find_spine(to);
  if (spine == nullptr) {
    return Status(StatusCode::NotFound, "spine " + to_string(to) + " does not exist");
  }
  PathAssessment assessment;
  assessment.from = NodeKey{from};
  assessment.to = NodeKey{to};
  assessment.generation = fabric.generation;
  assessment.epoch = fabric.epoch;
  assessment.controller = controller_;
  assessment.topology_digest = fabric.topology_digest;
  assessment.evidence_required = requires_evidence(constraints);
  assessment.cls = index_->structural_class(NodeKey{from}, NodeKey{to});
  assessment.capacity.access_capacity_mbps = leaf->access_capacity_mbps;

  if (assessment.cls != ReachabilityClass::DirectLeafSpine) {
    if (assessment.cls == ReachabilityClass::Unreachable) {
      assessment.verdict = EligibilityVerdict::Ineligible;
      assessment.code = StatusCode::NotFound;
      assessment.explanation.push_back("leaf and spine are not adjacent in this generation");
      return assessment;
    }
    assessment.verdict = EligibilityVerdict::Indeterminate;
    assessment.code = StatusCode::Unsupported;
    assessment.explanation.push_back("only direct leaf-to-spine adjacency is evaluated for eligibility");
    return assessment;
  }
  for (const auto& uplink : index_->uplinks(from)) {
    if (uplink.peer != NodeKey{to}) {
      continue;
    }
    ++assessment.links_examined;
    const auto entry = evaluate_uplink(from, uplink, constraints);
    if (!entry.ok()) {
      return entry.status();
    }
    const SpineEligibility& result = entry.value();
    assessment.eligible_spines.push_back(result.spine);
    assessment.covered_domains.push_back(result.domain);
    assessment.verdict = result.verdict;
    assessment.code = result.code;
    if (result.verdict == EligibilityVerdict::Eligible) {
      (void)capacity_add_uplink(assessment.capacity, result.capacity_mbps);
      (void)capacity_add_downlink(assessment.capacity, result.capacity_mbps);
      std::uint64_t next = 0;
      if (checked_add(assessment.capacity.spine_capacity_mbps, spine->fabric_capacity_mbps, next)) {
        assessment.capacity.spine_capacity_mbps = next;
      }
      assessment.capacity.spine_count = 1;
      assessment.spine_sets = select_spine_sets({result}, constraints, CancellationToken{});
    }
    capacity_finalize(assessment.capacity);
    if (assessment.explanation.size() < constraints.max_explanations) {
      assessment.explanation.push_back("spine " + describe_spine(result));
    }
    return assessment;
  }
  assessment.verdict = EligibilityVerdict::Ineligible;
  assessment.code = StatusCode::NotFound;
  assessment.explanation.push_back("the structural index contains no adjacency for this pair");
  return assessment;
}

Outcome<FabricHealthReport> EligibilityEngine::health(const PathConstraints& constraints,
                                                      ControllerIncarnation controller, Epoch epoch,
                                                      CancellationToken token) const {
  const Fabric& fabric = index_->fabric();
  FabricHealthReport report;
  report.generation = fabric.generation;
  report.epoch = epoch;
  report.controller = controller;
  report.topology_digest = fabric.topology_digest;
  report.leaves_total = static_cast<std::uint32_t>(fabric.leaves.size());
  report.spines_total = static_cast<std::uint32_t>(fabric.spines.size());
  for (const auto& link : fabric.links) {
    if (link.admin == LinkAdmin::Disabled) {
      ++report.disabled_link_count;
    }
  }

  if (fabric.state == FabricState::Unformed || fabric.leaves.empty()) {
    report.health = FabricHealth::Unformed;
    report.summary = "no topology has been formed";
    return report;
  }
  if (fabric.state == FabricState::Retired) {
    report.health = FabricHealth::Retired;
    report.summary = "fabric is retired";
    return report;
  }
  if (fabric.state == FabricState::Draining) {
    report.health = FabricHealth::Draining;
    report.summary = "fabric is administratively draining";
    return report;
  }

  for (const auto& spine : fabric.spines) {
    if (spine.state == NodeState::Failed) {
      ++report.spines_failed;
    } else if (spine.state == NodeState::Draining) {
      ++report.spines_draining;
    } else if (state_is_serving(spine.state)) {
      ++report.spines_serving;
    }
  }

  const std::uint32_t required =
      constraints.min_eligible_spines == 0 ? 1U : constraints.min_eligible_spines;
  for (const auto& leaf : fabric.leaves) {
    if ((report.leaves_serving + report.leaves_isolated + report.leaves_indeterminate) % 16U == 0U &&
        token.cancelled()) {
      return Status(StatusCode::Cancelled, "health evaluation was cancelled");
    }
    const auto entries = evaluate_uplinks(leaf.id, constraints, token);
    if (!entries.ok()) {
      return entries.status();
    }
    std::uint32_t eligible = 0;
    std::uint32_t indeterminate = 0;
    for (const auto& entry : entries.value()) {
      if (entry.verdict == EligibilityVerdict::Eligible) {
        ++eligible;
      } else if (entry.verdict == EligibilityVerdict::Indeterminate) {
        ++indeterminate;
      }
    }
    const bool serving = state_is_serving(leaf.state) && eligible >= required;
    if (serving) {
      ++report.leaves_serving;
      continue;
    }
    if (!state_is_serving(leaf.state)) {
      ++report.leaves_isolated;
      if (report.reasons.size() < 16U) {
        report.reasons.push_back(to_string(leaf.id) + " is in state " + std::string(to_string(leaf.state)));
      }
      continue;
    }
    if (eligible == 0 && indeterminate > 0) {
      ++report.leaves_indeterminate;
      if (report.reasons.size() < 16U) {
        report.reasons.push_back(to_string(leaf.id) +
                                 " has no decided uplink: required evidence is missing or stale");
      }
      continue;
    }
    ++report.leaves_isolated;
    if (report.reasons.size() < 16U) {
      report.reasons.push_back(to_string(leaf.id) + " has " + std::to_string(eligible) +
                               " eligible spine(s), fewer than the required " + std::to_string(required));
    }
  }

  if (report.leaves_isolated > 0 || report.spines_failed > 0 || report.spines_draining > 0) {
    report.health = FabricHealth::Degraded;
    report.code = StatusCode::Ok;
    report.summary = "fabric is degraded: " + std::to_string(report.leaves_isolated) +
                     " isolated leaf/leaves, " + std::to_string(report.spines_failed) +
                     " failed spine(s), " + std::to_string(report.spines_draining) + " draining spine(s)";
    return report;
  }
  if (report.leaves_indeterminate > 0) {
    report.health = FabricHealth::Indeterminate;
    report.code = StatusCode::Indeterminate;
    report.summary = "fabric health cannot be decided: " + std::to_string(report.leaves_indeterminate) +
                     " leaf/leaves depend on evidence that is missing or stale";
    return report;
  }
  report.health = FabricHealth::Operational;
  report.code = StatusCode::Ok;
  report.summary = "fabric is operational: all " + std::to_string(report.leaves_serving) +
                   " leaves have at least " + std::to_string(required) + " eligible spine(s)";
  return report;
}

std::string PathAssessment::summary() const {
  std::ostringstream out;
  out << slf::to_string(from) << " -> " << slf::to_string(to) << ": " << slf::to_string(verdict) << " ("
      << slf::to_string(code) << ") class=" << slf::to_string(cls) << " spines="
      << eligible_spines.size() << " domains=" << covered_domains.size()
      << " diversity_tolerance=" << max_domain_loss_tolerance;
  return out.str();
}

std::string PathAssessment::to_string() const {
  std::ostringstream out;
  out << summary() << "\n";
  out << "  generation=" << generation.value << " epoch=" << epoch.value
      << " digest=" << topology_digest.short_hex(16) << " controller=" << slf::to_string(controller)
      << " evidence_required=" << (evidence_required ? "true" : "false") << "\n";
  out << "  capacity: " << capacity.to_string() << "\n";
  out << "  eligible spines:";
  for (const auto spine : eligible_spines) {
    out << " " << slf::to_string(spine);
  }
  out << "\n";
  out << "  failure domains:";
  for (const auto domain : covered_domains) {
    out << " " << domain.value;
  }
  out << "\n";
  for (const auto& set : spine_sets.sets) {
    out << "  spine set (capacity=" << set.total_capacity_mbps
        << " domains=" << set.distinct_domains << "):";
    for (const auto spine : set.spines) {
      out << " " << slf::to_string(spine);
    }
    out << "\n";
  }
  if (spine_sets.truncated) {
    out << "  spine set enumeration was bounded (candidate combinations="
        << spine_sets.candidate_combinations << ")\n";
  }
  for (const auto& line : explanation) {
    out << "  - " << line << "\n";
  }
  return out.str();
}

std::string FabricHealthReport::to_string() const {
  std::ostringstream out;
  out << slf::to_string(health) << " (" << slf::to_string(code) << "): " << summary << "\n";
  out << "  generation=" << generation.value << " epoch=" << epoch.value
      << " digest=" << topology_digest.short_hex(16) << "\n";
  out << "  leaves: total=" << leaves_total << " serving=" << leaves_serving
      << " isolated=" << leaves_isolated << " indeterminate=" << leaves_indeterminate << "\n";
  out << "  spines: total=" << spines_total << " serving=" << spines_serving
      << " failed=" << spines_failed << " draining=" << spines_draining
      << " disabled_links=" << disabled_link_count << "\n";
  for (const auto& reason : reasons) {
    out << "  - " << reason << "\n";
  }
  return out.str();
}

}  // namespace slf
