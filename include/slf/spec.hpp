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
#include <filesystem>
#include <string>
#include <string_view>

#include "slf/model.hpp"

namespace slf {

/// Bounds applied while parsing a textual fabric specification. They are
/// checked before any allocation derived from the input.
struct SpecLimits {
  std::uint64_t max_file_bytes{16U * 1024U * 1024U};
  std::uint32_t max_line_bytes{8192};
  std::uint32_t max_lines{1000000};
  std::uint32_t max_name_bytes{256};
  BuilderLimits builder{};

  friend bool operator==(const SpecLimits&, const SpecLimits&) = default;
};

/// Parses the textual fabric specification.
///
/// Grammar (one directive per line, '#' starts a comment, blank lines ignored):
///
/// code
/// fabric id=<u64> name="<utf8>" [site="<utf8>"] generation=<u64> epoch=<u64>
///        [state=unformed|forming|operational|draining|retired]   (default operational)
/// option allow_same_tier_adjacency=<bool> require_domain_diversity=<bool>
///        min_eligible_spines=<u32> evidence_policy=config_only|require_fresh
/// domain id=<u32> name="<utf8>" generation=<u64>
/// leaf id=<u32> name="<utf8>" role=access|tor|border|service
///      domain=<u32> state=active|maintenance|draining|failed|retired
///      access_mbps=<u64> [access_links=<u32>] role_incarnation=<u64> generation=<u64>
/// spine id=<u32> name="<utf8>" role=fabric|super
///       domain=<u32> state=<state> fabric_mbps=<u64> role_incarnation=<u64> generation=<u64>
/// port id=<u32> owner=leaf:<u32>|spine:<u32> speed_mbps=<u64>
///      admin=enabled|disabled generation=<u64>
/// link id=<u32> a=<node> pa=<u32> b=<node> pb=<u32> class=leaf_spine|same_tier_peer
///      admin=enabled|disabled [speed_mbps=<u64>] generation=<u64>
/// history-link <same fields as link>
/// evidence link=<u32> observed=up|down|unknown generation=<u64>
///          observer=<32 hex digits> epoch=<u64> seq=<u64> [at_ms=<u64>]
/// endcode
///
/// The parser is strict: unknown directives and keys, duplicate keys, missing
/// required keys, out-of-range numbers, malformed quoting, invalid UTF-8, and
/// trailing garbage are all rejected with c StatusCode::Invalid.
[[nodiscard]] Outcome<Fabric> parse_spec(std::string_view text, const SpecLimits& limits = {},
                                         ValidationReport* report_out = nullptr);

[[nodiscard]] Outcome<Fabric> load_spec_file(const std::filesystem::path& path, const SpecLimits& limits = {},
                                             ValidationReport* report_out = nullptr);

/// Canonical textual rendering. c parse_spec(format_spec(fabric)) reproduces
/// the same fabric and the same digests.
[[nodiscard]] std::string format_spec(const Fabric& fabric);

[[nodiscard]] Status write_spec_file(const std::filesystem::path& path, const Fabric& fabric);

}  // namespace slf
