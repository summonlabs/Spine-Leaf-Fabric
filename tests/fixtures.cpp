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

#include "fixtures.hpp"

namespace slf::test {

LeafRecord leaf_record(const LeafSpec& spec, TopologyGeneration generation) {
  LeafRecord record;
  record.id = LeafId{spec.id};
  record.name = spec.name.empty() ? ("leaf-" + std::to_string(spec.id)) : spec.name;
  record.role = spec.role;
  record.role_incarnation = RoleIncarnation{spec.role_incarnation == 0 ? spec.id : spec.role_incarnation};
  record.domain = FailureDomainId{spec.domain};
  record.state = spec.state;
  record.access_capacity_mbps = spec.access_mbps;
  record.access_link_count = 8;
  record.generation = generation;
  return record;
}

SpineRecord spine_record(const SpineSpec& spec, TopologyGeneration generation) {
  SpineRecord record;
  record.id = SpineId{spec.id};
  record.name = spec.name.empty() ? ("spine-" + std::to_string(spec.id)) : spec.name;
  record.role = spec.role;
  record.role_incarnation =
      RoleIncarnation{spec.role_incarnation == 0 ? (1000 + spec.id) : spec.role_incarnation};
  record.domain = FailureDomainId{spec.domain};
  record.state = spec.state;
  record.fabric_capacity_mbps = spec.fabric_mbps;
  record.generation = generation;
  return record;
}

Outcome<Fabric> make_grid(TopologyGeneration generation, const std::vector<LeafSpec>& leaves,
                          const std::vector<SpineSpec>& spines, const std::vector<LinkSpec>& links,
                          FabricOptions options, std::uint64_t epoch) {
  FabricBuilder builder(FabricId{1}, "test-fabric", generation, Epoch{epoch}, options);
  for (const auto& spec : leaves) {
    const Status status = builder.add_leaf(leaf_record(spec, generation));
    if (!status.ok()) {
      return status;
    }
  }
  for (const auto& spec : spines) {
    const Status status = builder.add_spine(spine_record(spec, generation));
    if (!status.ok()) {
      return status;
    }
  }
  std::uint32_t port_id = 1;
  std::uint32_t link_id = 1;
  for (const auto& spec : links) {
    PortRecord leaf_port;
    leaf_port.id = PortId{port_id++};
    leaf_port.owner = NodeKey{LeafId{spec.leaf}};
    leaf_port.speed_mbps = spec.speed_mbps;
    leaf_port.admin = spec.leaf_port_admin;
    leaf_port.generation = generation;
    PortRecord spine_port;
    spine_port.id = PortId{port_id++};
    spine_port.owner = NodeKey{SpineId{spec.spine}};
    spine_port.speed_mbps = spec.speed_mbps;
    spine_port.admin = spec.spine_port_admin;
    spine_port.generation = generation;
    LinkRecord link;
    link.id = LinkId{link_id++};
    link.a = NodeKey{LeafId{spec.leaf}};
    link.port_a = leaf_port.id;
    link.b = NodeKey{SpineId{spec.spine}};
    link.port_b = spine_port.id;
    link.cls = LinkClass::LeafSpine;
    link.admin = spec.admin;
    link.generation = generation;
    const Status status_a = builder.add_port(std::move(leaf_port));
    if (!status_a.ok()) {
      return status_a;
    }
    const Status status_b = builder.add_port(std::move(spine_port));
    if (!status_b.ok()) {
      return status_b;
    }
    const Status status_c = builder.add_link(std::move(link));
    if (!status_c.ok()) {
      return status_c;
    }
  }
  return builder.freeze();
}

Outcome<Fabric> make_two_by_two(FabricOptions options) {
  const std::vector<LeafSpec> leaves{{1, "", LeafRole::Tor, 1, NodeState::Active, 400000, 11},
                                     {2, "", LeafRole::Tor, 2, NodeState::Active, 400000, 12}};
  const std::vector<SpineSpec> spines{{1, "", SpineRole::FabricSpine, 1, NodeState::Active, 3200000, 21},
                                      {2, "", SpineRole::FabricSpine, 2, NodeState::Active, 3200000, 22}};
  const std::vector<LinkSpec> links{{1, 1, 100000}, {1, 2, 100000}, {2, 1, 100000}, {2, 2, 100000}};
  return make_grid(TopologyGeneration{1}, leaves, spines, links, options);
}

std::vector<LinkEvidence> observe_all_links(const Fabric& fabric, ControllerIncarnation observer, Epoch epoch,
                                            LinkObservation observation, std::uint64_t sequence_base) {
  std::vector<LinkEvidence> records;
  std::uint64_t sequence = sequence_base;
  for (const auto& link : fabric.links) {
    LinkEvidence record;
    record.link = link.id;
    record.observed = observation;
    record.generation = fabric.generation;
    record.observer = observer;
    record.epoch = epoch;
    record.observed_seq = SequenceNumber{sequence++};
    record.observed_at_unix_ms = 1000 + sequence;
    records.push_back(record);
  }
  return records;
}

}  // namespace slf::test
