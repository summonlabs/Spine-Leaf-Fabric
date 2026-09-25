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

#include "slf/bytes.hpp"

#include <limits>

#include "slf/identity.hpp"

namespace slf {

Status ByteWriter::require(std::size_t additional) const {
  std::size_t total = 0;
  if (additional > max_bytes_ || buffer_.size() > max_bytes_ - additional) {
    return Status(StatusCode::Exhausted, "byte writer bound exceeded");
  }
  total = buffer_.size() + additional;
  (void)total;
  return Status::success();
}

Status ByteWriter::u8(std::uint8_t value) {
  const Status s = require(1);
  if (!s.ok()) {
    return s;
  }
  buffer_.push_back(static_cast<std::byte>(value));
  return Status::success();
}

Status ByteWriter::u16(std::uint16_t value) {
  const Status s = require(2);
  if (!s.ok()) {
    return s;
  }
  buffer_.push_back(static_cast<std::byte>(value & 0xFFU));
  buffer_.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
  return Status::success();
}

Status ByteWriter::u32(std::uint32_t value) {
  const Status s = require(4);
  if (!s.ok()) {
    return s;
  }
  for (unsigned shift = 0; shift < 32; shift += 8) {
    buffer_.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
  return Status::success();
}

Status ByteWriter::u64(std::uint64_t value) {
  const Status s = require(8);
  if (!s.ok()) {
    return s;
  }
  for (unsigned shift = 0; shift < 64; shift += 8) {
    buffer_.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
  return Status::success();
}

Status ByteWriter::bytes(std::span<const std::byte> data) {
  const Status s = require(data.size());
  if (!s.ok()) {
    return s;
  }
  buffer_.insert(buffer_.end(), data.begin(), data.end());
  return Status::success();
}

Status ByteWriter::block(std::span<const std::byte> data) {
  if (data.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Status(StatusCode::Overflow, "block length does not fit in u32");
  }
  const Status status = u32(static_cast<std::uint32_t>(data.size()));
  if (!status.ok()) {
    return status;
  }
  return bytes(data);
}

Status ByteWriter::string(std::string_view text) {
  if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Status(StatusCode::Overflow, "string length does not fit in u32");
  }
  if (!is_valid_utf8(text)) {
    return Status(StatusCode::Invalid, "string is not valid UTF-8");
  }
  const auto length = static_cast<std::uint32_t>(text.size());
  Status s = u32(length);
  if (!s.ok()) {
    return s;
  }
  return bytes(std::as_bytes(std::span<const char>(text.data(), text.size())));
}

Outcome<std::uint8_t> ByteReader::u8() {
  if (remaining() < 1) {
    return Status(StatusCode::Truncated, "byte reader exhausted reading u8");
  }
  const auto value = static_cast<std::uint8_t>(data_[offset_]);
  ++offset_;
  return value;
}

Outcome<std::uint16_t> ByteReader::u16() {
  if (remaining() < 2) {
    return Status(StatusCode::Truncated, "byte reader exhausted reading u16");
  }
  std::uint16_t value = 0;
  value |= static_cast<std::uint16_t>(static_cast<std::uint8_t>(data_[offset_]));
  value |= static_cast<std::uint16_t>(static_cast<std::uint16_t>(
               static_cast<std::uint8_t>(data_[offset_ + 1]))
           << 8U);
  offset_ += 2;
  return value;
}

Outcome<std::uint32_t> ByteReader::u32() {
  if (remaining() < 4) {
    return Status(StatusCode::Truncated, "byte reader exhausted reading u32");
  }
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data_[offset_ + i])) << (8U * i);
  }
  offset_ += 4;
  return value;
}

Outcome<std::uint64_t> ByteReader::u64() {
  if (remaining() < 8) {
    return Status(StatusCode::Truncated, "byte reader exhausted reading u64");
  }
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(data_[offset_ + i])) << (8U * i);
  }
  offset_ += 8;
  return value;
}

Outcome<std::span<const std::byte>> ByteReader::bytes(std::size_t count) {
  if (count > remaining()) {
    return Status(StatusCode::Truncated, "byte reader exhausted reading byte block");
  }
  const std::span<const std::byte> out = data_.subspan(offset_, count);
  offset_ += count;
  return out;
}

Outcome<std::span<const std::byte>> ByteReader::block(std::size_t max_bytes) {
  const auto length = u32();
  if (!length.ok()) {
    return length.status();
  }
  const std::size_t size = static_cast<std::size_t>(length.value());
  if (size > max_bytes) {
    return Status(StatusCode::Exhausted, "declared block length exceeds the configured bound");
  }
  return bytes(size);
}

Outcome<std::string> ByteReader::string() {
  const auto length = u32();
  if (!length.ok()) {
    return length.status();
  }
  const std::size_t size = static_cast<std::size_t>(length.value());
  if (size > max_string_bytes_) {
    return Status(StatusCode::Exhausted, "declared string length exceeds the configured bound");
  }
  const auto raw = bytes(size);
  if (!raw.ok()) {
    return raw.status();
  }
  std::string text(reinterpret_cast<const char*>(raw.value().data()), raw.value().size());
  if (!is_valid_utf8(text)) {
    return Status(StatusCode::Invalid, "string is not valid UTF-8");
  }
  return text;
}

Status ByteReader::expect_end() const {
  if (!exhausted()) {
    return Status(StatusCode::Invalid, "trailing bytes after the expected structure");
  }
  return Status::success();
}

void append_u16(std::vector<std::byte>& out, std::uint16_t value) {
  out.push_back(static_cast<std::byte>(value & 0xFFU));
  out.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void append_u32(std::vector<std::byte>& out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void append_u64(std::vector<std::byte>& out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

}  // namespace slf
