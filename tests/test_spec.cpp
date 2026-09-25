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

#include "slf/spec.hpp"

#include "fixtures.hpp"
#include "slf_test.hpp"

using namespace slf;
using namespace slf::test;

SLF_TEST(spec_roundtrip_preserves_identity) {
  const auto fabric = make_two_by_two(FabricOptions{.require_domain_diversity = true,
                                                    .min_eligible_spines = 2});
  SLF_EXPECT(fabric.ok());
  const std::string text = format_spec(fabric.value());
  SLF_EXPECT(!text.empty());
  ValidationReport report;
  const auto reparsed = parse_spec(text, SpecLimits{}, &report);
  SLF_EXPECT(reparsed.ok());
  if (!reparsed.ok()) {
    SLF_FAIL(reparsed.status().to_string());
    return;
  }
  SLF_EXPECT_EQ(reparsed.value().topology_digest, fabric.value().topology_digest);
  SLF_EXPECT_EQ(reparsed.value().evidence_digest, fabric.value().evidence_digest);
  SLF_EXPECT_EQ(reparsed.value().links.size(), fabric.value().links.size());
  SLF_EXPECT_EQ(reparsed.value().ports.size(), fabric.value().ports.size());
  SLF_EXPECT_EQ(reparsed.value().options.require_domain_diversity, true);
  SLF_EXPECT_EQ(reparsed.value().options.min_eligible_spines, 2U);
  SLF_EXPECT_EQ(format_spec(reparsed.value()), text);
}

SLF_TEST(spec_parses_full_feature_set) {
  const std::string text =
      "# a full specification\n"
      "fabric id=7 name=\"prod\\\"east\" site=\"dc-1\" generation=3 epoch=2 state=operational\n"
      "option allow_same_tier_adjacency=true require_domain_diversity=true min_eligible_spines=2 "
      "evidence_policy=require_fresh\n"
      "domain id=1 name=\"rack-a\" generation=3\n"
      "leaf id=1 name=\"leaf-a\" role=border domain=1 state=active access_mbps=400000 "
      "access_links=8 role_incarnation=11 generation=3\n"
      "leaf id=2 name=\"leaf-b\" role=border domain=1 state=maintenance access_mbps=200000 "
      "role_incarnation=12 generation=3\n"
      "spine id=1 name=\"spine-a\" role=fabric domain=1 state=active fabric_mbps=3200000 "
      "role_incarnation=21 generation=3\n"
      "port id=1 owner=leaf:1 speed_mbps=100000 admin=enabled generation=3\n"
      "port id=2 owner=leaf:2 speed_mbps=100000 admin=enabled generation=3\n"
      "port id=3 owner=spine:1 speed_mbps=100000 admin=enabled generation=3\n"
      "port id=4 owner=spine:1 speed_mbps=100000 admin=enabled generation=3\n"
      "port id=5 owner=leaf:1 speed_mbps=100000 admin=enabled generation=3\n"
      "port id=6 owner=leaf:2 speed_mbps=100000 admin=enabled generation=3\n"
      "link id=1 a=leaf:1 pa=1 b=spine:1 pb=3 class=leaf_spine admin=enabled generation=3\n"
      "link id=2 a=leaf:2 pa=2 b=spine:1 pb=4 class=leaf_spine admin=disabled generation=3\n"
      "link id=3 a=leaf:1 pa=5 b=leaf:2 pb=6 class=same_tier_peer admin=enabled speed_mbps=10000 generation=3\n"
      "history-link id=4 a=leaf:1 pa=1 b=spine:1 pb=3 generation=2\n"
      "evidence link=1 observed=up generation=3 observer=0123456789abcdef0123456789abcdef epoch=2 "
      "seq=9 at_ms=1234\n";
  ValidationReport report;
  const auto fabric = parse_spec(text, SpecLimits{}, &report);
  SLF_EXPECT(fabric.ok());
  if (!fabric.ok()) {
    SLF_FAIL(fabric.status().to_string());
    return;
  }
  SLF_EXPECT_EQ(fabric.value().id, FabricId{7});
  SLF_EXPECT_EQ(fabric.value().name, std::string("prod\"east"));
  SLF_EXPECT_EQ(fabric.value().site, std::string("dc-1"));
  SLF_EXPECT_EQ(fabric.value().generation, TopologyGeneration{3});
  SLF_EXPECT_EQ(fabric.value().epoch, Epoch{2});
  SLF_EXPECT_EQ(fabric.value().state, FabricState::Operational);
  SLF_EXPECT_EQ(fabric.value().leaves.size(), 2U);
  SLF_EXPECT_EQ(fabric.value().links.size(), 3U);
  SLF_EXPECT_EQ(fabric.value().history_links.size(), 1U);
  SLF_EXPECT_EQ(fabric.value().evidence.size(), 1U);
  SLF_EXPECT_EQ(fabric.value().leaves[1].state, NodeState::Maintenance);
  SLF_EXPECT_EQ(fabric.value().options.evidence_policy, EvidencePolicy::RequireFresh);
  SLF_EXPECT_EQ(report.error_count, 0U);
  // The canonical text round-trips through the same parser.
  const auto again = parse_spec(format_spec(fabric.value()));
  SLF_EXPECT(again.ok());
  if (again.ok()) {
    SLF_EXPECT_EQ(again.value().topology_digest, fabric.value().topology_digest);
  }
}

SLF_TEST(spec_reports_structural_failures_with_codes) {
  const std::string base = "fabric id=1 name=\"x\" generation=1\n";
  // A duplicate identity is refused while parsing.
  SLF_EXPECT_CODE(parse_spec(base + "leaf id=1 name=\"a\" role=tor domain=1 access_mbps=1 "
                                    "role_incarnation=1 generation=1\n"
                                    "leaf id=1 name=\"b\" role=tor domain=1 access_mbps=1 "
                                    "role_incarnation=1 generation=1\n"),
                  StatusCode::DuplicateIdentity);
  // A link to a node that does not exist is a dangling edge.
  SLF_EXPECT_CODE(parse_spec(base + "port id=1 owner=leaf:1 speed_mbps=1 generation=1\n"),
                  StatusCode::DanglingEdge);
  // A same-tier link while the policy is off is an illegal adjacency.
  SLF_EXPECT_CODE(parse_spec(base + "leaf id=1 name=\"a\" role=tor domain=1 access_mbps=1 "
                                    "role_incarnation=1 generation=1\n"
                                    "leaf id=2 name=\"b\" role=tor domain=1 access_mbps=1 "
                                    "role_incarnation=1 generation=1\n"
                                    "port id=1 owner=leaf:1 speed_mbps=1 generation=1\n"
                                    "port id=2 owner=leaf:2 speed_mbps=1 generation=1\n"
                                    "link id=1 a=leaf:1 pa=1 b=leaf:2 pb=2 class=same_tier_peer generation=1\n"),
                  StatusCode::IllegalAdjacency);
  // Cross-generation references are refused.
  SLF_EXPECT_CODE(parse_spec(base + "leaf id=1 name=\"a\" role=tor domain=1 access_mbps=1 "
                                    "role_incarnation=1 generation=2\n"),
                  StatusCode::CrossGeneration);
  // Contradictory roles are refused.
  SLF_EXPECT_CODE(parse_spec(base + "leaf id=1 name=\"a\" role=fabric domain=1 access_mbps=1 "
                                    "role_incarnation=1 generation=1\n"),
                  StatusCode::ContradictoryRole);
  // A file that cannot be read is reported as such.
  SLF_EXPECT_CODE(load_spec_file("does-not-exist.slf"), StatusCode::NotFound);
}
