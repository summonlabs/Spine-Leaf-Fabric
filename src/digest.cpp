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

#include "slf/digest.hpp"

#include <cstring>

namespace slf {
namespace {

constexpr std::uint32_t kRoundConstants[64] = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U,
    0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU,
    0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU,
    0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU, 0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
    0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU,
    0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU,
    0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U, 0x19A4C116U,
    0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U,
    0xC67178F2U};

constexpr std::uint32_t kInitialState[8] = {0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
                                            0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, unsigned bits) noexcept {
  return (value >> bits) | (value << (32U - bits));
}

[[nodiscard]] constexpr char hex_digit(unsigned value) noexcept {
  return static_cast<char>(value < 10U ? ('0' + value) : ('a' + (value - 10U)));
}

}  // namespace

void Sha256::reset() noexcept {
  for (std::size_t i = 0; i < 8; ++i) {
    state_[i] = kInitialState[i];
  }
  buffer_.fill(0);
  buffered_ = 0;
  total_bytes_ = 0;
}

void Sha256::transform(const std::uint8_t* block) noexcept {
  std::uint32_t w[64];
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24U) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16U) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8U) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];
  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
    const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::uint8_t byte) noexcept {
  buffer_[buffered_] = byte;
  ++buffered_;
  ++total_bytes_;
  if (buffered_ == 64) {
    transform(buffer_.data());
    buffered_ = 0;
  }
}

void Sha256::update(std::span<const std::byte> data) noexcept {
  update(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

void Sha256::update(std::string_view text) noexcept {
  const auto* raw = reinterpret_cast<const std::uint8_t*>(text.data());
  std::size_t remaining = text.size();
  std::size_t offset = 0;
  while (remaining > 0) {
    if (buffered_ == 0 && remaining >= 64) {
      transform(raw + offset);
      offset += 64;
      remaining -= 64;
      total_bytes_ += 64;
      continue;
    }
    const std::size_t take = (64 - buffered_) < remaining ? (64 - buffered_) : remaining;
    std::memcpy(buffer_.data() + buffered_, raw + offset, take);
    buffered_ += take;
    offset += take;
    remaining -= take;
    total_bytes_ += take;
    if (buffered_ == 64) {
      transform(buffer_.data());
      buffered_ = 0;
    }
  }
}

std::array<std::byte, Sha256::kDigestBytes> Sha256::finish() noexcept {
  const std::uint64_t bit_length = total_bytes_ * 8U;
  const std::uint8_t pad = 0x80U;
  update(pad);
  const std::uint8_t zero = 0x00U;
  while (buffered_ != 56) {
    update(zero);
  }
  std::uint8_t length_bytes[8];
  for (unsigned i = 0; i < 8; ++i) {
    length_bytes[i] = static_cast<std::uint8_t>((bit_length >> (56U - 8U * i)) & 0xFFU);
  }
  for (unsigned i = 0; i < 8; ++i) {
    update(length_bytes[i]);
  }
  std::array<std::byte, kDigestBytes> out{};
  for (std::size_t i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<std::byte>((state_[i] >> 24U) & 0xFFU);
    out[i * 4 + 1] = static_cast<std::byte>((state_[i] >> 16U) & 0xFFU);
    out[i * 4 + 2] = static_cast<std::byte>((state_[i] >> 8U) & 0xFFU);
    out[i * 4 + 3] = static_cast<std::byte>(state_[i] & 0xFFU);
  }
  return out;
}

std::array<std::byte, Sha256::kDigestBytes> Sha256::hash(std::span<const std::byte> data) noexcept {
  Sha256 sha;
  sha.update(data);
  return sha.finish();
}

std::array<std::byte, Sha256::kDigestBytes> Sha256::hash(std::string_view text) noexcept {
  Sha256 sha;
  sha.update(text);
  return sha.finish();
}

bool Digest::is_zero() const noexcept {
  for (const std::byte b : bytes) {
    if (b != std::byte{0}) {
      return false;
    }
  }
  return true;
}

std::string Digest::hex() const {
  std::string out;
  out.reserve(64);
  for (const std::byte b : bytes) {
    const auto value = static_cast<unsigned>(b);
    out.push_back(hex_digit((value >> 4U) & 0xFU));
    out.push_back(hex_digit(value & 0xFU));
  }
  return out;
}

std::string Digest::short_hex(std::size_t n) const {
  const std::size_t count = n > Sha256::kDigestBytes ? Sha256::kDigestBytes : n;
  std::string out;
  out.reserve(count * 2);
  for (std::size_t i = 0; i < count; ++i) {
    const auto value = static_cast<unsigned>(bytes[i]);
    out.push_back(hex_digit((value >> 4U) & 0xFU));
    out.push_back(hex_digit(value & 0xFU));
  }
  return out;
}

std::optional<Digest> parse_digest_hex(std::string_view text) noexcept {
  if (text.size() != 64) {
    return std::nullopt;
  }
  Digest out;
  for (std::size_t i = 0; i < 32; ++i) {
    const char hi_char = text[i * 2];
    const char lo_char = text[i * 2 + 1];
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') {
        return c - '0';
      }
      if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
      }
      if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
      }
      return -1;
    };
    const int hi = nibble(hi_char);
    const int lo = nibble(lo_char);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    out.bytes[i] = static_cast<std::byte>((static_cast<unsigned>(hi) << 4U) | static_cast<unsigned>(lo));
  }
  return out;
}

void DigestBuilder::add_u8(std::uint8_t value) noexcept { sha_.update(value); }

void DigestBuilder::add_u16(std::uint16_t value) noexcept {
  sha_.update(static_cast<std::uint8_t>(value & 0xFFU));
  sha_.update(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void DigestBuilder::add_u32(std::uint32_t value) noexcept {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    sha_.update(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
  }
}

void DigestBuilder::add_u64(std::uint64_t value) noexcept {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    sha_.update(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
  }
}

void DigestBuilder::add_bytes(std::span<const std::byte> data) noexcept {
  add_u32(static_cast<std::uint32_t>(data.size()));
  sha_.update(data);
}

void DigestBuilder::add_string(std::string_view text) noexcept {
  add_u32(static_cast<std::uint32_t>(text.size()));
  sha_.update(text);
}

Digest DigestBuilder::finish() noexcept {
  Digest out;
  out.bytes = sha_.finish();
  return out;
}

}  // namespace slf
