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

#include "slf_test.hpp"

using namespace slf;

SLF_TEST(unit_identity_format_parse) {
  SLF_EXPECT_EQ(to_string(LeafId{7}), std::string("leaf:7"));
  SLF_EXPECT_EQ(to_string(SpineId{7}), std::string("spine:7"));
  SLF_EXPECT_EQ(to_string(PortId{3}), std::string("port:3"));
  SLF_EXPECT_EQ(to_string(LinkId{3}), std::string("link:3"));
  SLF_EXPECT_EQ(to_string(FailureDomainId{3}), std::string("domain:3"));
  SLF_EXPECT_EQ(to_string(NodeKey{LeafId{2}}), std::string("leaf:2"));
  SLF_EXPECT_EQ(to_string(NodeKey{SpineId{2}}), std::string("spine:2"));

  const auto node = parse_node_key("leaf:12");
  SLF_EXPECT(node.has_value());
  SLF_EXPECT_EQ(node->tier, Tier::Leaf);
  SLF_EXPECT_EQ(node->index, 12U);
  SLF_EXPECT_EQ(parse_node_key("spine:0")->tier, Tier::Spine);

  SLF_EXPECT(!parse_node_key("leaf:").has_value());
  SLF_EXPECT(!parse_node_key("leaf").has_value());
  SLF_EXPECT(!parse_node_key(":1").has_value());
  SLF_EXPECT(!parse_node_key("rack:1").has_value());
  SLF_EXPECT(!parse_node_key("leaf:1x").has_value());
  SLF_EXPECT(!parse_node_key("leaf:-1").has_value());
  SLF_EXPECT(!parse_node_key("leaf:4294967296").has_value());
  SLF_EXPECT(!parse_node_key("leaf:99999999999999999999999").has_value());
  SLF_EXPECT(parse_node_key("leaf:4294967295").has_value());

  SLF_EXPECT(!parse_u64("").has_value());
  SLF_EXPECT(!parse_u64("+1").has_value());
  SLF_EXPECT(!parse_u64(" 1").has_value());
  const auto max_u64 = parse_u64("18446744073709551615");
  SLF_EXPECT(max_u64.has_value());
  SLF_EXPECT_EQ(max_u64.value(), UINT64_MAX);
  SLF_EXPECT(!parse_u64("18446744073709551616").has_value());
}

SLF_TEST(unit_identity_utf8_validation) {
  SLF_EXPECT(is_valid_utf8("plain ascii"));
  SLF_EXPECT(is_valid_utf8(""));
  SLF_EXPECT(is_valid_utf8("caf\xC3\xA9"));
  SLF_EXPECT(is_valid_utf8("\xE2\x82\xAC"));
  SLF_EXPECT(is_valid_utf8("\xF0\x9F\x94\xA5"));
  SLF_EXPECT(is_valid_utf8("\x7F"));
  SLF_EXPECT(!is_valid_utf8("\xC0\x80"));
  SLF_EXPECT(!is_valid_utf8("\xE0\x80\xAF"));
  SLF_EXPECT(!is_valid_utf8("\xED\xA0\x80"));
  SLF_EXPECT(!is_valid_utf8("\xF5\x88\x80\x80"));
  SLF_EXPECT(!is_valid_utf8("\xE2\x82"));
  SLF_EXPECT(!is_valid_utf8("\x80"));
  SLF_EXPECT(!is_valid_utf8("\xFF"));
  SLF_EXPECT(!is_valid_utf8("ok\xC3"));
}

SLF_TEST(unit_identity_incarnation) {
  const ControllerIncarnation first = mint_incarnation();
  const ControllerIncarnation second = mint_incarnation();
  SLF_EXPECT_NE(first, second);
  SLF_EXPECT(!first.is_zero());
  const std::string text = to_string(first);
  SLF_EXPECT_EQ(text.size(), 32U);
  const auto parsed = parse_incarnation(text);
  SLF_EXPECT(parsed.has_value());
  SLF_EXPECT_EQ(*parsed, first);
  SLF_EXPECT(parse_incarnation("0x" + text).has_value());
  SLF_EXPECT(!parse_incarnation("").has_value());
  SLF_EXPECT(!parse_incarnation("zzzz").has_value());
  SLF_EXPECT(!parse_incarnation(text.substr(1)).has_value());
  SLF_EXPECT(!parse_incarnation(text + "0").has_value());
  SLF_EXPECT(parse_incarnation("00000000000000000000000000000000")->is_zero());
}

SLF_TEST(unit_identity_ordering_and_hash) {
  SLF_EXPECT(LeafId{1} < LeafId{2});
  SLF_EXPECT(LeafId{2} > LeafId{1});
  SLF_EXPECT(LeafId{1} != LeafId{2});
  SLF_EXPECT_EQ(LeafId{1}, LeafId{1});
  SLF_EXPECT(NodeKey{LeafId{5}} < NodeKey{SpineId{1}});
  SLF_EXPECT(NodeKey{SpineId{1}} != NodeKey{LeafId{1}});
  SLF_EXPECT_EQ(std::hash<LeafId>{}(LeafId{9}), std::hash<LeafId>{}(LeafId{9}));
  SLF_EXPECT_EQ(std::hash<NodeKey>{}(NodeKey{SpineId{4}}), std::hash<NodeKey>{}(NodeKey{SpineId{4}}));
  SLF_EXPECT(LeafId{0}.is_zero());
  SLF_EXPECT(!LeafId{1}.is_zero());
}
