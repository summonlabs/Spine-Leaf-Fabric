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

#include <cstdint>
#include <string>
#include <vector>

#include "slf/status.hpp"

namespace slf {

/// Exact non-negative rational number. Capacity ratios (oversubscription) are
/// never represented in binary floating point, so comparisons are exact and
/// reproducible across platforms.
struct Rational {
  std::uint64_t num{0};
  std::uint64_t den{1};

  /// Reduces the fraction. Fails with c StatusCode::Invalid when p den is 0.
  [[nodiscard]] static Outcome<Rational> make(std::uint64_t num, std::uint64_t den);

  [[nodiscard]] bool is_zero() const noexcept { return num == 0; }
  /// True when the value is exactly 1/1.
  [[nodiscard]] bool is_one() const noexcept { return num == den && den != 0; }
  [[nodiscard]] std::string to_string() const;
  /// Display-only decimal rendering with p digits fraction digits. Never used
  /// for decisions.
  [[nodiscard]] std::string to_decimal(std::uint32_t digits = 3) const;

  /// Exact comparison. Returns -1, 0, or 1. Fails with c StatusCode::Overflow
  /// when the cross multiplication does not fit in 64 bits.
  [[nodiscard]] static Outcome<int> compare(const Rational& a, const Rational& b);
};

/// Classification of the downlink/uplink relationship of a leaf.
enum class OversubscriptionClass : std::uint8_t {
  /// No uplink capacity and no access capacity: nothing to decide.
  Unprovisioned = 0,
  /// Access capacity is at or below the uplink capacity (ratio <= 1).
  Subscribed = 1,
  /// Access capacity exceeds the uplink capacity (ratio > 1).
  Oversubscribed = 2,
  /// Uplink capacity is unknown because required evidence is missing/stale.
  Indeterminate = 3,
};

[[nodiscard]] std::string_view to_string(OversubscriptionClass cls) noexcept;

/// Capacity accounting for one leaf (or for a leaf pair). All sums are checked:
/// an overflowing aggregate is reported as c StatusCode::Overflow and the
/// partially computed value is not reported as a usable capacity.
struct CapacitySummary {
  /// Sum of the effective speeds of the counted uplinks.
  std::uint64_t uplink_capacity_mbps{0};
  std::uint32_t uplink_count{0};
  /// Sum of the effective speeds of the counted downlinks (peer links).
  std::uint64_t downlink_capacity_mbps{0};
  std::uint32_t downlink_count{0};
  /// Host-facing access capacity declared by the leaf.
  std::uint64_t access_capacity_mbps{0};
  /// Smallest and largest counted uplink speed.
  std::uint64_t min_uplink_mbps{0};
  std::uint64_t max_uplink_mbps{0};
  /// Sum of fabric capacity of the eligible spines.
  std::uint64_t spine_capacity_mbps{0};
  std::uint32_t spine_count{0};
  /// access / uplink, reduced. Zero numerator when there is no access capacity.
  Rational oversubscription{};
  OversubscriptionClass classification{OversubscriptionClass::Unprovisioned};
  /// c StatusCode::Ok when the accounting is complete; otherwise the first
  /// problem encountered (Overflow, Incomplete, Indeterminate).
  StatusCode code{StatusCode::Ok};
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return code == StatusCode::Ok; }
  [[nodiscard]] std::string to_string() const;
};

/// Adds one uplink capacity term with overflow checking. Returns c false when
/// the addition would overflow.
[[nodiscard]] bool capacity_add_uplink(CapacitySummary& summary, std::uint64_t speed_mbps) noexcept;
[[nodiscard]] bool capacity_add_downlink(CapacitySummary& summary, std::uint64_t speed_mbps) noexcept;

/// Finalizes counts, min/max, the ratio, and the classification.
void capacity_finalize(CapacitySummary& summary) noexcept;

}  // namespace slf
