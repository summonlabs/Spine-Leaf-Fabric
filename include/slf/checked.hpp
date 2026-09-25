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
#include <limits>
#include <type_traits>

namespace slf {

/// Checked arithmetic. Every count, size, offset, and capacity computation in
/// this runtime goes through these helpers: overflowing arithmetic must produce
/// a failure status, not a wrapped value.

[[nodiscard]] constexpr bool checked_add(std::uint64_t a, std::uint64_t b,
                                         std::uint64_t& out) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return false;
  }
  out = a + b;
  return true;
}

[[nodiscard]] constexpr bool checked_sub(std::uint64_t a, std::uint64_t b,
                                         std::uint64_t& out) noexcept {
  if (b > a) {
    return false;
  }
  out = a - b;
  return true;
}

[[nodiscard]] constexpr bool checked_mul(std::uint64_t a, std::uint64_t b,
                                         std::uint64_t& out) noexcept {
  if (a == 0 || b == 0) {
    out = 0;
    return true;
  }
  if (a > std::numeric_limits<std::uint64_t>::max() / b) {
    return false;
  }
  out = a * b;
  return true;
}

[[nodiscard]] constexpr bool checked_div(std::uint64_t a, std::uint64_t b,
                                         std::uint64_t& out) noexcept {
  if (b == 0) {
    return false;
  }
  out = a / b;
  return true;
}

/// Widens to c std::uint64_t and rejects negative input.
[[nodiscard]] constexpr bool checked_widen(std::int64_t value, std::uint64_t& out) noexcept {
  if (value < 0) {
    return false;
  }
  out = static_cast<std::uint64_t>(value);
  return true;
}

/// Narrows a c std::uint64_t into c std::uint32_t, rejecting values that do
/// not fit. Used at every external-input boundary.
[[nodiscard]] constexpr bool checked_narrow_u32(std::uint64_t value, std::uint32_t& out) noexcept {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

[[nodiscard]] constexpr bool checked_narrow_u16(std::uint64_t value, std::uint16_t& out) noexcept {
  if (value > std::numeric_limits<std::uint16_t>::max()) {
    return false;
  }
  out = static_cast<std::uint16_t>(value);
  return true;
}

[[nodiscard]] constexpr bool checked_narrow_u8(std::uint64_t value, std::uint8_t& out) noexcept {
  if (value > std::numeric_limits<std::uint8_t>::max()) {
    return false;
  }
  out = static_cast<std::uint8_t>(value);
  return true;
}

/// Sum of a range with overflow detection.
template <class It>
[[nodiscard]] constexpr bool checked_sum(It first, It last, std::uint64_t& out) noexcept {
  std::uint64_t acc = 0;
  for (It it = first; it != last; ++it) {
    const std::uint64_t term = static_cast<std::uint64_t>(*it);
    if (!checked_add(acc, term, acc)) {
      return false;
    }
  }
  out = acc;
  return true;
}

/// Greatest common divisor for c std::uint64_t (Euclid, no recursion).
[[nodiscard]] constexpr std::uint64_t gcd_u64(std::uint64_t a, std::uint64_t b) noexcept {
  while (b != 0) {
    const std::uint64_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

/// Saturating multiplication used only for statistics, never for decisions.
[[nodiscard]] constexpr std::uint64_t saturating_mul(std::uint64_t a, std::uint64_t b) noexcept {
  std::uint64_t out = 0;
  if (!checked_mul(a, b, out)) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return out;
}

[[nodiscard]] constexpr std::uint64_t saturating_add(std::uint64_t a, std::uint64_t b) noexcept {
  std::uint64_t out = 0;
  if (!checked_add(a, b, out)) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return out;
}

}  // namespace slf
