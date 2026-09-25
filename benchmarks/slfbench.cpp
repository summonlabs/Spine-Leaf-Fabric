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

// slfbench: completed-work benchmarks over a deterministic synthetic fabric.
//
// The fabric is generated from a fixed seed, so the numbers are comparable
// between runs on the same machine. Nothing here measures hardware, switching
// silicon, or multi-host behaviour: it measures this runtime's own data
// structures, persistence path, and framed transport.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "slf/builder.hpp"
#include "slf/eligibility.hpp"
#include "slf/platform.hpp"
#include "slf/runtime.hpp"
#include "slf/service.hpp"
#include "slf/spec.hpp"
#include "slf/topology.hpp"

namespace {

using namespace slf;

struct Stopwatch {
  std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
  [[nodiscard]] double ms() const {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(now - start).count();
  }
};

void report(const std::string& name, double milliseconds, std::uint64_t operations, const char* unit) {
  const double per_operation = operations == 0 ? 0.0 : milliseconds * 1000.0 / static_cast<double>(operations);
  std::printf("%-44s %10.3f ms  %12llu %-8s %10.3f us/op\n", name.c_str(), milliseconds,
              static_cast<unsigned long long>(operations), unit, per_operation);
}

struct Shape {
  std::uint32_t leaves{64};
  std::uint32_t spines{8};
  std::uint32_t domains{4};
};

Outcome<Fabric> synthesise(const Shape& shape) {
  FabricBuilder builder(FabricId{1}, "bench-fabric", TopologyGeneration{1}, Epoch{1},
                        FabricOptions{.require_domain_diversity = true, .min_eligible_spines = 2});
  for (std::uint32_t domain = 1; domain <= shape.domains; ++domain) {
    FailureDomainRecord record;
    record.id = FailureDomainId{domain};
    record.name = "domain-" + std::to_string(domain);
    record.generation = TopologyGeneration{1};
    const Status status = builder.add_failure_domain(std::move(record));
    if (!status.ok()) {
      return status;
    }
  }
  for (std::uint32_t i = 1; i <= shape.leaves; ++i) {
    LeafRecord leaf;
    leaf.id = LeafId{i};
    leaf.name = "leaf-" + std::to_string(i);
    leaf.role = LeafRole::Tor;
    leaf.role_incarnation = RoleIncarnation{i};
    leaf.domain = FailureDomainId{(i % shape.domains) + 1};
    leaf.access_capacity_mbps = 400000;
    leaf.generation = TopologyGeneration{1};
    const Status status = builder.add_leaf(std::move(leaf));
    if (!status.ok()) {
      return status;
    }
  }
  for (std::uint32_t i = 1; i <= shape.spines; ++i) {
    SpineRecord spine;
    spine.id = SpineId{i};
    spine.name = "spine-" + std::to_string(i);
    spine.role = SpineRole::FabricSpine;
    spine.role_incarnation = RoleIncarnation{1000 + i};
    spine.domain = FailureDomainId{(i % shape.domains) + 1};
    spine.fabric_capacity_mbps = 3200000;
    spine.generation = TopologyGeneration{1};
    const Status status = builder.add_spine(std::move(spine));
    if (!status.ok()) {
      return status;
    }
  }
  std::uint32_t port_id = 1;
  std::uint32_t link_id = 1;
  for (std::uint32_t leaf_index = 1; leaf_index <= shape.leaves; ++leaf_index) {
    for (std::uint32_t spine_index = 1; spine_index <= shape.spines; ++spine_index) {
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
  }
  return builder.freeze();
}

}  // namespace

int main(int argc, char** argv) {
  Shape shape;
  std::uint32_t queries = 2000;
  std::uint64_t journal_records = 2000;
  bool run_transport = true;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--small") {
      shape = Shape{16, 4, 2};
    } else if (argument == "--large") {
      shape = Shape{256, 16, 8};
    } else if (argument == "--no-transport") {
      run_transport = false;
    } else if (argument.rfind("--queries=", 0) == 0) {
      queries = static_cast<std::uint32_t>(std::strtoul(argument.c_str() + 10, nullptr, 10));
    } else if (argument.rfind("--records=", 0) == 0) {
      journal_records = std::strtoull(argument.c_str() + 10, nullptr, 10);
    } else if (argument == "--help") {
      std::cout << "usage: slfbench [--small|--large] [--queries=N] [--records=N] [--no-transport]\n";
      return 0;
    }
  }

  std::printf("spine-leaf-fabric benchmark\n");
  std::printf("build: %s\n", std::string(build_info()).c_str());
  std::printf("shape: %u leaves, %u spines, %u failure domains\n", shape.leaves, shape.spines, shape.domains);
  std::printf("%-44s %13s  %12s %-8s %10s\n", "benchmark", "elapsed", "operations", "unit", "per op");

  Stopwatch total;
  Stopwatch watch;
  const auto fabric = synthesise(shape);
  if (!fabric.ok()) {
    std::cerr << "synthesis failed: " << fabric.status().to_string() << "\n";
    return 1;
  }
  const double build_ms = watch.ms();
  report("fabric build + validate + digest", build_ms, fabric.value().links.size(), "links");

  watch = Stopwatch{};
  const auto bytes = canonical_topology_bytes(fabric.value());
  report("canonical encoding", watch.ms(), bytes.size(), "bytes");

  watch = Stopwatch{};
  for (int i = 0; i < 20; ++i) {
    (void)Digest::of(std::span<const std::byte>(bytes));
  }
  report("sha256 digest (20 passes over canonical bytes)", watch.ms(), 20, "passes");

  watch = Stopwatch{};
  auto index = TopologyIndex::build(fabric.value());
  if (!index.ok()) {
    std::cerr << index.status().to_string() << "\n";
    return 1;
  }
  report("topology index build", watch.ms(), index.value().edge_count(), "arcs");

  EligibilityEngine engine(index.value());
  const auto constraints = PathConstraints::defaults_for(fabric.value().options);

  watch = Stopwatch{};
  std::uint64_t assessed = 0;
  for (std::uint32_t i = 0; i < queries; ++i) {
    const LeafId from{1 + (i % shape.leaves)};
    const LeafId to{1 + ((i + 1) % shape.leaves)};
    const auto assessment = engine.assess_leaf_pair(from, to, constraints);
    if (!assessment.ok()) {
      std::cerr << assessment.status().to_string() << "\n";
      return 1;
    }
    ++assessed;
  }
  report("leaf-pair assessment (eligible spine sets)", watch.ms(), assessed, "queries");

  watch = Stopwatch{};
  for (std::uint32_t i = 0; i < queries; ++i) {
    const auto entries = engine.eligible_spines_for_leaf(LeafId{1 + (i % shape.leaves)}, constraints);
    if (!entries.ok()) {
      std::cerr << entries.status().to_string() << "\n";
      return 1;
    }
  }
  report("per-leaf eligible spine evaluation", watch.ms(), queries, "queries");

  watch = Stopwatch{};
  for (std::uint32_t i = 0; i < 200; ++i) {
    const auto health = engine.health(constraints, ControllerIncarnation{}, fabric.value().epoch);
    if (!health.ok()) {
      std::cerr << health.status().to_string() << "\n";
      return 1;
    }
  }
  report("fabric health classification", watch.ms(), 200, "passes");

  // Persistence: journal append (durable) and recovery.
  const std::filesystem::path state = std::filesystem::temp_directory_path() / "slf-bench-state";
  std::error_code ec;
  std::filesystem::remove_all(state, ec);
  StoreConfig store_config;
  store_config.sync_on_commit = false;  // throughput measurement without fsync per record
  const auto incarnation = mint_incarnation();
  auto store = FabricStore::create(state, store_config, incarnation, 1, SequenceNumber{1});
  if (!store.ok()) {
    std::cerr << store.status().to_string() << "\n";
    return 1;
  }
  const std::vector<std::byte> payload = serialize_fabric(fabric.value());
  watch = Stopwatch{};
  for (std::uint64_t i = 0; i < journal_records; ++i) {
    const Status status = store.value()->append(RecordType::FabricSnapshot, payload, incarnation, Epoch{1},
                                                fabric.value().generation);
    if (!status.ok()) {
      std::cerr << status.to_string() << "\n";
      return 1;
    }
  }
  const double append_ms = watch.ms();
  report("journal append (no fsync per record)", append_ms, journal_records, "records");
  (void)store.value()->close_clean();

  watch = Stopwatch{};
  std::vector<RecoveredRecord> records;
  const auto recovery = recover_journal(default_journal_path(state, 1), store_config, &records);
  if (!recovery.ok()) {
    std::cerr << recovery.status().to_string() << "\n";
    return 1;
  }
  report("journal recovery + integrity verification", watch.ms(), records.size(), "records");
  std::printf("  recovery status: %s\n", std::string(to_string(recovery.value().status)).c_str());

  watch = Stopwatch{};
  for (std::uint64_t i = 0; i < 50; ++i) {
    const auto parsed = deserialize_fabric(std::span<const std::byte>(payload), BuilderLimits{});
    if (!parsed.ok()) {
      std::cerr << parsed.status().to_string() << "\n";
      return 1;
    }
  }
  report("fabric deserialization", watch.ms(), 50, "passes");
  std::filesystem::remove_all(state, ec);

  if (run_transport) {
    RuntimeConfig runtime_config;
    runtime_config.enable_persistence = false;
    runtime_config.worker_threads = 0;
    ControllerRuntime runtime(runtime_config);
    if (const Status started = runtime.start(); !started.ok()) {
      std::cerr << started.to_string() << "\n";
      return 1;
    }
    if (const Status installed = runtime.install_fabric(fabric.value(), true); !installed.ok()) {
      std::cerr << installed.to_string() << "\n";
      return 1;
    }
    net::RuntimeService service(runtime);
    net::ServerConfig server_config;
    server_config.endpoint.host = "127.0.0.1";
    server_config.endpoint.port = 0;
    auto server = net::FabricServer::create(server_config, &service);
    if (!server.ok()) {
      std::cerr << server.status().to_string() << "\n";
      return 1;
    }
    if (const Status listening = server.value()->start(); !listening.ok()) {
      std::cerr << listening.to_string() << "\n";
      return 1;
    }
    net::ClientConfig client_config;
    client_config.endpoint.host = "127.0.0.1";
    client_config.endpoint.port = server.value()->port();
    auto client = net::FabricClient::connect(client_config);
    if (!client.ok()) {
      std::cerr << client.status().to_string() << "\n";
      return 1;
    }
    const std::uint32_t round_trips = 500;
    watch = Stopwatch{};
    for (std::uint32_t i = 0; i < round_trips; ++i) {
      net::Request request;
      request.kind = net::RequestKind::Status;
      request.request_id = RequestId{1000 + i};
      const auto response = client.value()->call(request);
      if (!response.ok()) {
        std::cerr << response.status().to_string() << "\n";
        return 1;
      }
    }
    const double rtt_ms = watch.ms();
    report("loopback frame round trip (status)", rtt_ms, round_trips, "requests");
    std::printf("  round-trip latency: %.3f us\n", rtt_ms * 1000.0 / static_cast<double>(round_trips));
    (void)client.value()->close();
    (void)server.value()->stop();
    (void)runtime.stop();
  }

  std::printf("%-44s %10.3f ms\n", "total wall clock", total.ms());
  return 0;
}
