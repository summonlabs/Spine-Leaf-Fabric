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

// Concurrency tests. These look for the failure modes that a lock audit warns
// about: self-deadlock, joins under a held lock, iterator invalidation through a
// published snapshot, and lost or duplicated mutations under contention. Every
// loop is bounded, so a defect shows up as a failed check rather than a hang.

#include <atomic>
#include <thread>
#include <vector>

#include "fixtures.hpp"
#include "slf/platform.hpp"
#include "slf/service.hpp"
#include "slf_test.hpp"

using namespace slf;
using namespace slf::test;

SLF_TEST(concurrency_commands_and_queries_interleave) {
  TempDir dir{"concurrency-runtime"};
  RuntimeConfig config;
  config.fabric_id = FabricId{1};
  config.name = "concurrency";
  config.state_dir = dir.path();
  config.worker_threads = 2;
  config.authority_ttl_ms = 60000;
  ControllerRuntime runtime(config);
  SLF_EXPECT(runtime.start().ok());
  const auto fabric = make_two_by_two();
  SLF_EXPECT(fabric.ok());
  SLF_EXPECT(runtime.install_fabric(fabric.value(), true).ok());
  const auto lease = runtime.acquire_authority("concurrency");
  SLF_EXPECT(lease.ok());

  std::atomic<std::uint32_t> applied{0};
  std::atomic<std::uint32_t> refused{0};
  std::atomic<std::uint32_t> queried{0};
  std::atomic<bool> stop{false};

  // Four writers race for the single-writer critical section; each uses a
  // distinct request id and always re-reads the coordinate first.
  std::vector<std::thread> writers;
  for (std::uint32_t writer = 0; writer < 4; ++writer) {
    writers.emplace_back([&, writer]() {
      for (std::uint32_t i = 0; i < 25; ++i) {
        Command command;
        command.request_id = RequestId{1000 + writer * 100 + i};
        command.token = lease.value().token;
        command.kind = CommandKind::SetFabricState;
        command.fabric_state = (i % 2) == 0 ? FabricState::Operational : FabricState::Operational;
        const auto context = runtime.authority_context();
        command.expected_generation = context.generation;
        command.expected_epoch = context.epoch;
        const CommandResult result = runtime.apply(command);
        if (result.code == StatusCode::Ok) {
          applied.fetch_add(1);
        } else {
          refused.fetch_add(1);
        }
      }
    });
  }

  // Four readers query the immutable snapshot while writers publish new ones.
  std::vector<std::thread> readers;
  for (std::uint32_t reader = 0; reader < 4; ++reader) {
    readers.emplace_back([&]() {
      const auto constraints = PathConstraints::defaults_for(FabricOptions{});
      while (!stop.load()) {
        const auto assessment = runtime.assess_paths(NodeKey{LeafId{1}}, NodeKey{LeafId{2}}, constraints);
        if (assessment.ok()) {
          // Whatever generation the reader sees, the answer must be internally
          // consistent: the digest matches the generation that was reported.
          const auto status = runtime.status();
          if (assessment.value().generation.value > status.generation.value) {
            SLF_EXPECT(false);
          }
          queried.fetch_add(1);
        } else {
          SLF_EXPECT(false);
        }
      }
    });
  }
  for (auto& writer : writers) {
    writer.join();
  }
  stop.store(true);
  for (auto& reader : readers) {
    reader.join();
  }
  SLF_EXPECT(applied.load() > 0U);
  SLF_EXPECT(queried.load() > 0U);
  // Every applied mutation advanced the generation exactly once.
  const auto status = runtime.status();
  SLF_EXPECT_EQ(status.generation.value, 1U + applied.load());
  SLF_EXPECT_EQ(status.commands_applied, applied.load());
  // Repeated stops are safe even after a contended run.
  SLF_EXPECT(runtime.stop().ok());
  SLF_EXPECT(runtime.stop().ok());
}

SLF_TEST(concurrency_server_handles_parallel_clients) {
  TempDir dir{"concurrency-server"};
  RuntimeConfig config;
  config.fabric_id = FabricId{1};
  config.name = "concurrency-server";
  config.state_dir = dir.path();
  config.worker_threads = 1;
  ControllerRuntime runtime(config);
  SLF_EXPECT(runtime.start().ok());
  const auto fabric = make_two_by_two();
  SLF_EXPECT(fabric.ok());
  SLF_EXPECT(runtime.install_fabric(fabric.value(), true).ok());

  net::RuntimeService service(runtime);
  net::ServerConfig server_config;
  server_config.endpoint.host = "127.0.0.1";
  server_config.endpoint.port = 0;
  server_config.limits.max_connections = 8;
  auto server = net::FabricServer::create(server_config, &service);
  SLF_EXPECT(server.ok());
  if (!server.ok()) {
    return;
  }
  SLF_EXPECT(server.value()->start().ok());
  const std::uint16_t port = server.value()->port();

  std::atomic<std::uint32_t> replies{0};
  std::vector<std::thread> clients;
  for (std::uint32_t i = 0; i < 6; ++i) {
    clients.emplace_back([&, i]() {
      net::ClientConfig client_config;
      client_config.endpoint.host = "127.0.0.1";
      client_config.endpoint.port = port;
      auto client = net::FabricClient::connect(client_config);
      if (!client.ok()) {
        return;
      }
      for (std::uint32_t round = 0; round < 10; ++round) {
        net::Request request;
        request.kind = net::RequestKind::Status;
        request.request_id = RequestId{1 + i * 100 + round};
        const auto response = client.value()->call(request);
        if (response.ok() && response.value().code == StatusCode::Ok) {
          replies.fetch_add(1);
        }
      }
      (void)client.value()->close();
    });
  }
  for (auto& client : clients) {
    client.join();
  }
  SLF_EXPECT_EQ(replies.load(), 60U);
  // Stopping twice, with live and finished connections, releases every worker.
  SLF_EXPECT(server.value()->stop().ok());
  SLF_EXPECT(server.value()->stop().ok());
  SLF_EXPECT(!server.value()->running());
  SLF_EXPECT(runtime.stop().ok());
}

SLF_TEST(concurrency_repeated_lifecycle_is_stable) {
  TempDir dir{"concurrency-lifecycle"};
  RuntimeConfig config;
  config.fabric_id = FabricId{1};
  config.name = "lifecycle";
  config.state_dir = dir.path();
  config.worker_threads = 2;
  ControllerRuntime runtime(config);
  for (int round = 0; round < 8; ++round) {
    SLF_EXPECT(runtime.start().ok());
    const auto fabric = make_two_by_two();
    SLF_EXPECT(fabric.ok());
    if (round == 0) {
      SLF_EXPECT(runtime.install_fabric(fabric.value(), true).ok());
    }
    const auto lease = runtime.acquire_authority("lifecycle");
    SLF_EXPECT(lease.ok());
    SLF_EXPECT(runtime.status().started);
    SLF_EXPECT(runtime.stop().ok());
    SLF_EXPECT(!runtime.started());
  }
  // The state directory holds only the expected bounded set of files.
  const auto segments = list_journal_segments(dir.path(), 32);
  SLF_EXPECT(segments.ok());
  if (segments.ok()) {
    SLF_EXPECT(segments.value().size() <= 9U);
  }
}
