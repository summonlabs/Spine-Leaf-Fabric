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

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "slf/eligibility.hpp"
#include "slf/governance.hpp"
#include "slf/runtime.hpp"
#include "slf/transport.hpp"

namespace slf::net {

enum class RequestKind : std::uint16_t {
  Hello = 1,
  Status = 2,
  Paths = 3,
  EligibleSpines = 4,
  Health = 5,
  Command = 6,
  AuthorizePath = 7,
  ValidateAuthority = 8,
  Compact = 9,
  Lease = 10,
  Ping = 11,
  Bye = 12,
};

[[nodiscard]] std::string_view to_string(RequestKind kind) noexcept;
[[nodiscard]] std::optional<RequestKind> parse_request_kind(std::string_view text) noexcept;

/// A request. All bounded fields are validated on decode.
struct Request {
  RequestId request_id{};
  RequestKind kind{RequestKind::Ping};
  std::uint16_t protocol_version{kProtocolVersion};
  std::string client_name;

  NodeKey from{};
  NodeKey to{};
  PathConstraints constraints{};

  Command command{};
  PathAuthority authority{};
  AuthorityToken token{};
  std::uint64_t ttl_ms{0};
};

/// A response. Exactly one of the optional payloads is meaningful for a given
/// request kind; the rest stay default constructed.
struct Response {
  RequestId request_id{};
  StatusCode code{StatusCode::Ok};
  std::string detail;

  std::uint16_t protocol_version{kProtocolVersion};
  ControllerIncarnation controller{};
  Epoch epoch{};
  TopologyGeneration generation{};
  Digest topology_digest{};
  std::uint32_t max_frame_payload{0};
  std::string server_name;
  std::uint64_t server_uptime_ms{0};

  RuntimeStatus status{};
  PathAssessment assessment{};
  std::vector<SpineEligibility> spines{};
  FabricHealthReport health{};
  CommandResult command_result{};
  PathAuthority path_authority{};
  AuthorityCheck authority_check{};
  /// Minted lease, returned for a Lease request. The token is the caller's
  /// authority for the coordinate it names; it is fenced by any restart.
  AuthorityToken lease_token{};
  std::string lease_requester;
};

/// Payload codecs. Bounds are enforced before allocation, so a hostile peer
/// cannot force a large allocation with a small frame.
[[nodiscard]] std::vector<std::byte> encode_request(const Request& request);
[[nodiscard]] Outcome<Request> decode_request(std::span<const std::byte> payload, const TransportLimits& limits);
[[nodiscard]] std::vector<std::byte> encode_response(const Response& response);
[[nodiscard]] Outcome<Response> decode_response(std::span<const std::byte> payload, const TransportLimits& limits);

/// Request handler interface used by FabricServer.
class RequestHandler {
 public:
  virtual ~RequestHandler() = default;
  [[nodiscard]] virtual Response handle(const Request& request) = 0;
};

/// Translates requests into runtime calls. Stateless apart from the runtime
/// reference, so it is safe to call from many connection threads at once. It is
/// itself a RequestHandler, which is what the CLI and the benchmarks serve.
class RuntimeService final : public RequestHandler {
 public:
  explicit RuntimeService(ControllerRuntime& runtime) : runtime_(&runtime) {}

  [[nodiscard]] Response handle(const Request& request) override;

 private:
  ControllerRuntime* runtime_;
};

/// Counters exposed by the server for tests and status reporting.
struct ServerStats {
  std::uint64_t accepted_connections{0};
  std::uint64_t rejected_connections{0};
  std::uint64_t frames_read{0};
  std::uint64_t frames_written{0};
  std::uint64_t protocol_errors{0};
  std::uint64_t handler_errors{0};
  std::uint64_t bytes_in{0};
  std::uint64_t bytes_out{0};
  std::uint32_t active_connections{0};
};

struct ServerConfig {
  Endpoint endpoint{};
  TransportLimits limits{};
  std::string server_name{"slf-controller"};
  bool trace{false};
};

/// Multi-threaded framed server over loopback TCP.
///
/// Ownership: the handler is owned by the caller and must outlive the server.
/// c stop() closes the listener, wakes and joins every connection thread, and
/// returns only after all of them have finished - it never holds a state lock
/// while joining.
class FabricServer {
 public:
  FabricServer() = default;
  ~FabricServer();
  FabricServer(const FabricServer&) = delete;
  FabricServer& operator=(const FabricServer&) = delete;

  [[nodiscard]] static Outcome<std::unique_ptr<FabricServer>> create(ServerConfig config, RequestHandler* handler);

  /// Binds and starts the accept thread. Returns as soon as the socket is
  /// listening, so tests can connect deterministically.
  [[nodiscard]] Status start();

  /// Blocks until c stop() is called from another thread.
  [[nodiscard]] Status run_until_stopped();

  /// Stops accepting, closes the listener, and joins all connection threads.
  [[nodiscard]] Status stop();

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] ServerStats stats() const;
  [[nodiscard]] bool running() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::uint16_t port_{0};
};

struct ClientConfig {
  Endpoint endpoint{};
  TransportLimits limits{};
  std::string client_name{"slf-client"};
};

/// Framed client. Retries only ever resend the identical request (same request
/// id), which is what makes a retry after a crash-before-ack safe.
class FabricClient {
 public:
  FabricClient() = default;
  ~FabricClient();
  FabricClient(const FabricClient&) = delete;
  FabricClient& operator=(const FabricClient&) = delete;

  [[nodiscard]] static Outcome<std::unique_ptr<FabricClient>> connect(const ClientConfig& config);

  [[nodiscard]] Outcome<Response> call(const Request& request, std::uint64_t timeout_ms = 0);
  [[nodiscard]] Status close();
  [[nodiscard]] bool connected() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Helper used by tests and tooling: connects, performs the hello handshake and
/// verifies protocol compatibility.
[[nodiscard]] Outcome<Response> handshake(FabricClient& client, std::string_view client_name);

}  // namespace slf::net
