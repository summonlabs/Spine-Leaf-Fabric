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
#include <span>
#include <string>
#include <vector>

#include "slf/model.hpp"
#include "slf/status.hpp"

namespace slf::net {

/// Wire frame layout: 64 byte header, then the payload.
/// code
/// 0  magic        u32  ('SLFF')
/// 4  version      u16
/// 6  type         u16
/// 8  flags        u32
/// 12 payload_len  u32
/// 16 request_id   u64
/// 24 reserved     u64
/// 32 digest       32 bytes = SHA-256(header[0..31] || payload)
/// 64 payload
/// endcode
struct FrameHeader {
  static constexpr std::uint32_t kMagic = 0x534C4646U;  // 'SLFF'
  static constexpr std::size_t kHeaderBytes = 64;
  static constexpr std::size_t kDigestBytes = 32;
  static constexpr std::uint32_t kFlagReply = 0x00000001U;

  std::uint32_t magic{kMagic};
  std::uint16_t version{kProtocolVersion};
  std::uint16_t type{0};
  std::uint32_t flags{0};
  std::uint32_t payload_len{0};
  std::uint64_t request_id{0};
  std::uint64_t reserved{0};
};

/// Frame-level bounds. Every limit is enforced before allocation or queueing.
struct TransportLimits {
  /// Maximum payload accepted in a single frame.
  std::uint32_t max_frame_payload{4U * 1024U * 1024U};
  /// Maximum simultaneous connections the server accepts.
  std::uint32_t max_connections{32};
  /// Per-connection in-flight request bound.
  std::uint32_t max_in_flight{64};
  /// Socket receive/send deadline.
  std::uint64_t io_timeout_ms{5000};
  /// Maximum client retries for an idempotent request.
  std::uint32_t max_retries{2};
  /// Maximum frames served per connection before the server closes it.
  std::uint32_t max_frames_per_connection{100000};

  friend bool operator==(const TransportLimits&, const TransportLimits&) = default;
};

struct Endpoint {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};

  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] static Outcome<Endpoint> parse(std::string_view text);
};

/// Encodes a frame. Fails with c StatusCode::Exhausted when the payload
/// exceeds the configured limit or c StatusCode::Overflow on size overflow.
[[nodiscard]] Outcome<std::vector<std::byte>> encode_frame(const FrameHeader& header,
                                                           std::span<const std::byte> payload,
                                                           const TransportLimits& limits);

/// Decodes a frame header from exactly c FrameHeader::kHeaderBytes bytes.
[[nodiscard]] Outcome<FrameHeader> decode_frame_header(std::span<const std::byte> header_bytes,
                                                       const TransportLimits& limits);

/// Verifies the digest of a complete frame (header plus payload).
[[nodiscard]] Status verify_frame(std::span<const std::byte> header_bytes,
                                  std::span<const std::byte> payload);

/// A connected socket with deadline-bounded, length-checked IO. All reads and
/// writes loop over partial transfers and fail with c StatusCode::Truncated,
/// c StatusCode::DeadlineExpired, or c StatusCode::IoError - never with a
/// silently short read.
class Connection {
 public:
  Connection() = default;
  ~Connection();
  Connection(Connection&& other) noexcept;
  Connection& operator=(Connection&& other) noexcept;
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  [[nodiscard]] bool valid() const noexcept { return socket_ != kInvalidSocket; }
  void close() noexcept;

  /// Half-closes the connection for both directions without releasing the
  /// handle. Used by the server to unblock a receive that is waiting on a
  /// silent peer before the owning thread is joined.
  void shutdown() noexcept;

  /// Applies receive and send deadlines. A deadline expiry is reported as
  /// \c StatusCode::DeadlineExpired, which is always a failure.
  [[nodiscard]] Status set_deadlines(std::uint64_t timeout_ms);

  [[nodiscard]] std::intptr_t native_handle() const noexcept { return socket_; }

  [[nodiscard]] Status write_all(std::span<const std::byte> data);
  [[nodiscard]] Status read_all(std::span<std::byte> out);

  /// Sends a complete frame.
  [[nodiscard]] Status write_frame(const FrameHeader& header, std::span<const std::byte> payload,
                                   const TransportLimits& limits);
  /// Reads one frame. The payload is bounded by p limits before allocation.
  [[nodiscard]] Outcome<std::vector<std::byte>> read_frame(FrameHeader* header_out,
                                                           const TransportLimits& limits);

  [[nodiscard]] std::string peer() const;

  /// Connects to \p endpoint and applies the configured deadlines.
  [[nodiscard]] static Outcome<Connection> connect(const Endpoint& endpoint, const TransportLimits& limits);

  static constexpr std::intptr_t kInvalidSocket = -1;

 private:
  friend class TcpListener;
  friend class FabricClient;
  explicit Connection(std::intptr_t socket) noexcept : socket_(socket) {}

  std::intptr_t socket_{kInvalidSocket};
};

/// Loopback (or explicitly addressed) TCP listener.
class TcpListener {
 public:
  TcpListener() = default;
  ~TcpListener();
  TcpListener(TcpListener&& other) noexcept;
  TcpListener& operator=(TcpListener&& other) noexcept;
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  [[nodiscard]] static Outcome<TcpListener> bind(const Endpoint& endpoint);
  [[nodiscard]] Outcome<Connection> accept(std::uint64_t timeout_ms);
  void close() noexcept;
  [[nodiscard]] bool valid() const noexcept { return socket_ != Connection::kInvalidSocket; }
  /// Actual bound port (useful with port 0).
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

 private:
  std::intptr_t socket_{Connection::kInvalidSocket};
  std::uint16_t port_{0};
};

/// Shuts a socket down for both directions without closing the handle. Used by
/// the server to unblock a receive that is waiting on a silent peer before the
/// owning worker thread is joined. Safe to call with an invalid handle.
void shutdown_socket(std::intptr_t handle) noexcept;

/// Initialises and tears down the platform socket layer (WSAStartup).
class SocketRuntime {
 public:
  SocketRuntime();
  ~SocketRuntime();
  SocketRuntime(const SocketRuntime&) = delete;
  SocketRuntime& operator=(const SocketRuntime&) = delete;
  [[nodiscard]] static Status ensure();
};

}  // namespace slf::net
