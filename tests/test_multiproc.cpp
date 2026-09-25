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

// Real multiprocess proof over loopback TCP.
//
// The controller under test is the slfctl child process; the test speaks the
// framed protocol to it. The child is terminated with TerminateProcess (a hard
// kill: no destructors, no trailer) at material lifecycle boundaries, then
// restarted, and the test proves that
//   * a fresh incarnation and a bumped epoch fence the previous authority,
//   * a committed mutation survives the kill and is not applied twice when the
//     client retries the same request id (durable-commit ambiguity),
//   * a kill immediately after the durable commit but before the reply is
//     answered exactly once.
//
// Threads are not multiprocess proof; this test therefore never shares a
// runtime object with the controller: only bytes over a socket.

#include <filesystem>
#include <string>
#include <thread>

#include "fixtures.hpp"
#include "slf/platform.hpp"
#include "slf/service.hpp"
#include "slf/spec.hpp"
#include "slf_test.hpp"

using namespace slf;
using namespace slf::test;

namespace {

/// The child executable path is passed by CTest as the first extra argument.
[[nodiscard]] std::string controller_path() {
  const auto& arguments = extra_arguments();
  if (arguments.empty()) {
    return {};
  }
  return arguments.front();
}

struct ServeOptions {
  std::filesystem::path spec;
  std::filesystem::path state;
  std::uint16_t port{0};
  std::string fault{"none"};
};

[[nodiscard]] Outcome<platform::ChildProcess> spawn_controller(const ServeOptions& options) {
  platform::ProcessOptions process;
  process.executable = controller_path();
  process.arguments = {"serve",
                       "--spec",
                       options.spec.string(),
                       "--state",
                       options.state.string(),
                       "--port",
                       std::to_string(options.port),
                       "--bind",
                       "127.0.0.1",
                       "--fault",
                       options.fault,
                       "--workers",
                       "2"};
  return platform::ChildProcess::spawn(process);
}

/// Connects with a bounded number of attempts. Each attempt is short; if the
/// controller never accepts, the caller fails the test rather than hanging.
[[nodiscard]] Outcome<std::unique_ptr<net::FabricClient>> connect_bounded(std::uint16_t port,
                                                                         std::uint32_t attempts = 400) {
  net::ClientConfig config;
  config.endpoint.host = "127.0.0.1";
  config.endpoint.port = port;
  Status last = Status(StatusCode::NotStarted, "no attempt made");
  for (std::uint32_t attempt = 0; attempt < attempts; ++attempt) {
    auto client = net::FabricClient::connect(config);
    if (client.ok()) {
      return client;
    }
    last = client.status();
    platform::sleep_ms(25);
  }
  return last;
}

[[nodiscard]] std::uint16_t free_port() {
  // Bind port 0 and read back the assigned port, then close it. The window in
  // which another process could take the port is small and the child binds
  // immediately; a collision is reported as a failed connection, not as a hang.
  auto listener = net::TcpListener::bind(net::Endpoint{"127.0.0.1", 0});
  if (!listener.ok()) {
    return 0;
  }
  const std::uint16_t port = listener.value().port();
  listener.value().close();
  return port;
}

}  // namespace

SLF_TEST(multiproc_kill_restart_fences_authority) {
  if (controller_path().empty()) {
    SLF_FAIL("the slfctl path was not provided; run this test through CTest");
    return;
  }
  TempDir dir{"multiproc"};
  const auto fabric = make_two_by_two(FabricOptions{.min_eligible_spines = 1});
  SLF_EXPECT(fabric.ok());
  const std::filesystem::path spec_path = dir.path() / "fabric.slf";
  SLF_EXPECT(write_spec_file(spec_path, fabric.value()).ok());
  const std::filesystem::path state = dir.path() / "state";

  ServeOptions options;
  options.spec = spec_path;
  options.state = state;
  options.port = free_port();
  SLF_EXPECT(options.port != 0);

  // --- first incarnation -----------------------------------------------------
  auto child = spawn_controller(options);
  if (!child.ok()) {
    SLF_FAIL("cannot spawn slfctl: " + child.status().to_string());
    return;
  }
  auto client = connect_bounded(options.port);
  if (!client.ok()) {
    (void)child.value().kill();
    SLF_FAIL("cannot connect to the controller: " + client.status().to_string());
    return;
  }
  SLF_EXPECT(net::handshake(*client.value(), "multiproc").ok());

  net::Request lease_request;
  lease_request.kind = net::RequestKind::Lease;
  lease_request.request_id = RequestId{1};
  lease_request.ttl_ms = 30000;
  const auto lease = client.value()->call(lease_request);
  SLF_EXPECT(lease.ok());
  if (!lease.ok()) {
    (void)child.value().kill();
    return;
  }
  const AuthorityToken first_token = lease.value().lease_token;
  const ControllerIncarnation first_incarnation = lease.value().controller;
  SLF_EXPECT(!first_token.lease.is_zero());
  SLF_EXPECT(!first_incarnation.is_zero());
  const TopologyGeneration generation_before = lease.value().generation;
  const Epoch epoch_before = lease.value().epoch;

  // A governed mutation over the wire.
  net::Request command_request;
  command_request.kind = net::RequestKind::Command;
  command_request.request_id = RequestId{42};
  command_request.command.request_id = RequestId{42};
  command_request.command.token = first_token;
  command_request.command.expected_generation = generation_before;
  command_request.command.expected_epoch = epoch_before;
  command_request.command.kind = CommandKind::SetFabricState;
  command_request.command.fabric_state = FabricState::Operational;
  const auto committed = client.value()->call(command_request);
  SLF_EXPECT(committed.ok());
  SLF_EXPECT_EQ(committed.value().code, StatusCode::Ok);
  const TopologyGeneration generation_after = committed.value().command_result.generation;
  SLF_EXPECT_EQ(generation_after, TopologyGeneration{generation_before.value + 1});

  // Query the fabric over the wire before the kill.
  net::Request paths_request;
  paths_request.kind = net::RequestKind::Paths;
  paths_request.request_id = RequestId{43};
  paths_request.from = NodeKey{LeafId{1}};
  paths_request.to = NodeKey{LeafId{2}};
  const auto paths = client.value()->call(paths_request);
  SLF_EXPECT_EQ(paths.code(), StatusCode::Ok);
  SLF_EXPECT_EQ(paths.value().assessment.verdict, EligibilityVerdict::Eligible);

  // Hard kill: TerminateProcess, no destructors, no clean-shutdown trailer.
  SLF_EXPECT(child.value().kill().ok());
  const auto exit_code = child.value().wait(10000);
  SLF_EXPECT(exit_code.ok());
  (void)client.value()->close();

  // --- second incarnation ----------------------------------------------------
  auto second_child = spawn_controller(options);
  if (!second_child.ok()) {
    SLF_FAIL("cannot respawn slfctl: " + second_child.status().to_string());
    return;
  }
  auto second_client = connect_bounded(options.port);
  if (!second_client.ok()) {
    (void)second_child.value().kill();
    SLF_FAIL("cannot reconnect: " + second_client.status().to_string());
    return;
  }
  SLF_EXPECT(net::handshake(*second_client.value(), "multiproc").ok());
  net::Request status_request;
  status_request.kind = net::RequestKind::Status;
  status_request.request_id = RequestId{50};
  const auto status = second_client.value()->call(status_request);
  SLF_EXPECT(status.ok());
  if (!status.ok()) {
    (void)second_child.value().kill();
    return;
  }
  // A fresh incarnation and a bumped epoch: all previous authority is fenced.
  SLF_EXPECT_NE(status.value().controller, first_incarnation);
  SLF_EXPECT(status.value().epoch.value > epoch_before.value);
  // The committed mutation survived the hard kill.
  SLF_EXPECT_EQ(status.value().generation, generation_after);
  SLF_EXPECT(status.value().status.recovery.usable());
  SLF_EXPECT(status.value().status.recovery.records_valid > 0U);

  // The old token is refused: fenced by the incarnation change.
  net::Request fenced_request;
  fenced_request.kind = net::RequestKind::Command;
  fenced_request.request_id = RequestId{51};
  fenced_request.command.request_id = RequestId{51};
  fenced_request.command.token = first_token;
  fenced_request.command.expected_generation = generation_after;
  fenced_request.command.expected_epoch = status.value().epoch;
  fenced_request.command.kind = CommandKind::SetFabricState;
  fenced_request.command.fabric_state = FabricState::Draining;
  const auto fenced = second_client.value()->call(fenced_request);
  SLF_EXPECT(fenced.ok());
  SLF_EXPECT_EQ(fenced.value().code, StatusCode::Fenced);
  SLF_EXPECT_EQ(fenced.value().command_result.generation, generation_after);

  // Retrying the committed request id replays the recorded result: exactly once.
  net::Request retry;
  retry.kind = net::RequestKind::Lease;
  retry.request_id = RequestId{52};
  const auto fresh_lease = second_client.value()->call(retry);
  SLF_EXPECT(fresh_lease.ok());
  net::Request replay_request = command_request;
  replay_request.request_id = RequestId{53};
  replay_request.command.request_id = RequestId{42};
  replay_request.command.token = fresh_lease.value().lease_token;
  replay_request.command.expected_generation = generation_after;
  replay_request.command.expected_epoch = status.value().epoch;
  const auto replayed = second_client.value()->call(replay_request);
  SLF_EXPECT(replayed.ok());
  SLF_EXPECT_EQ(replayed.value().code, StatusCode::Ok);
  SLF_EXPECT(replayed.value().command_result.deduplicated);
  SLF_EXPECT_EQ(replayed.value().command_result.generation, generation_after);
  SLF_EXPECT_EQ(second_client.value()->call(status_request).value().generation, generation_after);

  SLF_EXPECT(second_child.value().kill().ok());
  SLF_EXPECT(second_child.value().wait(10000).ok());
  (void)second_client.value()->close();
}

SLF_TEST(multiproc_crash_after_commit_before_ack) {
  if (controller_path().empty()) {
    SLF_FAIL("the slfctl path was not provided; run this test through CTest");
    return;
  }
  TempDir dir{"multiproc-fault"};
  const auto fabric = make_two_by_two();
  SLF_EXPECT(fabric.ok());
  const std::filesystem::path spec_path = dir.path() / "fabric.slf";
  SLF_EXPECT(write_spec_file(spec_path, fabric.value()).ok());
  const std::filesystem::path state = dir.path() / "state";

  ServeOptions options;
  options.spec = spec_path;
  options.state = state;
  options.port = free_port();
  options.fault = "after_commit_before_ack";
  SLF_EXPECT(options.port != 0);

  auto child = spawn_controller(options);
  if (!child.ok()) {
    SLF_FAIL("cannot spawn slfctl: " + child.status().to_string());
    return;
  }
  auto client = connect_bounded(options.port);
  if (!client.ok()) {
    (void)child.value().kill();
    SLF_FAIL("cannot connect: " + client.status().to_string());
    return;
  }
  SLF_EXPECT(net::handshake(*client.value(), "multiproc-fault").ok());
  net::Request lease_request;
  lease_request.kind = net::RequestKind::Lease;
  lease_request.request_id = RequestId{1};
  const auto lease = client.value()->call(lease_request);
  SLF_EXPECT(lease.ok());
  if (!lease.ok()) {
    (void)child.value().kill();
    return;
  }

  // The commit reaches the journal and *then* the process dies without replying:
  // the client cannot know whether the mutation happened.
  net::Request ambiguous;
  ambiguous.kind = net::RequestKind::Command;
  ambiguous.request_id = RequestId{77};
  ambiguous.command.request_id = RequestId{77};
  ambiguous.command.token = lease.value().lease_token;
  ambiguous.command.expected_generation = lease.value().generation;
  ambiguous.command.expected_epoch = lease.value().epoch;
  ambiguous.command.kind = CommandKind::SetFabricState;
  ambiguous.command.fabric_state = FabricState::Draining;
  const auto reply = client.value()->call(ambiguous);
  SLF_EXPECT(!reply.ok());  // the socket dies instead of replying
  (void)client.value()->close();
  const auto exit_code = child.value().wait(10000);
  SLF_EXPECT(exit_code.ok());
  SLF_EXPECT_EQ(exit_code.value(), kFaultExitCode);

  // Restart with no fault injection and retry the identical request id.
  ServeOptions clean = options;
  clean.fault = "none";
  auto second_child = spawn_controller(clean);
  if (!second_child.ok()) {
    SLF_FAIL("cannot respawn: " + second_child.status().to_string());
    return;
  }
  auto second_client = connect_bounded(options.port);
  if (!second_client.ok()) {
    (void)second_child.value().kill();
    SLF_FAIL("cannot reconnect: " + second_client.status().to_string());
    return;
  }
  SLF_EXPECT(net::handshake(*second_client.value(), "multiproc-fault").ok());
  net::Request status_request;
  status_request.kind = net::RequestKind::Status;
  status_request.request_id = RequestId{80};
  const auto status = second_client.value()->call(status_request);
  SLF_EXPECT(status.ok());
  if (!status.ok()) {
    (void)second_child.value().kill();
    return;
  }
  // The mutation was durable before the crash: exactly one generation advance.
  const TopologyGeneration expected_generation{lease.value().generation.value + 1};
  SLF_EXPECT_EQ(status.value().generation, expected_generation);

  net::Request fresh_lease_request;
  fresh_lease_request.kind = net::RequestKind::Lease;
  fresh_lease_request.request_id = RequestId{81};
  const auto fresh_lease = second_client.value()->call(fresh_lease_request);
  SLF_EXPECT(fresh_lease.ok());
  net::Request retry = ambiguous;
  retry.command.token = fresh_lease.value().lease_token;
  retry.command.expected_generation = status.value().generation;
  retry.command.expected_epoch = status.value().epoch;
  const auto retried = second_client.value()->call(retry);
  SLF_EXPECT(retried.ok());
  SLF_EXPECT_EQ(retried.value().code, StatusCode::Ok);
  SLF_EXPECT(retried.value().command_result.deduplicated);
  SLF_EXPECT_EQ(retried.value().command_result.generation, expected_generation);
  SLF_EXPECT_EQ(second_client.value()->call(status_request).value().generation, expected_generation);

  SLF_EXPECT(second_child.value().kill().ok());
  SLF_EXPECT(second_child.value().wait(10000).ok());
  (void)second_client.value()->close();
}

SLF_TEST(multiproc_immediate_kill_recovers_conservatively) {
  if (controller_path().empty()) {
    SLF_FAIL("the slfctl path was not provided; run this test through CTest");
    return;
  }
  TempDir dir{"multiproc-early"};
  const auto fabric = make_two_by_two();
  SLF_EXPECT(fabric.ok());
  const std::filesystem::path spec_path = dir.path() / "fabric.slf";
  SLF_EXPECT(write_spec_file(spec_path, fabric.value()).ok());
  const std::filesystem::path state = dir.path() / "state";
  ServeOptions options;
  options.spec = spec_path;
  options.state = state;
  options.port = free_port();
  SLF_EXPECT(options.port != 0);

  // Kill the controller as soon as it is accepting: the state directory holds a
  // header-only segment with no clean-shutdown trailer.
  for (int round = 0; round < 3; ++round) {
    auto child = spawn_controller(options);
    if (!child.ok()) {
      SLF_FAIL("cannot spawn slfctl: " + child.status().to_string());
      return;
    }
    auto client = connect_bounded(options.port, 200);
    if (client.ok()) {
      (void)client.value()->close();
    }
    SLF_EXPECT(child.value().kill().ok());
    SLF_EXPECT(child.value().wait(10000).ok());
  }

  // The next start recovers conservatively: usable state, no invented evidence,
  // and a fresh incarnation.
  auto child = spawn_controller(options);
  if (!child.ok()) {
    SLF_FAIL("cannot respawn: " + child.status().to_string());
    return;
  }
  auto client = connect_bounded(options.port);
  if (!client.ok()) {
    (void)child.value().kill();
    SLF_FAIL("cannot reconnect: " + client.status().to_string());
    return;
  }
  SLF_EXPECT(net::handshake(*client.value(), "multiproc-early").ok());
  net::Request status_request;
  status_request.kind = net::RequestKind::Status;
  status_request.request_id = RequestId{1};
  const auto status = client.value()->call(status_request);
  SLF_EXPECT(status.ok());
  if (status.ok()) {
    SLF_EXPECT(status.value().status.recovery.usable());
    SLF_EXPECT(!status.value().status.recovery.requires_reobservation);
    SLF_EXPECT(!status.value().controller.is_zero());
    // The topology is present (it was written as the first durable record).
    SLF_EXPECT_EQ(status.value().status.leaf_count, 2U);
  }
  SLF_EXPECT(child.value().kill().ok());
  SLF_EXPECT(child.value().wait(10000).ok());
  (void)client.value()->close();
}
