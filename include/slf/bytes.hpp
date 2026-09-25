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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "slf/status.hpp"

namespace slf {

/// Little-endian byte writer with an explicit capacity bound. Every write is
/// length-checked against the configured maximum, so an oversized count can
/// never cause a large allocation.
class ByteWriter {
 public:
  explicit ByteWriter(std::size_t max_bytes = kDefaultMaxBytes) : max_bytes_(max_bytes) {
    buffer_.reserve(max_bytes < 4096 ? max_bytes : 4096);
  }

  static constexpr std::size_t kDefaultMaxBytes = 64U * 1024U * 1024U;

  [[nodiscard]] Status u8(std::uint8_t value);
  [[nodiscard]] Status u16(std::uint16_t value);
  [[nodiscard]] Status u32(std::uint32_t value);
  [[nodiscard]] Status u64(std::uint64_t value);
  [[nodiscard]] Status bytes(std::span<const std::byte> data);
  /// Length-prefixed (u32) byte block. Pairs with \c ByteReader::block(); a
  /// raw \c bytes() write is only correct for fixed-size fields.
  [[nodiscard]] Status block(std::span<const std::byte> data);
  /// Length-prefixed (u32) string. Rejects strings longer than the bound and
  /// strings that are not valid UTF-8.
  [[nodiscard]] Status string(std::string_view text);

  [[nodiscard]] const std::vector<std::byte>& buffer() const noexcept { return buffer_; }
  [[nodiscard]] std::vector<std::byte> take() && { return std::move(buffer_); }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] std::size_t max_bytes() const noexcept { return max_bytes_; }

 private:
  [[nodiscard]] Status require(std::size_t additional) const;

  std::vector<std::byte> buffer_;
  std::size_t max_bytes_;
};

/// Little-endian byte reader. Every read validates that the declared length is
/// available before touching memory, and string reads validate the length
/// prefix against the remaining bytes and the configured maximum, which is what
/// makes hostile framing safe: a frame that claims 4 GiB of payload is refused
/// before anything is allocated.
class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> data, std::size_t max_string_bytes = kDefaultMaxString)
      : data_(data), max_string_bytes_(max_string_bytes) {}

  static constexpr std::size_t kDefaultMaxString = 1U * 1024U * 1024U;

  [[nodiscard]] Outcome<std::uint8_t> u8();
  [[nodiscard]] Outcome<std::uint16_t> u16();
  [[nodiscard]] Outcome<std::uint32_t> u32();
  [[nodiscard]] Outcome<std::uint64_t> u64();
  [[nodiscard]] Outcome<std::span<const std::byte>> bytes(std::size_t count);
  /// Length-prefixed (u32) byte block. p max_bytes is validated against the
  /// declared length before any data is exposed, so an oversized claim is
  /// refused without allocating.
  [[nodiscard]] Outcome<std::span<const std::byte>> block(std::size_t max_bytes);
  [[nodiscard]] Outcome<std::string> string();

  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] bool exhausted() const noexcept { return offset_ == data_.size(); }
  /// Fails when unread bytes remain. Used to reject trailing garbage.
  [[nodiscard]] Status expect_end() const;

 private:
  std::span<const std::byte> data_;
  std::size_t offset_{0};
  std::size_t max_string_bytes_;
};

/// Appends a little-endian integer to a raw byte vector (unbounded helper used
/// only for trusted, already size-checked inputs such as digests).
void append_u16(std::vector<std::byte>& out, std::uint16_t value);
void append_u32(std::vector<std::byte>& out, std::uint32_t value);
void append_u64(std::vector<std::byte>& out, std::uint64_t value);

}  // namespace slf
