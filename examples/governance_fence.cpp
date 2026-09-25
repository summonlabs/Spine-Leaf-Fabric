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

// Example: authority fencing across a controller restart.
//
// The runtime persists to a state directory, a lease is minted, a command is
// applied with that lease, the runtime is stopped and started again (a fresh
// process incarnation and a bumped epoch), and the old token is refused with
// StatusCode::Fenced. Nothing here is hardware: the "topology" is a two-leaf
// fabric described in code.

#include <filesystem>
#include <iostream>
#include <string>

#include "slf/builder.hpp"
#include "slf/platform.hpp"
#include "slf/runtime.hpp"

namespace {

using namespace slf;

Outcome<Fabric> small_fabric() {
  FabricBuilder builder(FabricId{7}, "fence-demo", TopologyGeneration{1}, Epoch{1},
                        FabricOptions{.min_eligible_spines = 1});
  LeafRecord leaf;
  leaf.id = LeafId{1};
  leaf.name = "leaf-1";
  leaf.role = LeafRole::Access;
  leaf.role_incarnation = RoleIncarnation{5};
  leaf.domain = FailureDomainId{1};
  leaf.access_capacity_mbps = 100000;
  leaf.generation = TopologyGeneration{1};
  const Status leaf_status = builder.add_leaf(std::move(leaf));
  if (!leaf_status.ok()) {
    return leaf_status;
  }
  SpineRecord spine;
  spine.id = SpineId{1};
  spine.name = "spine-1";
  spine.role = SpineRole::FabricSpine;
  spine.role_incarnation = RoleIncarnation{9};
  spine.domain = FailureDomainId{1};
  spine.fabric_capacity_mbps = 400000;
  spine.generation = TopologyGeneration{1};
  const Status spine_status = builder.add_spine(std::move(spine));
  if (!spine_status.ok()) {
    return spine_status;
  }
  PortRecord leaf_port;
  leaf_port.id = PortId{1};
  leaf_port.owner = NodeKey{LeafId{1}};
  leaf_port.speed_mbps = 100000;
  leaf_port.generation = TopologyGeneration{1};
  PortRecord spine_port;
  spine_port.id = PortId{2};
  spine_port.owner = NodeKey{SpineId{1}};
  spine_port.speed_mbps = 100000;
  spine_port.generation = TopologyGeneration{1};
  LinkRecord link;
  link.id = LinkId{1};
  link.a = NodeKey{LeafId{1}};
  link.port_a = PortId{1};
  link.b = NodeKey{SpineId{1}};
  link.port_b = PortId{2};
  link.cls = LinkClass::LeafSpine;
  link.generation = TopologyGeneration{1};
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
  return builder.freeze();
}

}  // namespace

int main() {
  const std::filesystem::path state = std::filesystem::temp_directory_path() / "slf-example-governance";
  std::error_code ec;
  std::filesystem::remove_all(state, ec);

  RuntimeConfig config;
  config.fabric_id = FabricId{7};
  config.name = "fence-demo";
  config.state_dir = state;
  config.worker_threads = 0;

  ControllerRuntime runtime(config);
  if (const Status started = runtime.start(); !started.ok()) {
    std::cerr << "start failed: " << started.to_string() << "\n";
    return 1;
  }
  if (const Status installed = runtime.install_fabric(small_fabric().value(), true); !installed.ok()) {
    std::cerr << "install failed: " << installed.to_string() << "\n";
    return 1;
  }
  const auto lease = runtime.acquire_authority("example");
  if (!lease.ok()) {
    std::cerr << "lease failed: " << lease.status().to_string() << "\n";
    return 1;
  }
  std::cout << "first controller: " << to_string(lease.value().token.controller)
            << " epoch=" << lease.value().token.epoch.value << "\n";

  Command command;
  command.request_id = RequestId{1};
  command.token = lease.value().token;
  command.expected_generation = runtime.status().generation;
  command.expected_epoch = lease.value().token.epoch;
  command.kind = CommandKind::SetFabricState;
  command.fabric_state = FabricState::Operational;
  const CommandResult applied = runtime.apply(command);
  std::cout << "command: " << applied.to_string() << "\n";
  const AuthorityToken old_token = lease.value().token;

  if (const Status restarted = runtime.restart(); !restarted.ok()) {
    std::cerr << "restart failed: " << restarted.to_string() << "\n";
    return 1;
  }
  const auto status = runtime.status();
  std::cout << "after restart: controller=" << to_string(status.controller)
            << " epoch=" << status.epoch.value << " generation=" << status.generation.value << "\n";
  std::cout << "recovery: " << status.recovery.to_string();

  const AuthorityCheck check = runtime.validate_authority(old_token);
  std::cout << "old token: " << to_string(check.code) << " (" << check.detail << ")\n";

  Command replay;
  replay.request_id = RequestId{1};
  replay.token = old_token;
  replay.expected_generation = runtime.status().generation;
  replay.expected_epoch = old_token.epoch;
  replay.kind = CommandKind::SetFabricState;
  replay.fabric_state = FabricState::Draining;
  const CommandResult refused = runtime.apply(replay);
  std::cout << "replayed command: " << refused.to_string() << "\n";

  const Status stopped = runtime.stop();
  std::filesystem::remove_all(state, ec);
  if (!stopped.ok()) {
    std::cerr << "stop failed: " << stopped.to_string() << "\n";
    return 1;
  }
  return check.code == StatusCode::Fenced ? 0 : 1;
}
