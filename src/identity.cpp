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

#include "slf/identity.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "slf/platform.hpp"

namespace slf {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] int hex_value(char c) noexcept {
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
}

[[nodiscard]] std::string hex_u64(std::uint64_t value) {
  std::string out;
  out.reserve(16);
  for (int shift = 60; shift >= 0; shift -= 4) {
    out.push_back(kHexDigits[(value >> static_cast<unsigned>(shift)) & 0xFU]);
  }
  return out;
}

}  // namespace

std::string_view to_string(Tier tier) noexcept {
  return tier == Tier::Leaf ? "leaf" : "spine";
}

std::optional<Tier> parse_tier(std::string_view text) noexcept {
  if (text == "leaf") {
    return Tier::Leaf;
  }
  if (text == "spine") {
    return Tier::Spine;
  }
  return std::nullopt;
}

std::string to_string(ControllerIncarnation incarnation) {
  return hex_u64(incarnation.hi) + hex_u64(incarnation.lo);
}

std::optional<ControllerIncarnation> parse_incarnation(std::string_view text) noexcept {
  if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    text.remove_prefix(2);
  }
  if (text.size() != 32) {
    return std::nullopt;
  }
  ControllerIncarnation out;
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
  for (std::size_t i = 0; i < 16; ++i) {
    const int v = hex_value(text[i]);
    if (v < 0) {
      return std::nullopt;
    }
    hi = (hi << 4U) | static_cast<std::uint64_t>(v);
  }
  for (std::size_t i = 16; i < 32; ++i) {
    const int v = hex_value(text[i]);
    if (v < 0) {
      return std::nullopt;
    }
    lo = (lo << 4U) | static_cast<std::uint64_t>(v);
  }
  out.hi = hi;
  out.lo = lo;
  return out;
}

ControllerIncarnation mint_incarnation() noexcept {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t seq = counter.fetch_add(1, std::memory_order_relaxed);
  const std::uint64_t pid = platform::current_pid();
  const std::uint64_t now = platform::now_unix_ms();
  const std::uint64_t mono = platform::monotonic_ms();

  // hi: pid mixed with the wall clock so two processes started in the same
  // millisecond still differ. lo: monotonic clock mixed with a process-local
  // counter so two incarnations in one process never collide.
  std::uint64_t hi = pid * 0x9E3779B97F4A7C15ULL;
  hi ^= now + 0x165667B19E3779F9ULL + (hi << 6U) + (hi >> 2U);
  std::uint64_t lo = mono * 0xC2B2AE3D27D4EB4FULL;
  lo ^= seq * 0x27D4EB2F165667C5ULL + (lo << 7U) + (lo >> 3U);
  if (hi == 0 && lo == 0) {
    lo = 1;
  }
  return ControllerIncarnation{hi, lo};
}

std::string to_string(FabricId id) { return "fabric:" + std::to_string(id.value); }
std::string to_string(LeafId id) { return "leaf:" + std::to_string(id.value); }
std::string to_string(SpineId id) { return "spine:" + std::to_string(id.value); }
std::string to_string(PortId id) { return "port:" + std::to_string(id.value); }
std::string to_string(LinkId id) { return "link:" + std::to_string(id.value); }
std::string to_string(FailureDomainId id) { return "domain:" + std::to_string(id.value); }

std::string to_string(NodeKey key) {
  return key.tier == Tier::Leaf ? to_string(LeafId{key.index}) : to_string(SpineId{key.index});
}

std::optional<std::uint64_t> parse_u64(std::string_view text) noexcept {
  if (text.empty() || text.size() > 20) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10U) {
      return std::nullopt;
    }
    value = value * 10U + digit;
  }
  return value;
}

std::optional<std::uint32_t> parse_u32(std::string_view text) noexcept {
  const auto value = parse_u64(text);
  if (!value.has_value() || *value > UINT32_MAX) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(*value);
}

std::optional<NodeKey> parse_node_key(std::string_view text) noexcept {
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view tier_text = text.substr(0, colon);
  const std::string_view index_text = text.substr(colon + 1);
  const auto tier = parse_tier(tier_text);
  if (!tier.has_value()) {
    return std::nullopt;
  }
  const auto index = parse_u32(index_text);
  if (!index.has_value()) {
    return std::nullopt;
  }
  return NodeKey{*tier, *index};
}

std::optional<LeafId> parse_leaf_id(std::string_view text) noexcept {
  const auto key = parse_node_key(text);
  if (!key.has_value() || key->tier != Tier::Leaf) {
    return std::nullopt;
  }
  return LeafId{key->index};
}

std::optional<SpineId> parse_spine_id(std::string_view text) noexcept {
  const auto key = parse_node_key(text);
  if (!key.has_value() || key->tier != Tier::Spine) {
    return std::nullopt;
  }
  return SpineId{key->index};
}

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t i = 0;
  const std::size_t n = text.size();
  while (i < n) {
    const auto byte = static_cast<unsigned char>(text[i]);
    std::size_t extra = 0;
    std::uint32_t code_point = 0;
    if (byte < 0x80U) {
      ++i;
      continue;
    } else if ((byte & 0xE0U) == 0xC0U) {
      extra = 1;
      code_point = byte & 0x1FU;
    } else if ((byte & 0xF0U) == 0xE0U) {
      extra = 2;
      code_point = byte & 0x0FU;
    } else if ((byte & 0xF8U) == 0xF0U) {
      extra = 3;
      code_point = byte & 0x07U;
    } else {
      return false;
    }
    if (i + extra >= n) {
      return false;
    }
    for (std::size_t k = 1; k <= extra; ++k) {
      const auto cont = static_cast<unsigned char>(text[i + k]);
      if ((cont & 0xC0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (cont & 0x3FU);
    }
    // Reject overlong encodings, surrogates, and out-of-range code points.
    if (extra == 1 && code_point < 0x80U) {
      return false;
    }
    if (extra == 2 && code_point < 0x800U) {
      return false;
    }
    if (extra == 3 && code_point < 0x10000U) {
      return false;
    }
    if (code_point > 0x10FFFFU) {
      return false;
    }
    if (code_point >= 0xD800U && code_point <= 0xDFFFU) {
      return false;
    }
    i += extra + 1;
  }
  return true;
}

}  // namespace slf
