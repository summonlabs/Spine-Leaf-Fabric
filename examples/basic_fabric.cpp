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

// Example: build a two-tier fabric programmatically, validate it, and ask which
// spine sets remain eligible for a leaf pair. Everything here is deterministic
// and offline: no sockets, no persistence, no hardware.

#include <iostream>
#include <string>

#include "slf/builder.hpp"
#include "slf/eligibility.hpp"
#include "slf/topology.hpp"

namespace {

using namespace slf;

/// Builds a 4-leaf / 2-spine fabric where each leaf has one uplink to each
/// spine, spines live in different failure domains, and domain diversity is
/// required by policy.
Outcome<Fabric> build_example_fabric() {
  FabricBuilder builder(FabricId{1}, "example-fabric", TopologyGeneration{1}, Epoch{1},
                        FabricOptions{.require_domain_diversity = true, .min_eligible_spines = 2});
  (void)builder.set_site("lab");
  for (std::uint32_t domain = 1; domain <= 2; ++domain) {
    FailureDomainRecord record;
    record.id = FailureDomainId{domain};
    record.name = "domain-" + std::to_string(domain);
    record.generation = TopologyGeneration{1};
    const Status status = builder.add_failure_domain(std::move(record));
    if (!status.ok()) {
      return status;
    }
  }
  for (std::uint32_t i = 1; i <= 4; ++i) {
    LeafRecord leaf;
    leaf.id = LeafId{i};
    leaf.name = "leaf-" + std::to_string(i);
    leaf.role = LeafRole::Tor;
    leaf.role_incarnation = RoleIncarnation{i};
    leaf.domain = FailureDomainId{(i % 2) + 1};
    leaf.access_capacity_mbps = 400000;
    leaf.access_link_count = 20;
    leaf.generation = TopologyGeneration{1};
    const Status status = builder.add_leaf(std::move(leaf));
    if (!status.ok()) {
      return status;
    }
  }
  for (std::uint32_t i = 1; i <= 2; ++i) {
    SpineRecord spine;
    spine.id = SpineId{i};
    spine.name = "spine-" + std::to_string(i);
    spine.role = SpineRole::FabricSpine;
    spine.role_incarnation = RoleIncarnation{100 + i};
    spine.domain = FailureDomainId{i};
    spine.fabric_capacity_mbps = 3200000;
    spine.generation = TopologyGeneration{1};
    const Status status = builder.add_spine(std::move(spine));
    if (!status.ok()) {
      return status;
    }
  }
  std::uint32_t port_id = 1;
  std::uint32_t link_id = 1;
  for (std::uint32_t leaf_index = 1; leaf_index <= 4; ++leaf_index) {
    for (std::uint32_t spine_index = 1; spine_index <= 2; ++spine_index) {
      PortRecord leaf_port;
      leaf_port.id = PortId{port_id++};
      leaf_port.owner = NodeKey{LeafId{leaf_index}};
      leaf_port.speed_mbps = 100000;
      leaf_port.generation = TopologyGeneration{1};
      PortRecord spine_port;
      spine_port.id = PortId{port_id++};
      spine_port.owner = NodeKey{SpineId{spine_index}};
      spine_port.speed_mbps = 100000;
      spine_port.generation = TopologyGeneration{1};
      LinkRecord link;
      link.id = LinkId{link_id++};
      link.a = NodeKey{LeafId{leaf_index}};
      link.port_a = leaf_port.id;
      link.b = NodeKey{SpineId{spine_index}};
      link.port_b = spine_port.id;
      link.cls = LinkClass::LeafSpine;
      link.generation = TopologyGeneration{1};
      const Status port_status = builder.add_port(std::move(leaf_port));
      if (!port_status.ok()) {
        return port_status;
      }
      const Status spine_port_status = builder.add_port(std::move(spine_port));
      if (!spine_port_status.ok()) {
        return spine_port_status;
      }
      const Status link_status = builder.add_link(std::move(link));
      if (!link_status.ok()) {
        return link_status;
      }
    }
  }
  return builder.freeze();
}

}  // namespace

int main() {
  const auto fabric = build_example_fabric();
  if (!fabric.ok()) {
    std::cerr << "fabric did not validate: " << fabric.status().to_string() << "\n";
    return 1;
  }
  std::cout << describe_fabric(fabric.value());

  const auto index = TopologyIndex::build(fabric.value());
  if (!index.ok()) {
    std::cerr << "index failed: " << index.status().to_string() << "\n";
    return 1;
  }
  EligibilityEngine engine(index.value());
  const auto constraints = PathConstraints::defaults_for(fabric.value().options);
  const auto assessment = engine.assess_leaf_pair(LeafId{1}, LeafId{2}, constraints);
  if (!assessment.ok()) {
    std::cerr << "assessment failed: " << assessment.status().to_string() << "\n";
    return 1;
  }
  std::cout << assessment.value().to_string();

  const auto health = engine.health(constraints, ControllerIncarnation{}, fabric.value().epoch);
  if (health.ok()) {
    std::cout << health.value().to_string();
  }

  // Drain one spine administratively and show the effect on eligibility.
  FabricBuilder builder(FabricId{1}, "example-fabric", TopologyGeneration{2}, Epoch{1},
                        fabric.value().options);
  (void)builder.set_site(fabric.value().site);
  for (const auto& domain : fabric.value().domains) {
    (void)builder.add_failure_domain(domain);
  }
  for (auto leaf : fabric.value().leaves) {
    leaf.generation = TopologyGeneration{2};
    (void)builder.add_leaf(leaf);
  }
  for (auto spine : fabric.value().spines) {
    spine.generation = TopologyGeneration{2};
    if (spine.id == SpineId{2}) {
      spine.state = NodeState::Draining;
    }
    (void)builder.add_spine(spine);
  }
  for (auto port : fabric.value().ports) {
    port.generation = TopologyGeneration{2};
    (void)builder.add_port(port);
  }
  for (auto link : fabric.value().links) {
    link.generation = TopologyGeneration{2};
    (void)builder.add_link(link);
  }
  const auto drained = builder.freeze();
  if (!drained.ok()) {
    std::cerr << "drained fabric did not validate: " << drained.status().to_string() << "\n";
    return 1;
  }
  const auto drained_index = TopologyIndex::build(drained.value());
  if (!drained_index.ok()) {
    std::cerr << drained_index.status().to_string() << "\n";
    return 1;
  }
  EligibilityEngine drained_engine(drained_index.value());
  const auto drained_assessment = drained_engine.assess_leaf_pair(LeafId{1}, LeafId{2}, constraints);
  if (!drained_assessment.ok()) {
    std::cerr << drained_assessment.status().to_string() << "\n";
    return 1;
  }
  std::cout << "after draining spine:2 -> " << drained_assessment.value().summary() << "\n";
  return 0;
}
