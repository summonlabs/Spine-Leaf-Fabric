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

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slf {

/// SHA-256, implemented in-tree so the runtime has no cryptographic
/// dependency. Used for integrity checking (journal records, snapshots) and for
/// canonical topology digests. This is an integrity mechanism, not a signature:
/// it detects corruption and accidental divergence, and is not claimed to
/// resist a deliberate collision attack by a hostile writer.
class Sha256 {
 public:
  static constexpr std::size_t kDigestBytes = 32;

  Sha256() noexcept { reset(); }

  void reset() noexcept;
  void update(std::span<const std::byte> data) noexcept;
  void update(std::string_view text) noexcept;
  void update(std::uint8_t byte) noexcept;

  /// Finalizes and returns the digest. Subsequent updates require c reset().
  [[nodiscard]] std::array<std::byte, kDigestBytes> finish() noexcept;

  [[nodiscard]] static std::array<std::byte, kDigestBytes> hash(std::span<const std::byte> data) noexcept;
  [[nodiscard]] static std::array<std::byte, kDigestBytes> hash(std::string_view text) noexcept;

 private:
  void transform(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_{0};
  std::uint64_t total_bytes_{0};
};

/// A 32-byte digest value with stable formatting and total ordering, so digests
/// can be compared, sorted, and stored in maps.
struct Digest {
  std::array<std::byte, Sha256::kDigestBytes> bytes{};

  [[nodiscard]] static Digest of(std::span<const std::byte> data) noexcept {
    Digest digest;
    digest.bytes = Sha256::hash(data);
    return digest;
  }
  [[nodiscard]] static Digest of(std::string_view text) noexcept {
    Digest digest;
    digest.bytes = Sha256::hash(text);
    return digest;
  }
  [[nodiscard]] bool is_zero() const noexcept;
  [[nodiscard]] std::string hex() const;
  /// First p n bytes as hex; used for compact log lines. p n is clamped to 32.
  [[nodiscard]] std::string short_hex(std::size_t n = 8) const;

  friend bool operator==(const Digest& a, const Digest& b) noexcept {
    for (std::size_t i = 0; i < Sha256::kDigestBytes; ++i) {
      if (a.bytes[i] != b.bytes[i]) {
        return false;
      }
    }
    return true;
  }
  friend bool operator!=(const Digest& a, const Digest& b) noexcept { return !(a == b); }
  friend bool operator<(const Digest& a, const Digest& b) noexcept {
    for (std::size_t i = 0; i < Sha256::kDigestBytes; ++i) {
      const auto av = static_cast<unsigned>(a.bytes[i]);
      const auto bv = static_cast<unsigned>(b.bytes[i]);
      if (av != bv) {
        return av < bv;
      }
    }
    return false;
  }
};

[[nodiscard]] std::optional<Digest> parse_digest_hex(std::string_view text) noexcept;

/// Incremental digest builder over canonical byte streams.
class DigestBuilder {
 public:
  void add_u8(std::uint8_t value) noexcept;
  void add_u16(std::uint16_t value) noexcept;
  void add_u32(std::uint32_t value) noexcept;
  void add_u64(std::uint64_t value) noexcept;
  /// Length-prefixed byte string: the 4-byte length participates in the digest,
  /// so concatenation ambiguity is impossible.
  void add_bytes(std::span<const std::byte> data) noexcept;
  void add_string(std::string_view text) noexcept;
  [[nodiscard]] Digest finish() noexcept;

 private:
  Sha256 sha_;
};

}  // namespace slf
