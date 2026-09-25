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

#include "slf/transport.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "slf/bytes.hpp"
#include "slf/digest.hpp"
#include "slf/platform.hpp"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <cerrno>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace slf::net {
namespace {

#if defined(_WIN32)
using socket_t = SOCKET;
constexpr socket_t kInvalid = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalid = -1;
#endif

std::once_flag g_socket_once;
Status g_socket_status = Status::success();

void initialise_sockets() {
#if defined(_WIN32)
  WSADATA data{};
  const int result = ::WSAStartup(MAKEWORD(2, 2), &data);
  if (result != 0) {
    g_socket_status = Status(StatusCode::IoError, "WSAStartup failed with error " + std::to_string(result));
    return;
  }
  g_socket_status = Status::success();
#endif
}

/// True when the last socket error was a deadline expiry rather than a real
/// failure. Deadlines are reported distinctly so a stalled peer is never
/// mistaken for a protocol error or, worse, for success.
[[nodiscard]] bool last_error_was_timeout() noexcept {
#if defined(_WIN32)
  const int error = ::WSAGetLastError();
  return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT;
#endif
}

[[nodiscard]] Status socket_error(std::string_view what) {
  if (last_error_was_timeout()) {
    return Status(StatusCode::DeadlineExpired, std::string(what) + " exceeded the configured deadline");
  }
#if defined(_WIN32)
  return Status(StatusCode::IoError, std::string(what) + " failed with socket error " +
                                          std::to_string(::WSAGetLastError()));
#else
  return Status(StatusCode::IoError, std::string(what) + " failed: " + std::strerror(errno));
#endif
}

void close_socket(socket_t socket) noexcept {
  if (socket == kInvalid) {
    return;
  }
#if defined(_WIN32)
  ::closesocket(socket);
#else
  ::close(socket);
#endif
}

[[nodiscard]] Status set_timeouts(socket_t socket, std::uint64_t timeout_ms) {
  if (timeout_ms == 0) {
    return Status::success();
  }
#if defined(_WIN32)
  const DWORD timeout = timeout_ms > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<DWORD>(timeout_ms);
  if (::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                   sizeof(timeout)) != 0) {
    return socket_error("setsockopt(SO_RCVTIMEO)");
  }
  if (::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
                   sizeof(timeout)) != 0) {
    return socket_error("setsockopt(SO_SNDTIMEO)");
  }
#else
  timeval tv{};
  tv.tv_sec = static_cast<long>(timeout_ms / 1000U);
  tv.tv_usec = static_cast<long>((timeout_ms % 1000U) * 1000U);
  if (::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
    return socket_error("setsockopt(SO_RCVTIMEO)");
  }
  if (::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
    return socket_error("setsockopt(SO_SNDTIMEO)");
  }
#endif
  return Status::success();
}

[[nodiscard]] std::vector<std::byte> frame_header_bytes(const FrameHeader& header) {
  std::vector<std::byte> bytes(FrameHeader::kHeaderBytes, std::byte{0});
  ByteWriter writer;
  (void)writer.u32(header.magic);
  (void)writer.u16(header.version);
  (void)writer.u16(header.type);
  (void)writer.u32(header.flags);
  (void)writer.u32(header.payload_len);
  (void)writer.u64(header.request_id);
  (void)writer.u64(header.reserved);
  std::copy(writer.buffer().begin(), writer.buffer().end(), bytes.begin());
  return bytes;
}

}  // namespace

std::string Endpoint::to_string() const { return host + ":" + std::to_string(port); }

Outcome<Endpoint> Endpoint::parse(std::string_view text) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
    return Status(StatusCode::Invalid, "endpoint must be host:port");
  }
  const std::string_view host = text.substr(0, colon);
  const auto port = parse_u32(text.substr(colon + 1));
  if (!port.has_value() || *port == 0 || *port > 65535U) {
    return Status(StatusCode::Invalid, "endpoint port must be in 1..65535");
  }
  if (!is_valid_utf8(host) || host.size() > 255) {
    return Status(StatusCode::Invalid, "endpoint host is not valid");
  }
  Endpoint endpoint;
  endpoint.host = std::string(host);
  endpoint.port = static_cast<std::uint16_t>(*port);
  return endpoint;
}

Outcome<std::vector<std::byte>> encode_frame(const FrameHeader& header, std::span<const std::byte> payload,
                                             const TransportLimits& limits) {
  if (payload.size() > limits.max_frame_payload) {
    return Status(StatusCode::Exhausted, "frame payload exceeds the configured bound");
  }
  FrameHeader local = header;
  local.magic = FrameHeader::kMagic;
  local.payload_len = static_cast<std::uint32_t>(payload.size());
  std::vector<std::byte> bytes = frame_header_bytes(local);
  Sha256 sha;
  sha.update(std::span<const std::byte>(bytes.data(), FrameHeader::kHeaderBytes - FrameHeader::kDigestBytes));
  sha.update(payload);
  const Digest digest{sha.finish()};
  std::copy(digest.bytes.begin(), digest.bytes.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(FrameHeader::kHeaderBytes - FrameHeader::kDigestBytes));
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  return bytes;
}

Outcome<FrameHeader> decode_frame_header(std::span<const std::byte> header_bytes,
                                         const TransportLimits& limits) {
  if (header_bytes.size() != FrameHeader::kHeaderBytes) {
    return Status(StatusCode::Invalid, "frame header must be exactly 64 bytes");
  }
  ByteReader reader(header_bytes);
  const auto magic = reader.u32();
  const auto version = reader.u16();
  const auto type = reader.u16();
  const auto flags = reader.u32();
  const auto payload_len = reader.u32();
  const auto request_id = reader.u64();
  const auto reserved = reader.u64();
  if (!magic.ok() || !version.ok() || !type.ok() || !flags.ok() || !payload_len.ok() || !request_id.ok() ||
      !reserved.ok()) {
    return Status(StatusCode::Truncated, "frame header is incomplete");
  }
  if (magic.value() != FrameHeader::kMagic) {
    return Status(StatusCode::Corrupt, "frame magic does not match");
  }
  if (version.value() != kProtocolVersion) {
    return Status(StatusCode::IncompatibleVersion,
                  "frame protocol version " + std::to_string(version.value()) + " is not supported");
  }
  if (payload_len.value() > limits.max_frame_payload) {
    return Status(StatusCode::Exhausted, "frame declares a payload larger than the configured bound");
  }
  FrameHeader header;
  header.magic = magic.value();
  header.version = version.value();
  header.type = type.value();
  header.flags = flags.value();
  header.payload_len = payload_len.value();
  header.request_id = request_id.value();
  header.reserved = reserved.value();
  return header;
}

Status verify_frame(std::span<const std::byte> header_bytes, std::span<const std::byte> payload) {
  if (header_bytes.size() != FrameHeader::kHeaderBytes) {
    return Status(StatusCode::Invalid, "frame header must be exactly 64 bytes");
  }
  Digest stored;
  std::copy(header_bytes.begin() + static_cast<std::ptrdiff_t>(FrameHeader::kHeaderBytes -
                                                              FrameHeader::kDigestBytes),
            header_bytes.end(), stored.bytes.begin());
  Sha256 sha;
  sha.update(header_bytes.first(FrameHeader::kHeaderBytes - FrameHeader::kDigestBytes));
  sha.update(payload);
  const Digest computed{sha.finish()};
  if (!(computed == stored)) {
    return Status(StatusCode::IntegrityError, "frame digest does not match its contents");
  }
  return Status::success();
}

SocketRuntime::SocketRuntime() { std::call_once(g_socket_once, initialise_sockets); }

SocketRuntime::~SocketRuntime() = default;

Status SocketRuntime::ensure() {
  std::call_once(g_socket_once, initialise_sockets);
  return g_socket_status;
}

Connection::~Connection() { close(); }

Connection::Connection(Connection&& other) noexcept : socket_(other.socket_) {
  other.socket_ = kInvalidSocket;
}

Connection& Connection::operator=(Connection&& other) noexcept {
  if (this != &other) {
    close();
    socket_ = other.socket_;
    other.socket_ = kInvalidSocket;
  }
  return *this;
}

void Connection::close() noexcept {
  if (socket_ != kInvalidSocket) {
    close_socket(static_cast<socket_t>(socket_));
    socket_ = kInvalidSocket;
  }
}

void Connection::shutdown() noexcept {
  if (socket_ == kInvalidSocket) {
    return;
  }
#if defined(_WIN32)
  (void)::shutdown(static_cast<socket_t>(socket_), SD_BOTH);
#else
  (void)::shutdown(static_cast<socket_t>(socket_), SHUT_RDWR);
#endif
}

Status Connection::set_deadlines(std::uint64_t timeout_ms) {
  if (socket_ == kInvalidSocket) {
    return Status(StatusCode::Closed, "connection is closed");
  }
  return set_timeouts(static_cast<socket_t>(socket_), timeout_ms);
}

Status Connection::write_all(std::span<const std::byte> data) {
  if (socket_ == kInvalidSocket) {
    return Status(StatusCode::Closed, "connection is closed");
  }
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t remaining = data.size() - offset;
    const int chunk = static_cast<int>(remaining > 1U << 20 ? 1U << 20 : remaining);
    const int written = ::send(static_cast<socket_t>(socket_),
                               reinterpret_cast<const char*>(data.data() + offset), chunk, 0);
    if (written <= 0) {
      return socket_error("send");
    }
    offset += static_cast<std::size_t>(written);
  }
  return Status::success();
}

Status Connection::read_all(std::span<std::byte> out) {
  if (socket_ == kInvalidSocket) {
    return Status(StatusCode::Closed, "connection is closed");
  }
  std::size_t offset = 0;
  while (offset < out.size()) {
    const std::size_t remaining = out.size() - offset;
    const int chunk = static_cast<int>(remaining > 1U << 20 ? 1U << 20 : remaining);
    const int received = ::recv(static_cast<socket_t>(socket_), reinterpret_cast<char*>(out.data() + offset),
                                chunk, 0);
    if (received == 0) {
      return Status(StatusCode::Truncated, "peer closed the connection mid-frame");
    }
    if (received < 0) {
      return socket_error("recv");
    }
    offset += static_cast<std::size_t>(received);
  }
  return Status::success();
}

Status Connection::write_frame(const FrameHeader& header, std::span<const std::byte> payload,
                               const TransportLimits& limits) {
  const auto encoded = encode_frame(header, payload, limits);
  if (!encoded.ok()) {
    return encoded.status();
  }
  return write_all(encoded.value());
}

Outcome<std::vector<std::byte>> Connection::read_frame(FrameHeader* header_out,
                                                       const TransportLimits& limits) {
  std::vector<std::byte> header_bytes(FrameHeader::kHeaderBytes);
  Status status = read_all(header_bytes);
  if (!status.ok()) {
    return status;
  }
  const auto header = decode_frame_header(header_bytes, limits);
  if (!header.ok()) {
    return header.status();
  }
  std::vector<std::byte> payload(header.value().payload_len);
  if (!payload.empty()) {
    status = read_all(payload);
    if (!status.ok()) {
      return status;
    }
  }
  const Status verified = verify_frame(header_bytes, payload);
  if (!verified.ok()) {
    return verified;
  }
  if (header_out != nullptr) {
    *header_out = header.value();
  }
  return payload;
}

void shutdown_socket(std::intptr_t handle) noexcept {
  if (handle == Connection::kInvalidSocket) {
    return;
  }
#if defined(_WIN32)
  (void)::shutdown(static_cast<socket_t>(handle), SD_BOTH);
#else
  (void)::shutdown(static_cast<socket_t>(handle), SHUT_RDWR);
#endif
}

Outcome<Connection> Connection::connect(const Endpoint& endpoint, const TransportLimits& limits) {
  const Status ready = SocketRuntime::ensure();
  if (!ready.ok()) {
    return ready;
  }
  const socket_t socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalid) {
    return socket_error("socket");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(endpoint.port);
  if (endpoint.host.empty() || endpoint.host == "localhost") {
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  } else if (::inet_pton(AF_INET, endpoint.host.c_str(), &address.sin_addr) != 1) {
    close_socket(socket);
    return Status(StatusCode::Invalid, "client host must be an IPv4 literal or localhost");
  }
  if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    const Status status = socket_error("connect");
    close_socket(socket);
    return status;
  }
  Connection connection(static_cast<std::intptr_t>(socket));
  const Status deadlines = connection.set_deadlines(limits.io_timeout_ms);
  if (!deadlines.ok()) {
    connection.close();
    return deadlines;
  }
  return connection;
}

std::string Connection::peer() const {
  if (socket_ == kInvalidSocket) {
    return "<closed>";
  }
  sockaddr_storage storage{};
  socklen_t length = sizeof(storage);
  if (::getpeername(static_cast<socket_t>(socket_), reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
    return "<unknown>";
  }
  char buffer[64] = {0};
  if (storage.ss_family == AF_INET) {
    const auto* address = reinterpret_cast<const sockaddr_in*>(&storage);
    const char* text = ::inet_ntop(AF_INET, &address->sin_addr, buffer, sizeof(buffer));
    if (text == nullptr) {
      return "<unknown>";
    }
    return std::string(text) + ":" + std::to_string(ntohs(address->sin_port));
  }
  return "<unknown>";
}

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept : socket_(other.socket_), port_(other.port_) {
  other.socket_ = Connection::kInvalidSocket;
  other.port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
  if (this != &other) {
    close();
    socket_ = other.socket_;
    port_ = other.port_;
    other.socket_ = Connection::kInvalidSocket;
    other.port_ = 0;
  }
  return *this;
}

void TcpListener::close() noexcept {
  if (socket_ != Connection::kInvalidSocket) {
    close_socket(static_cast<socket_t>(socket_));
    socket_ = Connection::kInvalidSocket;
  }
}

Outcome<TcpListener> TcpListener::bind(const Endpoint& endpoint) {
  const Status ready = SocketRuntime::ensure();
  if (!ready.ok()) {
    return ready;
  }
  const socket_t socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalid) {
    return socket_error("socket");
  }
  int reuse = 1;
  (void)::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(endpoint.port);
  if (endpoint.host.empty() || endpoint.host == "localhost") {
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  } else if (::inet_pton(AF_INET, endpoint.host.c_str(), &address.sin_addr) != 1) {
    close_socket(socket);
    return Status(StatusCode::Invalid, "listener host must be an IPv4 literal or localhost");
  }
  if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    const Status status = socket_error("bind");
    close_socket(socket);
    return status;
  }
  if (::listen(socket, 16) != 0) {
    const Status status = socket_error("listen");
    close_socket(socket);
    return status;
  }
  sockaddr_in bound{};
  socklen_t bound_length = sizeof(bound);
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
    const Status status = socket_error("getsockname");
    close_socket(socket);
    return status;
  }
  TcpListener listener;
  listener.socket_ = static_cast<std::intptr_t>(socket);
  listener.port_ = ntohs(bound.sin_port);
  return listener;
}

Outcome<Connection> TcpListener::accept(std::uint64_t timeout_ms) {
  if (socket_ == Connection::kInvalidSocket) {
    return Status(StatusCode::Closed, "listener is closed");
  }
  fd_set read_set;
  FD_ZERO(&read_set);
  const socket_t listener_socket = static_cast<socket_t>(socket_);
  FD_SET(listener_socket, &read_set);
  timeval timeout{};
  timeout.tv_sec = static_cast<long>(timeout_ms / 1000U);
  timeout.tv_usec = static_cast<long>((timeout_ms % 1000U) * 1000U);
  const int ready = ::select(static_cast<int>(listener_socket) + 1, &read_set, nullptr, nullptr,
                             timeout_ms == 0 ? nullptr : &timeout);
  if (ready == 0) {
    return Status(StatusCode::DeadlineExpired, "no connection arrived before the accept deadline");
  }
  if (ready < 0) {
    return socket_error("select");
  }
  const socket_t accepted = ::accept(listener_socket, nullptr, nullptr);
  if (accepted == kInvalid) {
    return socket_error("accept");
  }
  return Connection(static_cast<std::intptr_t>(accepted));
}

}  // namespace slf::net
