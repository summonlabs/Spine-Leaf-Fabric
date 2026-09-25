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

#include "slf/governance.hpp"

#include "fixtures.hpp"
#include "slf/platform.hpp"
#include "slf/service.hpp"
#include "slf_test.hpp"

using namespace slf;

SLF_TEST(governance_token_validation_matrix) {
  const ControllerIncarnation controller = mint_incarnation();
  const Epoch epoch{4};
  const TopologyGeneration generation{7};
  const Digest digest = Digest::of("topology");
  AuthorityContext context;
  context.controller = controller;
  context.epoch = epoch;
  context.generation = generation;
  context.topology_digest = digest;
  context.active_lease = LeaseId{3};
  context.now_unix_ms = 1000;

  AuthorityToken token;
  token.controller = controller;
  token.epoch = epoch;
  token.generation = generation;
  token.topology_digest = digest;
  token.lease = LeaseId{3};
  token.issued_at_unix_ms = 900;
  token.expires_at_unix_ms = 2000;
  SLF_EXPECT(validate_token(token, context).ok());

  // Every coordinate is checked separately, and each failure keeps its own code.
  AuthorityToken fenced = token;
  fenced.controller = mint_incarnation();
  SLF_EXPECT_EQ(validate_token(fenced, context).code, StatusCode::Fenced);

  AuthorityToken stale_epoch = token;
  stale_epoch.epoch = Epoch{3};
  SLF_EXPECT_EQ(validate_token(stale_epoch, context).code, StatusCode::Stale);

  AuthorityToken stale_generation = token;
  stale_generation.generation = TopologyGeneration{6};
  SLF_EXPECT_EQ(validate_token(stale_generation, context).code, StatusCode::Stale);

  AuthorityToken drifted = token;
  drifted.topology_digest = Digest::of("other");
  SLF_EXPECT_EQ(validate_token(drifted, context).code, StatusCode::Conflicting);

  AuthorityToken other_lease = token;
  other_lease.lease = LeaseId{9};
  SLF_EXPECT_EQ(validate_token(other_lease, context).code, StatusCode::Refused);

  AuthorityToken unminted;
  SLF_EXPECT_EQ(validate_token(unminted, context).code, StatusCode::Refused);

  AuthorityContext expired_context = context;
  expired_context.now_unix_ms = 2000;
  SLF_EXPECT_EQ(validate_token(token, expired_context).code, StatusCode::DeadlineExpired);

  AuthorityContext no_lease = context;
  no_lease.active_lease.reset();
  SLF_EXPECT_EQ(validate_token(token, no_lease).code, StatusCode::Refused);
}

SLF_TEST(governance_path_authority_digest_and_validation) {
  const ControllerIncarnation controller = mint_incarnation();
  const Epoch epoch{2};
  const TopologyGeneration generation{5};
  const Digest digest = Digest::of("topology");

  PathAssessment assessment;
  assessment.from = NodeKey{LeafId{1}};
  assessment.to = NodeKey{LeafId{2}};
  assessment.verdict = EligibilityVerdict::Eligible;
  assessment.cls = ReachabilityClass::LeafLeafMultiSpine;
  assessment.generation = generation;
  assessment.epoch = epoch;
  assessment.controller = controller;
  assessment.topology_digest = digest;
  assessment.eligible_spines = {SpineId{1}, SpineId{2}};

  AuthorityToken token;
  token.controller = controller;
  token.epoch = epoch;
  token.generation = generation;
  token.topology_digest = digest;
  token.lease = LeaseId{11};
  token.issued_at_unix_ms = 500;
  token.expires_at_unix_ms = 5000;

  const auto authority = mint_path_authority(assessment, token, 1000, 2000);
  SLF_EXPECT(authority.ok());
  if (!authority.ok()) {
    return;
  }
  SLF_EXPECT_EQ(authority.value().spines.size(), 2U);
  SLF_EXPECT_EQ(authority.value().expires_at_unix_ms, 3000U);
  SLF_EXPECT_EQ(compute_path_authority_digest(authority.value()), authority.value().authority_digest);

  AuthorityContext context;
  context.controller = controller;
  context.epoch = epoch;
  context.generation = generation;
  context.topology_digest = digest;
  context.active_lease = LeaseId{11};
  context.now_unix_ms = 2000;
  SLF_EXPECT(validate_path_authority(authority.value(), context).ok());

  PathAuthority tampered = authority.value();
  tampered.spines.push_back(SpineId{9});
  SLF_EXPECT_EQ(validate_path_authority(tampered, context).code, StatusCode::IntegrityError);

  PathAuthority expired = authority.value();
  AuthorityContext late = context;
  late.now_unix_ms = 4000;
  SLF_EXPECT_EQ(validate_path_authority(expired, late).code, StatusCode::DeadlineExpired);

  AuthorityContext restarted = context;
  restarted.controller = mint_incarnation();
  SLF_EXPECT_EQ(validate_path_authority(authority.value(), restarted).code, StatusCode::Fenced);

  AuthorityContext newer_epoch = context;
  newer_epoch.epoch = Epoch{3};
  SLF_EXPECT_EQ(validate_path_authority(authority.value(), newer_epoch).code, StatusCode::Stale);

  // Refusing to mint authority for a path that is not proven eligible.
  PathAssessment indeterminate = assessment;
  indeterminate.verdict = EligibilityVerdict::Indeterminate;
  SLF_EXPECT_CODE(mint_path_authority(indeterminate, token, 0, 0), StatusCode::Indeterminate);
  PathAssessment empty = assessment;
  empty.eligible_spines.clear();
  SLF_EXPECT_CODE(mint_path_authority(empty, token, 0, 0), StatusCode::Indeterminate);
  PathAssessment elsewhere = assessment;
  elsewhere.generation = TopologyGeneration{99};
  SLF_EXPECT_CODE(mint_path_authority(elsewhere, token, 0, 0), StatusCode::Stale);
}

SLF_TEST(governance_encodings_roundtrip) {
  Command command;
  command.request_id = RequestId{1234};
  command.expected_generation = TopologyGeneration{7};
  command.expected_epoch = Epoch{3};
  command.kind = CommandKind::SetLinkAdmin;
  command.link = LinkId{9};
  command.link_admin = LinkAdmin::Disabled;
  command.node = NodeKey{SpineId{4}};
  command.node_state = NodeState::Draining;
  command.port = PortId{5};
  command.port_admin = PortAdmin::Disabled;
  command.fabric_state = FabricState::Draining;
  command.token.controller = mint_incarnation();
  command.token.epoch = Epoch{3};
  command.token.generation = TopologyGeneration{7};
  command.token.lease = LeaseId{2};
  command.token.topology_digest = Digest::of("topology");
  command.token.issued_at_unix_ms = 10;
  command.token.expires_at_unix_ms = 20;
  command.new_link.id = LinkId{11};
  command.new_link.a = NodeKey{LeafId{1}};
  command.new_link.port_a = PortId{1};
  command.new_link.b = NodeKey{SpineId{1}};
  command.new_link.port_b = PortId{2};
  command.new_link.generation = TopologyGeneration{7};
  command.evidence.link = LinkId{3};
  command.evidence.observed = LinkObservation::Up;
  command.evidence.generation = TopologyGeneration{7};
  command.evidence.observer = command.token.controller;
  command.evidence.epoch = Epoch{3};
  command.evidence.observed_seq = SequenceNumber{5};
  command.evidence.observed_at_unix_ms = 99;

  const auto decoded = deserialize_command(serialize_command(command));
  SLF_EXPECT(decoded.ok());
  if (decoded.ok()) {
    SLF_EXPECT_EQ(decoded.value().request_id, command.request_id);
    SLF_EXPECT_EQ(decoded.value().kind, command.kind);
    SLF_EXPECT_EQ(decoded.value().link, command.link);
    SLF_EXPECT_EQ(decoded.value().link_admin, command.link_admin);
    SLF_EXPECT_EQ(decoded.value().node, command.node);
    SLF_EXPECT_EQ(decoded.value().node_state, command.node_state);
    SLF_EXPECT_EQ(decoded.value().port_admin, command.port_admin);
    SLF_EXPECT_EQ(decoded.value().fabric_state, command.fabric_state);
    SLF_EXPECT_EQ(decoded.value().token.lease, command.token.lease);
    SLF_EXPECT_EQ(decoded.value().new_link.id, command.new_link.id);
    SLF_EXPECT_EQ(decoded.value().evidence.observed_seq, command.evidence.observed_seq);
    SLF_EXPECT_EQ(serialize_command(decoded.value()), serialize_command(command));
  }

  CommandResult result;
  result.request_id = RequestId{1234};
  result.code = StatusCode::Refused;
  result.detail = "nope";
  result.generation = TopologyGeneration{8};
  result.epoch = Epoch{3};
  result.topology_digest = Digest::of("topology");
  result.deduplicated = true;
  result.mutated = false;
  result.refreshed_token = command.token;
  const auto decoded_result = deserialize_command_result(serialize_command_result(result));
  SLF_EXPECT(decoded_result.ok());
  if (decoded_result.ok()) {
    SLF_EXPECT_EQ(decoded_result.value().code, result.code);
    SLF_EXPECT_EQ(decoded_result.value().detail, result.detail);
    SLF_EXPECT_EQ(decoded_result.value().deduplicated, true);
    // The durable command-result encoding deliberately does not carry the
    // refreshed lease: leases are runtime authority, not durable state, and they
    // are fenced by the incarnation and epoch anyway.
    SLF_EXPECT(decoded_result.value().refreshed_token.lease.is_zero());
  }

  // A default-constructed path authority must round-trip: this is the payload
  // every request and response carries, so an asymmetry here breaks the wire.
  const PathAuthority empty;
  const auto empty_decoded = deserialize_path_authority(serialize_path_authority(empty));
  SLF_EXPECT(empty_decoded.ok());
  PathAuthority populated;
  populated.controller = command.token.controller;
  populated.epoch = Epoch{3};
  populated.generation = TopologyGeneration{7};
  populated.topology_digest = Digest::of("topology");
  populated.lease = LeaseId{5};
  populated.from = NodeKey{LeafId{1}};
  populated.to = NodeKey{LeafId{2}};
  populated.cls = ReachabilityClass::LeafLeafSingleSpine;
  populated.verdict = EligibilityVerdict::Eligible;
  populated.spines = {SpineId{1}, SpineId{2}, SpineId{3}};
  populated.issued_at_unix_ms = 100;
  populated.expires_at_unix_ms = 200;
  populated.authority_digest = compute_path_authority_digest(populated);
  const auto decoded_authority = deserialize_path_authority(serialize_path_authority(populated));
  SLF_EXPECT(decoded_authority.ok());
  if (decoded_authority.ok()) {
    SLF_EXPECT_EQ(decoded_authority.value().spines, populated.spines);
    SLF_EXPECT_EQ(decoded_authority.value().authority_digest, populated.authority_digest);
    SLF_EXPECT_EQ(serialize_path_authority(decoded_authority.value()), serialize_path_authority(populated));
  }
  // Truncated encodings are refused at every prefix length.
  const std::vector<std::byte> bytes = serialize_path_authority(populated);
  for (std::size_t cut = 0; cut < bytes.size(); cut += 3) {
    SLF_EXPECT(!deserialize_path_authority(std::span<const std::byte>(bytes.data(), cut)).ok());
  }
}

SLF_TEST(governance_protocol_roundtrip) {
  net::Request request;
  request.kind = net::RequestKind::Paths;
  request.request_id = RequestId{5};
  request.client_name = "tester";
  request.from = NodeKey{LeafId{1}};
  request.to = NodeKey{LeafId{2}};
  request.constraints.require_domain_diversity = true;
  request.constraints.min_eligible_spines = 2;
  request.constraints.max_oversubscription = Rational{4, 1};
  request.constraints.max_sets = 8;
  request.constraints.max_explanations = 4;
  request.ttl_ms = 1234;
  const auto decoded_request = net::decode_request(net::encode_request(request), net::TransportLimits{});
  SLF_EXPECT(decoded_request.ok());
  if (decoded_request.ok()) {
    SLF_EXPECT_EQ(decoded_request.value().kind, request.kind);
    SLF_EXPECT_EQ(decoded_request.value().from, request.from);
    SLF_EXPECT_EQ(decoded_request.value().constraints.min_eligible_spines, 2U);
    SLF_EXPECT_EQ(decoded_request.value().constraints.max_oversubscription->num, 4U);
    SLF_EXPECT_EQ(decoded_request.value().ttl_ms, 1234U);
  }
  net::Response response;
  response.request_id = RequestId{5};
  response.code = StatusCode::Indeterminate;
  response.detail = "evidence is stale";
  response.controller = mint_incarnation();
  response.epoch = Epoch{2};
  response.generation = TopologyGeneration{6};
  response.topology_digest = Digest::of("topology");
  response.assessment.from = NodeKey{LeafId{1}};
  response.assessment.to = NodeKey{LeafId{2}};
  response.assessment.verdict = EligibilityVerdict::Indeterminate;
  response.assessment.explanation = {"one", "two"};
  response.assessment.covered_domains = {FailureDomainId{1}, FailureDomainId{2}};
  response.assessment.max_domain_loss_tolerance = 1;
  response.assessment.spine_sets.truncated = true;
  response.assessment.spine_sets.candidate_combinations = 3;
  EligibleSpineSet set;
  set.spines = {SpineId{1}, SpineId{2}};
  set.domains = {FailureDomainId{1}, FailureDomainId{2}};
  set.total_capacity_mbps = 200000;
  set.distinct_domains = 2;
  response.assessment.spine_sets.sets.push_back(set);
  response.assessment.capacity.uplink_capacity_mbps = 200000;
  response.assessment.capacity.uplink_count = 2;
  response.assessment.capacity.access_capacity_mbps = 800000;
  response.assessment.capacity.oversubscription = Rational{4, 1};
  response.assessment.capacity.classification = OversubscriptionClass::Oversubscribed;
  response.assessment.capacity.detail = "ratio";
  response.status.leaf_count = 2;
  response.status.name = "wire";
  response.health.health = FabricHealth::Degraded;
  response.lease_token = AuthorityToken{};
  const auto decoded_response = net::decode_response(net::encode_response(response), net::TransportLimits{});
  SLF_EXPECT(decoded_response.ok());
  if (decoded_response.ok()) {
    SLF_EXPECT_EQ(decoded_response.value().code, response.code);
    SLF_EXPECT_EQ(decoded_response.value().detail, response.detail);
    SLF_EXPECT_EQ(decoded_response.value().assessment.explanation.size(), 2U);
    SLF_EXPECT_EQ(decoded_response.value().status.name, std::string("wire"));
    SLF_EXPECT_EQ(decoded_response.value().assessment.spine_sets.sets.size(), 1U);
    SLF_EXPECT_EQ(decoded_response.value().assessment.spine_sets.sets.front().spines.size(), 2U);
    SLF_EXPECT_EQ(decoded_response.value().assessment.spine_sets.sets.front().domains.size(), 2U);
    SLF_EXPECT_EQ(decoded_response.value().assessment.spine_sets.sets.front().distinct_domains, 2U);
    SLF_EXPECT(decoded_response.value().assessment.spine_sets.truncated);
    SLF_EXPECT_EQ(decoded_response.value().assessment.spine_sets.candidate_combinations, 3U);
    SLF_EXPECT_EQ(decoded_response.value().assessment.capacity.oversubscription.to_string(),
                  std::string("4/1"));
    SLF_EXPECT_EQ(decoded_response.value().assessment.covered_domains.size(), 2U);
    SLF_EXPECT_EQ(decoded_response.value().health.health, FabricHealth::Degraded);
  }
}
