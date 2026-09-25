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

// Adversarial input: hostile framing, malformed specifications, extreme values,
// oversized counts, replay, and cancellation boundaries. Every case must
// produce a typed failure - never a crash, a hang, or a success.

#include <string>
#include <vector>

#include "fixtures.hpp"
#include "slf/service.hpp"
#include "slf/spec.hpp"
#include "slf/transport.hpp"
#include "slf_test.hpp"

using namespace slf;
using namespace slf::test;

SLF_TEST(adversarial_hostile_framing) {
  Rng rng(slf_ctx.seed ^ 0xF00DULL);
  net::TransportLimits limits;
  std::vector<std::byte> header(net::FrameHeader::kHeaderBytes);
  for (std::uint32_t round = 0; round < 20000; ++round) {
    for (auto& byte : header) {
      byte = static_cast<std::byte>(rng.next() & 0xFFU);
    }
    const auto decoded = net::decode_frame_header(header, limits);
    if (decoded.ok()) {
      // Only a fully well-formed header may decode, and then only if the magic,
      // the version, and the payload bound all hold.
      SLF_EXPECT_EQ(decoded.value().magic, net::FrameHeader::kMagic);
      SLF_EXPECT_EQ(decoded.value().version, kProtocolVersion);
      SLF_EXPECT(decoded.value().payload_len <= limits.max_frame_payload);
    }
  }
  // A well-formed header with a corrupted payload digest is refused.
  const std::vector<std::byte> payload{std::byte{1}, std::byte{2}, std::byte{3}};
  net::FrameHeader frame;
  frame.type = 1;
  const auto encoded = net::encode_frame(frame, payload, limits);
  SLF_EXPECT(encoded.ok());
  if (encoded.ok()) {
    std::vector<std::byte> corrupted = encoded.value();
    corrupted.back() = static_cast<std::byte>(static_cast<unsigned>(corrupted.back()) ^ 0x01U);
    SLF_EXPECT_CODE(net::verify_frame(std::span<const std::byte>(corrupted.data(), net::FrameHeader::kHeaderBytes),
                                      std::span<const std::byte>(corrupted.data() + net::FrameHeader::kHeaderBytes,
                                                                 corrupted.size() - net::FrameHeader::kHeaderBytes)),
                    StatusCode::IntegrityError);
  }
  // An oversized declared payload is refused before allocation.
  const auto well_formed = net::encode_frame(frame, payload, limits);
  SLF_EXPECT(well_formed.ok());
  std::vector<std::byte> huge(well_formed.value().begin(),
                              well_formed.value().begin() + net::FrameHeader::kHeaderBytes);
  // Declare a payload that is far larger than the configured bound.
  for (unsigned i = 0; i < 4; ++i) {
    huge[12 + i] = static_cast<std::byte>((0x7FFFFFFFU >> (8U * i)) & 0xFFU);
  }
  SLF_EXPECT_CODE(net::decode_frame_header(huge, limits), StatusCode::Exhausted);
  // A garbled request payload is refused with a typed code.
  std::vector<std::byte> garbage(64, std::byte{0xAB});
  SLF_EXPECT(!net::decode_request(garbage, limits).ok());
  SLF_EXPECT(!net::decode_response(garbage, limits).ok());
}

SLF_TEST(adversarial_malformed_specifications) {
  Rng rng(slf_ctx.seed ^ 0xBEEFULL);
  const std::vector<std::string> hostile = {
      "",
      "   ",
      "# only a comment",
      "leaf id=1 name=\"a\" role=tor domain=1 access_mbps=1 role_incarnation=1 generation=1",
      "fabric id=0 name=\"zero\" generation=1",
      "fabric id=1 name=\"\" generation=1",
      "fabric id=1 name=\"ok\" generation=0",
      "fabric id=1 name=\"ok\" generation=1\nleaf id=0 name=\"a\" role=tor",
      "fabric id=1 name=\"ok\" generation=1\nleaf id=1 name=\"a\" role=spine",
      "fabric id=1 name=\"ok\" generation=1\nspine id=1 name=\"a\" role=tor",
      "fabric id=1 name=\"ok\" generation=1\noption unknown_key=true",
      "fabric id=1 name=\"ok\" generation=1\nleaf id=1 name=\"a\" role=tor generation=2",
      "fabric id=1 name=\"ok\" generation=1\nleaf id=1 name=\"a\" role=tor generation=1 name=\"b\"",
      "fabric id=1 name=\"ok\" generation=1\nunknown-directive id=1",
      "fabric id=1 name=\"unterminated generation=1",
      "fabric id=1 name=\"bad\\q escape\" generation=1",
      "fabric id=99999999999999999999999 name=\"big\" generation=1",
      "fabric id=1 name=\"ok\" generation=1\nport id=1 owner=nope:1 speed_mbps=1 generation=1",
      "fabric id=1 name=\"ok\" generation=1\nlink id=1 a=leaf:1 pa=1 b=spine:1 pb=2 generation=1",
  };
  for (const auto& text : hostile) {
    ValidationReport report;
    const auto parsed = parse_spec(text, SpecLimits{}, &report);
    SLF_EXPECT(!parsed.ok());
  }
  // Random byte soup must never parse and never crash.
  Rng byte_rng(slf_ctx.seed ^ 0x5A5AULL);
  for (std::uint32_t round = 0; round < 400; ++round) {
    std::string text;
    const std::size_t length = 1 + (byte_rng.below(160));
    for (std::size_t i = 0; i < length; ++i) {
      text.push_back(static_cast<char>(byte_rng.next() & 0xFFU));
    }
    const auto parsed = parse_spec(text);
    SLF_EXPECT(!parsed.ok());
  }
  // Over-long input and over-long lines are refused by bound, not by accident.
  SpecLimits tiny;
  tiny.max_file_bytes = 64;
  tiny.max_line_bytes = 16;
  SLF_EXPECT_CODE(parse_spec("fabric id=1 name=\"ok\" generation=1\n", tiny), StatusCode::Exhausted);
  SpecLimits narrow;
  narrow.max_line_bytes = 8;
  SLF_EXPECT_CODE(parse_spec("fabric id=1 name=\"ok\" generation=1\n", narrow), StatusCode::Exhausted);
  // Invalid UTF-8 inside the text is refused.
  std::string invalid_utf8 = "fabric id=1 name=\"";
  invalid_utf8.push_back(static_cast<char>(0xFF));
  invalid_utf8 += "\" generation=1\n";
  SLF_EXPECT_CODE(parse_spec(invalid_utf8), StatusCode::Invalid);
  (void)rng;
}

SLF_TEST(adversarial_extreme_values_and_bounds) {
  // Capacity above the configured ceiling is refused with Overflow.
  FabricBuilder builder(FabricId{1}, "extreme", TopologyGeneration{1}, Epoch{1}, FabricOptions{},
                        BuilderLimits{});
  LeafRecord leaf = leaf_record(LeafSpec{1, "", LeafRole::Tor, 1, NodeState::Active, UINT64_MAX, 1},
                                TopologyGeneration{1});
  SLF_EXPECT_CODE(builder.add_leaf(leaf), StatusCode::Overflow);

  // A zero-speed enabled port is refused rather than treated as a usable link.
  FabricBuilder second(FabricId{1}, "extreme", TopologyGeneration{1}, Epoch{1});
  SLF_EXPECT(second.add_leaf(leaf_record(LeafSpec{1, "", LeafRole::Tor, 1}, TopologyGeneration{1})).ok());
  PortRecord zero_speed;
  zero_speed.id = PortId{1};
  zero_speed.owner = NodeKey{LeafId{1}};
  zero_speed.speed_mbps = 0;
  zero_speed.generation = TopologyGeneration{1};
  SLF_EXPECT_CODE(second.add_port(zero_speed), StatusCode::Invalid);
  PortRecord disabled_zero_speed = zero_speed;
  disabled_zero_speed.admin = PortAdmin::Disabled;
  SLF_EXPECT(second.add_port(disabled_zero_speed).ok());

  // Deeply nested or duplicated identities are refused.
  SLF_EXPECT_CODE(second.add_leaf(leaf_record(LeafSpec{1, "", LeafRole::Tor, 1}, TopologyGeneration{1})),
                  StatusCode::DuplicateIdentity);

  // Cancellation is a distinct outcome on every query surface.
  const auto fabric = make_two_by_two();
  SLF_EXPECT(fabric.ok());
  const auto index = TopologyIndex::build(fabric.value());
  SLF_EXPECT(index.ok());
  EligibilityEngine engine(index.value());
  const auto constraints = PathConstraints::defaults_for(fabric.value().options);
  const CancellationToken cancelled = CancellationToken::pre_cancelled();
  SLF_EXPECT_CODE(engine.eligible_spines_for_leaf(LeafId{1}, constraints, cancelled),
                  StatusCode::Cancelled);
  SLF_EXPECT_CODE(engine.health(constraints, ControllerIncarnation{}, Epoch{1}, cancelled),
                  StatusCode::Cancelled);
}

SLF_TEST(adversarial_runtime_refuses_rather_than_guesses) {
  TempDir dir{"adversarial-runtime"};
  RuntimeConfig config;
  config.fabric_id = FabricId{1};
  config.name = "adversarial";
  config.state_dir = dir.path();
  config.worker_threads = 0;
  config.max_dedup_entries = 4;
  ControllerRuntime runtime(config);
  SLF_EXPECT(runtime.start().ok());
  const auto fabric = make_two_by_two();
  SLF_EXPECT(fabric.ok());
  SLF_EXPECT(runtime.install_fabric(fabric.value(), true).ok());
  const auto lease = runtime.acquire_authority("adversarial");
  SLF_EXPECT(lease.ok());

  // Commands with no request id, an unminted token, a foreign lease, and a
  // wrong coordinate are each refused with their own code, and none of them
  // changes the topology.
  const TopologyGeneration generation = runtime.status().generation;
  Command no_request;
  no_request.token = lease.value().token;
  no_request.expected_generation = generation;
  no_request.expected_epoch = runtime.status().epoch;
  SLF_EXPECT_EQ(runtime.apply(no_request).code, StatusCode::Invalid);
  Command unminted;
  unminted.request_id = RequestId{1};
  unminted.expected_generation = generation;
  unminted.expected_epoch = runtime.status().epoch;
  SLF_EXPECT_EQ(runtime.apply(unminted).code, StatusCode::Refused);
  Command foreign;
  foreign.request_id = RequestId{2};
  foreign.token = lease.value().token;
  foreign.token.lease = LeaseId{999};
  foreign.expected_generation = generation;
  foreign.expected_epoch = runtime.status().epoch;
  SLF_EXPECT_EQ(runtime.apply(foreign).code, StatusCode::Refused);
  SLF_EXPECT_EQ(runtime.status().generation, generation);

  // The exactly-once window is bounded and refuses rather than forgetting.
  runtime.set_fault_point(FaultPoint::None);
  AuthorityToken current_token = lease.value().token;
  for (std::uint32_t i = 0; i < 4; ++i) {
    const auto context = runtime.status();
    Command command;
    command.request_id = RequestId{100 + i};
    command.token = current_token;
    command.expected_generation = context.generation;
    command.expected_epoch = context.epoch;
    command.kind = CommandKind::SetFabricState;
    command.fabric_state = FabricState::Operational;
    const CommandResult applied = runtime.apply(command);
    SLF_EXPECT_EQ(applied.code, StatusCode::Ok);
    current_token = applied.refreshed_token;
  }
  const auto context = runtime.status();
  Command overflow;
  overflow.request_id = RequestId{900};
  overflow.token = current_token;
  overflow.expected_generation = context.generation;
  overflow.expected_epoch = context.epoch;
  overflow.kind = CommandKind::SetFabricState;
  SLF_EXPECT_EQ(runtime.apply(overflow).code, StatusCode::Exhausted);
  SLF_EXPECT(runtime.stop().ok());
}
