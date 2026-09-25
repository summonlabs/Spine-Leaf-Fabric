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

#include "slf/builder.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <sstream>
#include <utility>

namespace slf {
namespace {

enum class NameVerdict {
  Acceptable,
  /// Empty, or not well formed UTF-8.
  Invalid,
  /// Longer than the configured bound.
  TooLong,
};

[[nodiscard]] NameVerdict check_name(std::string_view name, std::uint32_t max_bytes) {
  if (name.size() > max_bytes) {
    return NameVerdict::TooLong;
  }
  if (name.empty() || !is_valid_utf8(name)) {
    return NameVerdict::Invalid;
  }
  return NameVerdict::Acceptable;
}

[[nodiscard]] std::string node_label(NodeKey key) { return to_string(key); }

}  // namespace

FabricBuilder::FabricBuilder(FabricId id, std::string name, TopologyGeneration generation, Epoch epoch,
                             FabricOptions options, BuilderLimits limits)
    : id_(id),
      name_(std::move(name)),
      generation_(generation),
      epoch_(epoch),
      options_(options),
      limits_(limits) {
  switch (check_name(name_, limits_.max_name_bytes)) {
    case NameVerdict::TooLong:
      (void)note(IssueSeverity::Error, StatusCode::Exhausted, "fabric",
                 "fabric name exceeds the configured length bound");
      break;
    case NameVerdict::Invalid:
      (void)note(IssueSeverity::Error, StatusCode::Invalid, "fabric",
                 "fabric name is missing or is not valid UTF-8");
      break;
    case NameVerdict::Acceptable:
      break;
  }
  if (generation_.is_zero()) {
    (void)note(IssueSeverity::Warning, StatusCode::Invalid, "fabric", "topology generation is zero");
  }
  if (options_.min_eligible_spines == 0) {
    (void)note(IssueSeverity::Error, StatusCode::Invalid, "fabric",
               "min_eligible_spines must be at least 1");
  }
}

Status FabricBuilder::note(IssueSeverity severity, StatusCode code, std::string where, std::string detail) {
  ValidationIssue issue;
  issue.severity = severity;
  issue.code = code;
  issue.where = std::move(where);
  issue.detail = std::move(detail);
  if (severity == IssueSeverity::Error) {
    ++report_.error_count;
  } else if (severity == IssueSeverity::Warning) {
    ++report_.warning_count;
  }
  report_.issues.push_back(std::move(issue));
  if (severity == IssueSeverity::Error) {
    return Status(code, report_.issues.back().where + ": " + report_.issues.back().detail);
  }
  return Status::success();
}

Status FabricBuilder::set_site(std::string site) {
  if (frozen_) {
    return Status(StatusCode::Closed, "fabric builder is already frozen");
  }
  if (!site.empty()) {
    switch (check_name(site, limits_.max_name_bytes)) {
      case NameVerdict::TooLong:
        return note(IssueSeverity::Error, StatusCode::Exhausted, "fabric",
                    "site name exceeds the configured length bound");
      case NameVerdict::Invalid:
        return note(IssueSeverity::Error, StatusCode::Invalid, "fabric", "site name is not valid UTF-8");
      case NameVerdict::Acceptable:
        break;
    }
  }
  site_ = std::move(site);
  return Status::success();
}

Status FabricBuilder::set_state(FabricState state) {
  if (frozen_) {
    return Status(StatusCode::Closed, "fabric builder is already frozen");
  }
  fabric_state_ = state;
  return Status::success();
}

Status FabricBuilder::validate_node_common(NodeKey key, std::string_view name, TopologyGeneration generation,
                                            FailureDomainId domain, std::uint64_t capacity_mbps) {
  if (generation != generation_) {
    return note(IssueSeverity::Error, StatusCode::CrossGeneration, node_label(key),
                "record generation " + std::to_string(generation.value) + " does not match fabric generation " +
                    std::to_string(generation_.value));
  }
  switch (check_name(name, limits_.max_name_bytes)) {
    case NameVerdict::TooLong:
      return note(IssueSeverity::Error, StatusCode::Exhausted, node_label(key),
                  "name exceeds the configured length bound");
    case NameVerdict::Invalid:
      return note(IssueSeverity::Error, StatusCode::Invalid, node_label(key),
                  "name is missing or is not valid UTF-8");
    case NameVerdict::Acceptable:
      break;
  }
  if (capacity_mbps > limits_.max_capacity_mbps) {
    return note(IssueSeverity::Error, StatusCode::Overflow, node_label(key),
                "declared capacity exceeds the configured maximum");
  }
  if (domain.is_zero()) {
    return note(IssueSeverity::Warning, StatusCode::Incomplete, node_label(key),
                "node has no failure domain (domain 0)");
  }
  return Status::success();
}

Status FabricBuilder::add_failure_domain(FailureDomainRecord record) {
  if (frozen_) {
    return Status(StatusCode::Closed, "fabric builder is already frozen");
  }
  if (domains_.size() >= limits_.max_failure_domains) {
    return note(IssueSeverity::Error, StatusCode::Exhausted, "domain",
                "failure domain count exceeds the configured bound");
  }
  if (domains_.count(record.id) != 0U) {
    return note(IssueSeverity::Error, StatusCode::DuplicateIdentity, to_string(record.id),
                "failure domain identity is already defined");
  }
  if (record.generation != generation_) {
    return note(IssueSeverity::Error, StatusCode::CrossGeneration, to_string(record.id),
                "failure domain generation does not match the fabric generation");
  }
  switch (check_name(record.name, limits_.max_name_bytes)) {
    case NameVerdict::TooLong:
      return note(IssueSeverity::Error, StatusCode::Exhausted, to_string(record.id),
                  "failure domain name exceeds the configured length bound");
    case NameVerdict::Invalid:
      return note(IssueSeverity::Error, StatusCode::Invalid, to_string(record.id),
                  "failure domain name is missing or is not valid UTF-8");
    case NameVerdict::Acceptable:
      break;
  }
  domains_.emplace(record.id, std::move(record));
  return Status::success();
}

Status FabricBuilder::add_leaf(LeafRecord record) {
  if (frozen_) {
    return Status(StatusCode::Closed, "fabric builder is already frozen");
  }
  if (leaves_.size() >= limits_.max_leaves) {
    return note(IssueSeverity::Error, StatusCode::Exhausted, "leaf",
                "leaf count exceeds the configured bound");
  }
  if (record.id.is_zero()) {
    return note(IssueSeverity::Error, StatusCode::Invalid, to_string(record.id),
                "leaf identity zero is reserved");
  }
  if (leaves_.count(record.id) != 0U) {
    return note(IssueSeverity::Error, StatusCode::DuplicateIdentity, to_string(record.id),
                "leaf identity is already defined");
  }
  for (const auto& [other_id, other] : leaves_) {
    if (other.name == record.name) {
      (void)note(IssueSeverity::Warning, StatusCode::Conflicting, to_string(record.id),
                 "leaf name duplicates " + to_string(other_id));
      break;
    }
  }
  const Status common = validate_node_common(NodeKey{record.id}, record.name, record.generation, record.domain,
                                             record.access_capacity_mbps);
  if (!common.ok()) {
    return common;
  }
  if (record.access_capacity_mbps == 0) {
    (void)note(IssueSeverity::Warning, StatusCode::Incomplete, to_string(record.id),
               "leaf declares zero access capacity");
  }
  leaves_.emplace(record.id, std::move(record));
  return Status::success();
}

Status FabricBuilder::add_spine(SpineRecord record) {
  if (frozen_) {
    return Status(StatusCode::Closed, "fabric builder is already frozen");
  }
  if (spines_.size() >= limits_.max_spines) {
    return note(IssueSeverity::Error, StatusCode::Exhausted, "spine",
                "spine count exceeds the configured bound");
  }
  if (record.id.is_zero()) {
    return note(IssueSeverity::Error, StatusCode::Invalid, to_string(record.id),
                "spine identity zero is reserved");
  }
  if (spines_.count(record.id) != 0U) {
    return note(IssueSeverity::Error, StatusCode::DuplicateIdentity, to_string(record.id),
                "spine identity is already defined");
  }
  for (const auto& [other_id, other] : spines_) {
    if (other.name == record.name) {
      (void)note(IssueSeverity::Warning, StatusCode::Conflicting, to_string(record.id),
                 "spine name duplicates " + to_string(other_id));
      break;
    }
  }
  const Status common = validate_node_common(NodeKey{record.id}, record.name, record.generation, record.domain,
                                             record.fabric_capacity_mbps);
  if (!common.ok()) {
    return common;
  }
  if (record.fabric_capacity_mbps == 0) {
    (void)note(IssueSeverity::Warning, StatusCode::Incomplete, to_string(record.id),
               "spine declares zero fabric capacity");
  }
  spines_.emplace(record.id, std::move(record));
  return Status::success();
}

Status FabricBuilder::add_port(PortRecord record) {
  if (frozen_) {
    return Status(StatusCode::Closed, "fabric builder is already frozen");
  }
  if (ports_.size() >= limits_.max_ports) {
    return note(IssueSeverity::Error, StatusCode::Exhausted, to_string(record.id),
                "port count exceeds the configured bound");
  }
  if (record.id.is_zero()) {
    return note(IssueSeverity::Error, StatusCode::Invalid, to_string(record.id),
                "port identity zero is reserved");
  }
  if (ports_.count(record.id) != 0U) {
    return note(IssueSeverity::Error, StatusCode::DuplicateIdentity, to_string(record.id),
                "port identity is already defined");
  }
  if (record.generation != generation_) {
    return note(IssueSeverity::Error, StatusCode::CrossGeneration, to_string(record.id),
                "port generation does not match the fabric generation");
  }
  bool owner_exists = false;
  if (record.owner.tier == Tier::Leaf) {
    owner_exists = leaves_.count(LeafId{record.owner.index}) != 0U;
  } else {
    owner_exists = spines_.count(SpineId{record.owner.index}) != 0U;
  }
  if (!owner_exists) {
    return note(IssueSeverity::Error, StatusCode::DanglingEdge, to_string(record.id),
                "port owner " + node_label(record.owner) + " does not exist");
  }
  if (record.speed_mbps > limits_.max_capacity_mbps) {
    return note(IssueSeverity::Error, StatusCode::Overflow, to_string(record.id),
                "port speed exceeds the configured maximum");
  }
  if (record.speed_mbps == 0 && record.admin == PortAdmin::Enabled) {
    return note(IssueSeverity::Error, StatusCode::Invalid, to_string(record.id),
                "an enabled port must declare a non-zero speed");
  }
  ports_.emplace(record.id, std::move(record));
  return Status::success();
}

Status FabricBuilder::add_link(LinkRecord record) {
  if (frozen_) {
    return Status(StatusCode::Closed, "fabric builder is already frozen");
  }
  if (links_.size() >= limits_.max_links) {
    return note(IssueSeverity::Error, StatusCode::Exhausted, to_string(record.id),
                "link count exceeds the configured bound");
  }
  if (record.id.is_zero()) {
    return note(IssueSeverity::Error, StatusCode::Invalid, to_string(record.id),
                "link identity zero is reserved");
  }
  if (links_.count(record.id) != 0U) {
    return note(IssueSeverity::Error, StatusCode::DuplicateIdentity, to_string(record.id),
                "link identity is already defined");
  }
  if (record.generation != generation_) {
    return note(IssueSeverity::Error, StatusCode::CrossGeneration, to_string(record.id),
                "link generation does not match the fabric generation");
  }
  if (record.a == record.b) {
    return note(IssueSeverity::Error, StatusCode::IllegalAdjacency, to_string(record.id),
                "a link cannot connect a node to itself");
  }
  const bool same_tier = record.a.tier == record.b.tier;
  if (same_tier && !options_.allow_same_tier_adjacency) {
    return note(IssueSeverity::Error, StatusCode::IllegalAdjacency, to_string(record.id),
                "same-tier adjacency is disabled by fabric policy");
  }
  if (!same_tier && record.cls == LinkClass::SameTierPeer) {
    return note(IssueSeverity::Error, StatusCode::ContradictoryRole, to_string(record.id),
                "a leaf-to-spine link cannot be classified as a same-tier peer link");
  }
  if (same_tier && record.cls == LinkClass::LeafSpine) {
    return note(IssueSeverity::Error, StatusCode::ContradictoryRole, to_string(record.id),
                "a same-tier link cannot be classified as a leaf-to-spine link");
  }
  const auto endpoint_exists = [this](NodeKey key) {
    return key.tier == Tier::Leaf ? leaves_.count(LeafId{key.index}) != 0U
                                  : spines_.count(SpineId{key.index}) != 0U;
  };
  if (!endpoint_exists(record.a)) {
    return note(IssueSeverity::Error, StatusCode::DanglingEdge, to_string(record.id),
                "endpoint " + node_label(record.a) + " does not exist");
  }
  if (!endpoint_exists(record.b)) {
    return note(IssueSeverity::Error, StatusCode::DanglingEdge, to_string(record.id),
                "endpoint " + node_label(record.b) + " does not exist");
  }
  const auto port_belongs = [this](PortId port_id, NodeKey owner) {
    const auto it = ports_.find(port_id);
    return it != ports_.end() && it->second.owner == owner;
  };
  if (!port_belongs(record.port_a, record.a)) {
    return note(IssueSeverity::Error, StatusCode::DanglingEdge, to_string(record.id),
                "port " + to_string(record.port_a) + " does not belong to " + node_label(record.a));
  }
  if (!port_belongs(record.port_b, record.b)) {
    return note(IssueSeverity::Error, StatusCode::DanglingEdge, to_string(record.id),
                "port " + to_string(record.port_b) + " does not belong to " + node_label(record.b));
  }
  if (same_tier && record.a.tier == Tier::Leaf) {
    const LeafRecord& leaf_a = leaves_.at(LeafId{record.a.index});
    const LeafRecord& leaf_b = leaves_.at(LeafId{record.b.index});
    if (!is_peer_capable(leaf_a.role) || !is_peer_capable(leaf_b.role)) {
      return note(IssueSeverity::Error, StatusCode::ContradictoryRole, to_string(record.id),
                  "peer link endpoints must both carry a peer-capable leaf role");
    }
  }
  const auto speed_a = ports_.at(record.port_a).speed_mbps;
  const auto speed_b = ports_.at(record.port_b).speed_mbps;
  const std::uint64_t slower = speed_a < speed_b ? speed_a : speed_b;
  if (record.declared_speed_mbps != 0 && record.declared_speed_mbps > slower) {
    return note(IssueSeverity::Error, StatusCode::Conflicting, to_string(record.id),
                "declared link speed exceeds the slower endpoint port speed");
  }
  if (record.declared_speed_mbps > limits_.max_capacity_mbps) {
    return note(IssueSeverity::Error, StatusCode::Overflow, to_string(record.id),
                "declared link speed exceeds the configured maximum");
  }
  for (const auto& [other_id, other] : links_) {
    if ((other.port_a == record.port_a && other.port_b == record.port_b) ||
        (other.port_a == record.port_b && other.port_b == record.port_a)) {
      return note(IssueSeverity::Error, StatusCode::AlreadyExists, to_string(record.id),
                  "an equivalent link already exists as " + to_string(other_id));
    }
  }
  links_.emplace(record.id, std::move(record));
  return Status::success();
}

Status FabricBuilder::add_history_link(LinkRecord record) {
  if (frozen_) {
    return Status(StatusCode::Closed, "fabric builder is already frozen");
  }
  if (history_links_.size() >= limits_.max_history_records) {
    return note(IssueSeverity::Error, StatusCode::Exhausted, to_string(record.id),
                "historical link count exceeds the configured bound");
  }
  if (record.id.is_zero()) {
    return note(IssueSeverity::Error, StatusCode::Invalid, "history:" + to_string(record.id),
                "historical link identity zero is reserved");
  }
  if (history_links_.count(record.id) != 0U || links_.count(record.id) != 0U) {
    return note(IssueSeverity::Error, StatusCode::DuplicateIdentity, "history:" + to_string(record.id),
                "historical link identity is already defined");
  }
  if (record.generation >= generation_) {
    return note(IssueSeverity::Error, StatusCode::CrossGeneration, "history:" + to_string(record.id),
                "historical links must belong to a generation older than the fabric generation");
  }
  history_links_.emplace(record.id, std::move(record));
  return Status::success();
}

Status FabricBuilder::add_evidence(LinkEvidence record) {
  if (frozen_) {
    return Status(StatusCode::Closed, "fabric builder is already frozen");
  }
  if (evidence_.size() >= limits_.max_evidence_records) {
    return note(IssueSeverity::Error, StatusCode::Exhausted, "evidence:" + to_string(record.link),
                "evidence count exceeds the configured bound");
  }
  if (record.generation > generation_) {
    return note(IssueSeverity::Error, StatusCode::CrossGeneration, "evidence:" + to_string(record.link),
                "evidence refers to a future topology generation");
  }
  if (record.observer.is_zero()) {
    return note(IssueSeverity::Error, StatusCode::Invalid, "evidence:" + to_string(record.link),
                "evidence must name the observing controller incarnation");
  }
  if (record.observed_seq.is_zero()) {
    return note(IssueSeverity::Error, StatusCode::Invalid, "evidence:" + to_string(record.link),
                "evidence must carry a non-zero observation sequence");
  }
  if (links_.count(record.link) == 0U && history_links_.count(record.link) == 0U) {
    return note(IssueSeverity::Error, StatusCode::DanglingEdge, "evidence:" + to_string(record.link),
                "evidence refers to a link that does not exist in this fabric");
  }
  evidence_.push_back(std::move(record));
  return Status::success();
}

void FabricBuilder::validate_domains() {
  if (domains_.empty()) {
    return;
  }
  for (const auto& [id, leaf] : leaves_) {
    if (domains_.count(leaf.domain) == 0U) {
      (void)note(IssueSeverity::Warning, StatusCode::NotFound, to_string(id),
                 "failure domain " + std::to_string(leaf.domain.value) + " is not declared");
    }
  }
  for (const auto& [id, spine] : spines_) {
    if (domains_.count(spine.domain) == 0U) {
      (void)note(IssueSeverity::Warning, StatusCode::NotFound, to_string(id),
                 "failure domain " + std::to_string(spine.domain.value) + " is not declared");
    }
  }
}

void FabricBuilder::validate_ports() {
  for (const auto& [id, port] : ports_) {
    const bool referenced = std::any_of(links_.begin(), links_.end(), [&port](const auto& entry) {
      return entry.second.port_a == port.id || entry.second.port_b == port.id;
    });
    if (!referenced) {
      (void)note(IssueSeverity::Info, StatusCode::NotFound, to_string(id), "port is not used by any link");
    }
  }
}

void FabricBuilder::validate_links() {
  for (const auto& [id, link] : links_) {
    const auto speed = [this, &link]() -> std::optional<std::uint64_t> {
      const auto it_a = ports_.find(link.port_a);
      const auto it_b = ports_.find(link.port_b);
      if (it_a == ports_.end() || it_b == ports_.end()) {
        return std::nullopt;
      }
      if (link.declared_speed_mbps != 0) {
        return link.declared_speed_mbps;
      }
      return it_a->second.speed_mbps < it_b->second.speed_mbps ? it_a->second.speed_mbps
                                                               : it_b->second.speed_mbps;
    }();
    if (!speed.has_value() || *speed == 0) {
      (void)note(IssueSeverity::Error, StatusCode::Invalid, to_string(id),
                 "link has no usable speed (a port is missing or declares zero speed)");
      continue;
    }
    if (link.admin == LinkAdmin::Disabled) {
      (void)note(IssueSeverity::Info, StatusCode::Refused, to_string(id),
                 "link is administratively disabled and is excluded from the structural graph");
    }
    const bool leaf_endpoint = link.a.tier == Tier::Leaf || link.b.tier == Tier::Leaf;
    if (link.cls == LinkClass::LeafSpine && !leaf_endpoint) {
      (void)note(IssueSeverity::Error, StatusCode::ContradictoryRole, to_string(id),
                 "leaf-to-spine link has no leaf endpoint");
    }
    const NodeKey leaf_key = link.a.tier == Tier::Leaf ? link.a : link.b;
    const NodeKey spine_key = link.a.tier == Tier::Leaf ? link.b : link.a;
    if (link.cls == LinkClass::LeafSpine) {
      const SpineRecord& spine = spines_.at(SpineId{spine_key.index});
      if (spine.state == NodeState::Retired) {
        (void)note(IssueSeverity::Info, StatusCode::Stale, to_string(id),
                   "link attaches to a retired spine");
      }
    }
    if (link.cls == LinkClass::SameTierPeer && link.a.tier == Tier::Leaf) {
      (void)note(IssueSeverity::Info, StatusCode::Ok, to_string(id),
                 "peer link " + node_label(leaf_key) + " <-> " + node_label(link.b));
    }
  }
}

void FabricBuilder::validate_evidence() {
  std::map<LinkId, SequenceNumber> highest;
  std::map<LinkId, int> duplicates;
  for (const auto& record : evidence_) {
    if (record.generation > generation_) {
      (void)note(IssueSeverity::Error, StatusCode::CrossGeneration, "evidence:" + to_string(record.link),
                 "evidence refers to a future topology generation");
      continue;
    }
    const auto it = highest.find(record.link);
    if (it == highest.end() || it->second < record.observed_seq) {
      highest[record.link] = record.observed_seq;
      duplicates[record.link] = 1;
      continue;
    }
    if (it->second == record.observed_seq) {
      ++duplicates[record.link];
    }
  }
  for (const auto& [link, count] : duplicates) {
    if (count > 1) {
      (void)note(IssueSeverity::Warning, StatusCode::Conflicting, "evidence:" + to_string(link),
                 "multiple evidence records share the highest observation sequence; the link is "
                 "reported as conflicting rather than decided");
    }
  }
  for (const auto& [link, seq] : highest) {
    if (links_.count(link) == 0U) {
      continue;
    }
    const auto it = std::find_if(evidence_.begin(), evidence_.end(),
                                 [link, seq](const LinkEvidence& record) {
                                   return record.link == link && record.observed_seq == seq;
                                 });
    if (it != evidence_.end() && it->observed == LinkObservation::Unknown) {
      (void)note(IssueSeverity::Warning, StatusCode::Indeterminate, "evidence:" + to_string(link),
                 "the newest observation is unknown; the link cannot be decided from evidence");
    }
  }
}

Status FabricBuilder::finish_report() {
  report_.leaf_count = static_cast<std::uint32_t>(leaves_.size());
  report_.spine_count = static_cast<std::uint32_t>(spines_.size());
  report_.port_count = static_cast<std::uint32_t>(ports_.size());
  report_.link_count = static_cast<std::uint32_t>(links_.size());
  report_.domain_count = static_cast<std::uint32_t>(domains_.size());
  report_.history_count = static_cast<std::uint32_t>(history_links_.size());
  report_.evidence_count = static_cast<std::uint32_t>(evidence_.size());
  if (report_.error_count != 0) {
    for (const auto& issue : report_.issues) {
      if (issue.severity == IssueSeverity::Error) {
        return Status(issue.code, issue.where + ": " + issue.detail);
      }
    }
    return Status(StatusCode::Invalid, "fabric has validation errors");
  }
  return Status::success();
}

Outcome<Fabric> FabricBuilder::freeze() {
  if (frozen_) {
    return Status(StatusCode::Closed, "fabric builder is already frozen");
  }
  frozen_ = true;
  validate_domains();
  validate_ports();
  validate_links();
  validate_evidence();
  const Status report_status = finish_report();
  if (!report_status.ok()) {
    return report_status;
  }

  Fabric fabric;
  fabric.id = id_;
  fabric.name = name_;
  fabric.site = site_;
  fabric.generation = generation_;
  fabric.epoch = epoch_;
  fabric.state = fabric_state_;
  fabric.options = options_;
  fabric.domains.reserve(domains_.size());
  for (const auto& [id, record] : domains_) {
    (void)id;
    fabric.domains.push_back(record);
  }
  fabric.leaves.reserve(leaves_.size());
  for (const auto& [id, record] : leaves_) {
    (void)id;
    fabric.leaves.push_back(record);
  }
  fabric.spines.reserve(spines_.size());
  for (const auto& [id, record] : spines_) {
    (void)id;
    fabric.spines.push_back(record);
  }
  fabric.ports.reserve(ports_.size());
  for (const auto& [id, record] : ports_) {
    (void)id;
    fabric.ports.push_back(record);
  }
  fabric.links.reserve(links_.size());
  for (const auto& [id, record] : links_) {
    (void)id;
    fabric.links.push_back(record);
  }
  fabric.history_links.reserve(history_links_.size());
  for (const auto& [id, record] : history_links_) {
    (void)id;
    fabric.history_links.push_back(record);
  }
  fabric.evidence = evidence_;
  std::sort(fabric.evidence.begin(), fabric.evidence.end(), [](const LinkEvidence& a, const LinkEvidence& b) {
    if (a.link != b.link) {
      return a.link < b.link;
    }
    if (a.observed_seq != b.observed_seq) {
      return a.observed_seq < b.observed_seq;
    }
    if (a.generation != b.generation) {
      return a.generation < b.generation;
    }
    return a.observer < b.observer;
  });
  const auto [topology_digest, evidence_digest] = compute_digests(fabric);
  fabric.topology_digest = topology_digest;
  fabric.evidence_digest = evidence_digest;
  return fabric;
}

Outcome<Fabric> build_fabric(FabricId id, std::string name, TopologyGeneration generation, Epoch epoch,
                             FabricOptions options, const std::vector<LeafRecord>& leaves,
                             const std::vector<SpineRecord>& spines, const std::vector<PortRecord>& ports,
                             const std::vector<LinkRecord>& links, ValidationReport* report_out,
                             BuilderLimits limits) {
  FabricBuilder builder(id, std::move(name), generation, epoch, options, limits);
  for (const auto& record : leaves) {
    const Status status = builder.add_leaf(record);
    if (!status.ok()) {
      if (report_out != nullptr) {
        *report_out = builder.report();
      }
      return status;
    }
  }
  for (const auto& record : spines) {
    const Status status = builder.add_spine(record);
    if (!status.ok()) {
      if (report_out != nullptr) {
        *report_out = builder.report();
      }
      return status;
    }
  }
  for (const auto& record : ports) {
    const Status status = builder.add_port(record);
    if (!status.ok()) {
      if (report_out != nullptr) {
        *report_out = builder.report();
      }
      return status;
    }
  }
  for (const auto& record : links) {
    const Status status = builder.add_link(record);
    if (!status.ok()) {
      if (report_out != nullptr) {
        *report_out = builder.report();
      }
      return status;
    }
  }
  Outcome<Fabric> fabric = builder.freeze();
  if (report_out != nullptr) {
    *report_out = builder.report();
  }
  return fabric;
}

}  // namespace slf
