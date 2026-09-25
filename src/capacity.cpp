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

#include "slf/capacity.hpp"

#include <sstream>

#include "slf/checked.hpp"

namespace slf {

std::string_view to_string(OversubscriptionClass cls) noexcept {
  switch (cls) {
    case OversubscriptionClass::Unprovisioned: return "unprovisioned";
    case OversubscriptionClass::Subscribed: return "subscribed";
    case OversubscriptionClass::Oversubscribed: return "oversubscribed";
    case OversubscriptionClass::Indeterminate: return "indeterminate";
  }
  return "invalid";
}

Outcome<Rational> Rational::make(std::uint64_t num, std::uint64_t den) {
  if (den == 0) {
    return Status(StatusCode::Invalid, "rational with a zero denominator");
  }
  Rational out;
  if (num == 0) {
    out.num = 0;
    out.den = 1;
    return out;
  }
  const std::uint64_t divisor = gcd_u64(num, den);
  out.num = num / divisor;
  out.den = den / divisor;
  return out;
}

std::string Rational::to_string() const { return std::to_string(num) + "/" + std::to_string(den); }

std::string Rational::to_decimal(std::uint32_t digits) const {
  if (den == 0) {
    return "invalid";
  }
  if (digits > 9) {
    digits = 9;
  }
  const long double value = static_cast<long double>(num) / static_cast<long double>(den);
  std::ostringstream out;
  out.precision(digits);
  out << std::fixed << static_cast<double>(value);
  return out.str();
}

Outcome<int> Rational::compare(const Rational& a, const Rational& b) {
  if (a.den == 0 || b.den == 0) {
    return Status(StatusCode::Invalid, "comparison with a zero denominator");
  }
  std::uint64_t left = 0;
  std::uint64_t right = 0;
  if (!checked_mul(a.num, b.den, left) || !checked_mul(b.num, a.den, right)) {
    return Status(StatusCode::Overflow, "oversubscription comparison overflowed 64 bits");
  }
  if (left == right) {
    return 0;
  }
  return left < right ? -1 : 1;
}

bool capacity_add_uplink(CapacitySummary& summary, std::uint64_t speed_mbps) noexcept {
  std::uint64_t next = 0;
  if (!checked_add(summary.uplink_capacity_mbps, speed_mbps, next)) {
    summary.code = StatusCode::Overflow;
    summary.detail = "uplink capacity aggregate overflowed";
    return false;
  }
  summary.uplink_capacity_mbps = next;
  ++summary.uplink_count;
  if (summary.min_uplink_mbps == 0 || speed_mbps < summary.min_uplink_mbps) {
    summary.min_uplink_mbps = speed_mbps;
  }
  if (speed_mbps > summary.max_uplink_mbps) {
    summary.max_uplink_mbps = speed_mbps;
  }
  return true;
}

bool capacity_add_downlink(CapacitySummary& summary, std::uint64_t speed_mbps) noexcept {
  std::uint64_t next = 0;
  if (!checked_add(summary.downlink_capacity_mbps, speed_mbps, next)) {
    summary.code = StatusCode::Overflow;
    summary.detail = "downlink capacity aggregate overflowed";
    return false;
  }
  summary.downlink_capacity_mbps = next;
  ++summary.downlink_count;
  return true;
}

void capacity_finalize(CapacitySummary& summary) noexcept {
  if (summary.uplink_count == 0) {
    summary.oversubscription = Rational{0, 1};
    summary.classification = OversubscriptionClass::Unprovisioned;
    if (summary.code == StatusCode::Ok) {
      summary.code = summary.access_capacity_mbps > 0 ? StatusCode::Incomplete : StatusCode::Ok;
      if (summary.detail.empty()) {
        summary.detail = "no eligible uplinks";
      }
    }
    return;
  }
  const auto ratio = Rational::make(summary.access_capacity_mbps, summary.uplink_capacity_mbps);
  if (!ratio.ok()) {
    summary.code = ratio.code();
    summary.detail = ratio.status().detail().empty() ? "invalid oversubscription ratio"
                                                     : std::string(ratio.status().detail());
    summary.classification = OversubscriptionClass::Indeterminate;
    return;
  }
  summary.oversubscription = ratio.value();
  const auto ordering = Rational::compare(summary.oversubscription, Rational{1, 1});
  if (!ordering.ok()) {
    summary.code = ordering.code();
    summary.detail = "oversubscription comparison overflowed";
    summary.classification = OversubscriptionClass::Indeterminate;
    return;
  }
  summary.classification = ordering.value() <= 0 ? OversubscriptionClass::Subscribed
                                                 : OversubscriptionClass::Oversubscribed;
}

std::string CapacitySummary::to_string() const {
  std::ostringstream out;
  out << "uplinks=" << uplink_count << " uplink_mbps=" << uplink_capacity_mbps << " access_mbps="
      << access_capacity_mbps << " oversubscription=" << oversubscription.to_string() << " ("
      << oversubscription.to_decimal(3) << ") class=" << slf::to_string(classification)
      << " code=" << slf::to_string(code);
  if (!detail.empty()) {
    out << " detail=\"" << detail << "\"";
  }
  return out.str();
}

}  // namespace slf
