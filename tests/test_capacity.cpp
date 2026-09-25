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

#include "slf/checked.hpp"
#include "slf_test.hpp"

using namespace slf;

SLF_TEST(capacity_rational_reduces_and_compares) {
  const auto half = Rational::make(2, 4);
  SLF_EXPECT(half.ok());
  SLF_EXPECT_EQ(half.value().num, 1U);
  SLF_EXPECT_EQ(half.value().den, 2U);
  SLF_EXPECT_EQ(half.value().to_string(), std::string("1/2"));
  SLF_EXPECT_EQ(half.value().to_decimal(2), std::string("0.50"));

  const auto zero = Rational::make(0, 7);
  SLF_EXPECT(zero.ok());
  SLF_EXPECT(zero.value().is_zero());
  SLF_EXPECT_EQ(zero.value().den, 1U);

  const auto invalid = Rational::make(1, 0);
  SLF_EXPECT_CODE(invalid, StatusCode::Invalid);

  const auto one = Rational::make(5, 5);
  SLF_EXPECT(one.value().is_one());

  SLF_EXPECT_EQ(Rational::compare(Rational{1, 2}, Rational{1, 3}).value(), 1);
  SLF_EXPECT_EQ(Rational::compare(Rational{1, 3}, Rational{1, 2}).value(), -1);
  SLF_EXPECT_EQ(Rational::compare(Rational{2, 4}, Rational{1, 2}).value(), 0);
  // Cross multiplication overflow is reported, never wrapped.
  SLF_EXPECT_CODE(Rational::compare(Rational{UINT64_MAX, 2}, Rational{UINT64_MAX - 1, 3}),
                  StatusCode::Overflow);
  SLF_EXPECT_CODE(Rational::compare(Rational{1, 0}, Rational{1, 1}), StatusCode::Invalid);
}

SLF_TEST(capacity_aggregation_and_classification) {
  CapacitySummary summary;
  summary.access_capacity_mbps = 200000;
  SLF_EXPECT(capacity_add_uplink(summary, 100000));
  SLF_EXPECT(capacity_add_uplink(summary, 50000));
  SLF_EXPECT_EQ(summary.uplink_capacity_mbps, 150000U);
  SLF_EXPECT_EQ(summary.uplink_count, 2U);
  SLF_EXPECT_EQ(summary.min_uplink_mbps, 50000U);
  SLF_EXPECT_EQ(summary.max_uplink_mbps, 100000U);
  capacity_finalize(summary);
  SLF_EXPECT_EQ(summary.classification, OversubscriptionClass::Oversubscribed);
  SLF_EXPECT_EQ(summary.oversubscription.to_string(), std::string("4/3"));
  SLF_EXPECT_EQ(summary.code, StatusCode::Ok);
  SLF_EXPECT(summary.to_string().find("oversubscribed") != std::string::npos);

  // At or below line rate is subscribed, not oversubscribed.
  CapacitySummary subscribed;
  subscribed.access_capacity_mbps = 100000;
  SLF_EXPECT(capacity_add_uplink(subscribed, 100000));
  capacity_finalize(subscribed);
  SLF_EXPECT_EQ(subscribed.classification, OversubscriptionClass::Subscribed);
  SLF_EXPECT(subscribed.oversubscription.is_one());

  // No uplinks at all is unprovisioned, and still a failure to decide when
  // access capacity exists.
  CapacitySummary none;
  none.access_capacity_mbps = 1000;
  capacity_finalize(none);
  SLF_EXPECT_EQ(none.classification, OversubscriptionClass::Unprovisioned);
  SLF_EXPECT_EQ(none.code, StatusCode::Incomplete);

  // Overflow is detected rather than wrapped.
  CapacitySummary overflow;
  SLF_EXPECT(capacity_add_uplink(overflow, UINT64_MAX));
  SLF_EXPECT(!capacity_add_uplink(overflow, 1));
  SLF_EXPECT_EQ(overflow.code, StatusCode::Overflow);
  SLF_EXPECT_EQ(overflow.uplink_capacity_mbps, UINT64_MAX);

  CapacitySummary downlinks;
  SLF_EXPECT(capacity_add_downlink(downlinks, 1000));
  SLF_EXPECT(capacity_add_downlink(downlinks, 2000));
  SLF_EXPECT_EQ(downlinks.downlink_capacity_mbps, 3000U);
  SLF_EXPECT_EQ(downlinks.downlink_count, 2U);
}

SLF_TEST(capacity_checked_arithmetic_helpers) {
  std::uint64_t out = 0;
  SLF_EXPECT(checked_add(1, 2, out));
  SLF_EXPECT_EQ(out, 3U);
  SLF_EXPECT(!checked_add(UINT64_MAX, 1, out));
  SLF_EXPECT(checked_sub(5, 3, out));
  SLF_EXPECT_EQ(out, 2U);
  SLF_EXPECT(!checked_sub(3, 5, out));
  SLF_EXPECT(checked_mul(6, 7, out));
  SLF_EXPECT_EQ(out, 42U);
  SLF_EXPECT(!checked_mul(UINT64_MAX, 2, out));
  SLF_EXPECT(checked_mul(0, UINT64_MAX, out));
  SLF_EXPECT_EQ(out, 0U);
  SLF_EXPECT(!checked_div(1, 0, out));
  SLF_EXPECT(checked_div(9, 3, out));
  SLF_EXPECT_EQ(out, 3U);
  std::uint32_t narrowed = 0;
  SLF_EXPECT(checked_narrow_u32(7, narrowed));
  SLF_EXPECT(!checked_narrow_u32(UINT64_MAX, narrowed));
  SLF_EXPECT(!checked_widen(-1, out));
  SLF_EXPECT(checked_widen(4, out));
  SLF_EXPECT_EQ(out, 4U);
  SLF_EXPECT_EQ(gcd_u64(12, 18), 6U);
  SLF_EXPECT_EQ(gcd_u64(0, 5), 5U);
  SLF_EXPECT_EQ(saturating_add(UINT64_MAX, 1), UINT64_MAX);
  SLF_EXPECT_EQ(saturating_mul(UINT64_MAX, 2), UINT64_MAX);
}
