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

#include "oracle.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <sstream>

namespace slf::oracle {
namespace {

struct Edge {
  NodeKey peer;
  LinkId link;
  std::uint64_t speed_mbps{0};
  LinkClass cls{LinkClass::LeafSpine};
};

[[nodiscard]] std::multimap<NodeKey, Edge> build_adjacency(const Fabric& fabric,
                                                          std::vector<StructuralIssue>& issues) {
  std::multimap<NodeKey, Edge> adjacency;
  std::map<PortId, std::uint32_t> port_use;
  std::set<std::pair<NodeKey, NodeKey>> seen_pairs;
  for (const auto& link : fabric.links) {
    const auto speed = fabric.effective_link_speed_mbps(link);
    if (!speed.has_value()) {
      issues.push_back({to_string(link.id), "effective speed cannot be resolved"});
      continue;
    }
    const auto exists = [&fabric](NodeKey key) {
      return key.tier == Tier::Leaf ? fabric.find_leaf(LeafId{key.index}) != nullptr
                                    : fabric.find_spine(SpineId{key.index}) != nullptr;
    };
    if (!exists(link.a) || !exists(link.b)) {
      issues.push_back({to_string(link.id), "endpoint does not exist"});
      continue;
    }
    const PortRecord* port_a = fabric.find_port(link.port_a);
    const PortRecord* port_b = fabric.find_port(link.port_b);
    if (port_a == nullptr || port_b == nullptr) {
      issues.push_back({to_string(link.id), "endpoint port does not exist"});
      continue;
    }
    if (port_a->owner != link.a || port_b->owner != link.b) {
      issues.push_back({to_string(link.id), "port owner does not match the link endpoint"});
      continue;
    }
    if (link.a == link.b) {
      issues.push_back({to_string(link.id), "self loop"});
      continue;
    }
    const bool same_tier = link.a.tier == link.b.tier;
    if (same_tier && !fabric.options.allow_same_tier_adjacency) {
      issues.push_back({to_string(link.id), "same-tier link while policy forbids it"});
      continue;
    }
    if (same_tier && link.cls != LinkClass::SameTierPeer) {
      issues.push_back({to_string(link.id), "same-tier link is not classified as a peer link"});
      continue;
    }
    if (!same_tier && link.cls != LinkClass::LeafSpine) {
      issues.push_back({to_string(link.id), "leaf-to-spine link is not classified as leaf_spine"});
      continue;
    }
    if (link.declared_speed_mbps != 0 &&
        link.declared_speed_mbps > std::min(port_a->speed_mbps, port_b->speed_mbps)) {
      issues.push_back({to_string(link.id), "declared speed exceeds the slower port"});
      continue;
    }
    const auto pair = std::minmax(link.a, link.b);
    if (!seen_pairs.insert({pair.first, pair.second}).second) {
      issues.push_back({to_string(link.id), "duplicate adjacency between the same endpoints"});
    }
    ++port_use[link.port_a];
    ++port_use[link.port_b];
    if (link.admin != LinkAdmin::Enabled) {
      continue;
    }
    adjacency.insert({link.a, Edge{link.b, link.id, *speed, link.cls}});
    adjacency.insert({link.b, Edge{link.a, link.id, *speed, link.cls}});
  }
  for (const auto& [port, count] : port_use) {
    if (count > 1) {
      issues.push_back({to_string(port), "port terminates more than one link"});
    }
  }
  return adjacency;
}

[[nodiscard]] std::vector<Edge> edges_of(const std::multimap<NodeKey, Edge>& adjacency, NodeKey key) {
  std::vector<Edge> out;
  for (auto it = adjacency.lower_bound(key); it != adjacency.upper_bound(key); ++it) {
    out.push_back(it->second);
  }
  return out;
}

}  // namespace

std::vector<StructuralIssue> structural_issues(const Fabric& fabric) {
  std::vector<StructuralIssue> issues;
  (void)build_adjacency(fabric, issues);
  for (const auto& link : fabric.links) {
    if (link.generation != fabric.generation) {
      issues.push_back({to_string(link.id), "link generation differs from the fabric generation"});
    }
  }
  for (const auto& leaf : fabric.leaves) {
    if (leaf.generation != fabric.generation) {
      issues.push_back({to_string(leaf.id), "leaf generation differs from the fabric generation"});
    }
  }
  for (const auto& spine : fabric.spines) {
    if (spine.generation != fabric.generation) {
      issues.push_back({to_string(spine.id), "spine generation differs from the fabric generation"});
    }
  }
  return issues;
}

ReachabilityClass classify(const Fabric& fabric, NodeKey from, NodeKey to) {
  if (from == to) {
    return ReachabilityClass::SameNode;
  }
  std::vector<StructuralIssue> issues;
  const auto adjacency = build_adjacency(fabric, issues);
  for (const auto& edge : edges_of(adjacency, from)) {
    if (edge.peer == to) {
      return edge.cls == LinkClass::LeafSpine ? ReachabilityClass::DirectLeafSpine
                                              : ReachabilityClass::DirectPeer;
    }
  }
  // Direct rules, evaluated by brute force over the edge list rather than
  // through any index. A same-tier adjacent pair is a peer link only when the
  // link is classified that way; anything else adjacent is a leaf-to-spine link.
  const auto adjacent_class = [&adjacency](NodeKey a, NodeKey b) -> std::optional<LinkClass> {
    for (const auto& edge : edges_of(adjacency, a)) {
      if (edge.peer == b) {
        return edge.cls;
      }
    }
    return std::nullopt;
  };
  const auto has_peer_bridge = [&](NodeKey a, NodeKey b) {
    for (const auto& edge : edges_of(adjacency, a)) {
      if (edge.cls != LinkClass::SameTierPeer) {
        continue;
      }
      if (adjacent_class(edge.peer, b).has_value()) {
        return true;
      }
    }
    return false;
  };
  if (from.tier == Tier::Leaf && to.tier == Tier::Leaf) {
    std::uint32_t common = 0;
    for (const auto& spine : fabric.spines) {
      if (adjacent_class(from, NodeKey{spine.id}).has_value() &&
          adjacent_class(to, NodeKey{spine.id}).has_value()) {
        ++common;
      }
    }
    if (common >= 2) {
      return ReachabilityClass::LeafLeafMultiSpine;
    }
    if (common == 1) {
      return ReachabilityClass::LeafLeafSingleSpine;
    }
    if (has_peer_bridge(from, to) || has_peer_bridge(to, from)) {
      return ReachabilityClass::LeafSpineViaPeer;
    }
    return ReachabilityClass::Unreachable;
  }
  if (from.tier == Tier::Spine && to.tier == Tier::Spine) {
    for (const auto& leaf : fabric.leaves) {
      if (adjacent_class(from, NodeKey{leaf.id}).has_value() &&
          adjacent_class(to, NodeKey{leaf.id}).has_value()) {
        return ReachabilityClass::SpineSpineViaLeaf;
      }
    }
    return ReachabilityClass::Unreachable;
  }
  if (has_peer_bridge(from, to)) {
    return ReachabilityClass::LeafSpineViaPeer;
  }
  return ReachabilityClass::Unreachable;
}

bool link_eligible_config_only(const Fabric& fabric, LeafId leaf, SpineId spine) {
  const LeafRecord* leaf_record = fabric.find_leaf(leaf);
  const SpineRecord* spine_record = fabric.find_spine(spine);
  if (leaf_record == nullptr || spine_record == nullptr) {
    return false;
  }
  if (!state_is_serving(leaf_record->state) || !state_is_serving(spine_record->state)) {
    return false;
  }
  for (const auto& link : fabric.links) {
    const bool forward = link.a == NodeKey{leaf} && link.b == NodeKey{spine};
    const bool backward = link.a == NodeKey{spine} && link.b == NodeKey{leaf};
    if (!forward && !backward) {
      continue;
    }
    if (link.admin != LinkAdmin::Enabled) {
      continue;
    }
    const PortRecord* port_a = fabric.find_port(link.port_a);
    const PortRecord* port_b = fabric.find_port(link.port_b);
    if (port_a == nullptr || port_b == nullptr) {
      continue;
    }
    if (port_a->admin != PortAdmin::Enabled || port_b->admin != PortAdmin::Enabled) {
      continue;
    }
    if (fabric.effective_link_speed_mbps(link).value_or(0) == 0) {
      continue;
    }
    return true;
  }
  return false;
}

std::vector<SpineId> eligible_spines_for_pair(const Fabric& fabric, LeafId from, LeafId to) {
  std::vector<SpineId> out;
  for (const auto& spine : fabric.spines) {
    if (link_eligible_config_only(fabric, from, spine.id) &&
        link_eligible_config_only(fabric, to, spine.id)) {
      out.push_back(spine.id);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool uplink_capacity(const Fabric& fabric, LeafId leaf, std::uint64_t& out) {
  out = 0;
  for (const auto& link : fabric.links) {
    if (link.a != NodeKey{leaf} && link.b != NodeKey{leaf}) {
      continue;
    }
    if (link.cls != LinkClass::LeafSpine) {
      continue;
    }
    if (link.admin != LinkAdmin::Enabled) {
      continue;
    }
    const auto speed = fabric.effective_link_speed_mbps(link);
    if (!speed.has_value()) {
      return false;
    }
    if (*speed > UINT64_MAX - out) {
      return false;
    }
    out += *speed;
  }
  return true;
}

std::vector<FailureDomainId> domains_of(const Fabric& fabric, const std::vector<SpineId>& spines) {
  std::set<FailureDomainId> unique;
  for (const auto spine : spines) {
    const SpineRecord* record = fabric.find_spine(spine);
    if (record != nullptr) {
      unique.insert(record->domain);
    }
  }
  return std::vector<FailureDomainId>(unique.begin(), unique.end());
}

std::vector<EligibleSpineSet> brute_force_sets(const Fabric& fabric, const std::vector<SpineId>& eligible,
                                               std::uint32_t max_sets) {
  std::vector<EligibleSpineSet> candidates;
  if (eligible.empty() || eligible.size() > 18U) {
    return candidates;
  }
  const std::size_t total = static_cast<std::size_t>(1) << eligible.size();
  std::uint32_t best_cardinality = 0;
  for (std::size_t mask = 1; mask < total; ++mask) {
    std::vector<SpineId> subset;
    std::set<FailureDomainId> domains;
    bool distinct = true;
    for (std::size_t bit = 0; bit < eligible.size(); ++bit) {
      if ((mask & (static_cast<std::size_t>(1) << bit)) == 0U) {
        continue;
      }
      const SpineRecord* record = fabric.find_spine(eligible[bit]);
      if (record == nullptr) {
        distinct = false;
        break;
      }
      if (!domains.insert(record->domain).second) {
        distinct = false;
        break;
      }
      subset.push_back(eligible[bit]);
    }
    if (!distinct || subset.empty()) {
      continue;
    }
    if (subset.size() < best_cardinality) {
      continue;
    }
    if (subset.size() > best_cardinality) {
      best_cardinality = static_cast<std::uint32_t>(subset.size());
      candidates.clear();
    }
    EligibleSpineSet set;
    set.spines = subset;
    set.domains.assign(domains.begin(), domains.end());
    set.distinct_domains = static_cast<std::uint32_t>(domains.size());
    for (const auto spine : subset) {
      const auto pair_links = fabric.links;
      std::uint64_t link_capacity = 0;
      for (const auto& link : pair_links) {
        if (link.a != NodeKey{spine} || link.cls != LinkClass::LeafSpine) {
          continue;
        }
        link_capacity += fabric.effective_link_speed_mbps(link).value_or(0);
      }
      set.total_capacity_mbps += link_capacity;
    }
    candidates.push_back(std::move(set));
  }
  std::sort(candidates.begin(), candidates.end(), [](const EligibleSpineSet& a, const EligibleSpineSet& b) {
    if (a.total_capacity_mbps != b.total_capacity_mbps) {
      return a.total_capacity_mbps > b.total_capacity_mbps;
    }
    return a.spines < b.spines;
  });
  if (candidates.size() > max_sets) {
    candidates.resize(max_sets);
  }
  return candidates;
}

std::string independent_fingerprint(const Fabric& fabric) {
  std::ostringstream out;
  out << "fabric:" << fabric.id.value << ":" << fabric.name << ":" << fabric.site << ":"
      << fabric.generation.value << ":" << fabric.epoch.value << ":"
      << static_cast<int>(fabric.state) << ";";
  std::vector<std::string> domain_lines;
  for (const auto& domain : fabric.domains) {
    domain_lines.push_back("d" + std::to_string(domain.id.value) + "=" + domain.name);
  }
  std::vector<std::string> leaf_lines;
  for (const auto& leaf : fabric.leaves) {
    leaf_lines.push_back("l" + std::to_string(leaf.id.value) + "=" + leaf.name + "," +
                         std::to_string(static_cast<int>(leaf.role)) + "," +
                         std::to_string(leaf.role_incarnation.value) + "," +
                         std::to_string(leaf.domain.value) + "," +
                         std::to_string(static_cast<int>(leaf.state)) + "," +
                         std::to_string(leaf.access_capacity_mbps));
  }
  std::vector<std::string> spine_lines;
  for (const auto& spine : fabric.spines) {
    spine_lines.push_back("s" + std::to_string(spine.id.value) + "=" + spine.name + "," +
                          std::to_string(static_cast<int>(spine.role)) + "," +
                          std::to_string(spine.role_incarnation.value) + "," +
                          std::to_string(spine.domain.value) + "," +
                          std::to_string(static_cast<int>(spine.state)) + "," +
                          std::to_string(spine.fabric_capacity_mbps));
  }
  std::vector<std::string> link_lines;
  for (const auto& link : fabric.links) {
    link_lines.push_back("k" + std::to_string(link.id.value) + "=" + to_string(link.a) + "-" +
                         std::to_string(link.port_a.value) + "-" + to_string(link.b) + "-" +
                         std::to_string(link.port_b.value) + "-" +
                         std::to_string(static_cast<int>(link.cls)) + "-" +
                         std::to_string(static_cast<int>(link.admin)) + "-" +
                         std::to_string(link.declared_speed_mbps));
  }
  std::sort(domain_lines.begin(), domain_lines.end());
  std::sort(leaf_lines.begin(), leaf_lines.end());
  std::sort(spine_lines.begin(), spine_lines.end());
  std::sort(link_lines.begin(), link_lines.end());
  for (const auto& line : domain_lines) {
    out << line << ";";
  }
  for (const auto& line : leaf_lines) {
    out << line << ";";
  }
  for (const auto& line : spine_lines) {
    out << line << ";";
  }
  for (const auto& line : link_lines) {
    out << line << ";";
  }
  out << (fabric.options.allow_same_tier_adjacency ? "1" : "0");
  out << (fabric.options.require_domain_diversity ? "1" : "0");
  out << ":" << fabric.options.min_eligible_spines << ":"
      << static_cast<int>(fabric.options.evidence_policy);
  return out.str();
}

}  // namespace slf::oracle
