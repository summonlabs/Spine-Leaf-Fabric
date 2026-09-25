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

#include "slf/runtime.hpp"

#include <thread>

#include "fixtures.hpp"
#include "slf/platform.hpp"
#include "slf_test.hpp"

using namespace slf;
using namespace slf::test;

namespace {

RuntimeConfig config_for(const TempDir& dir, bool persistent = true, std::uint32_t workers = 0) {
  RuntimeConfig config;
  config.fabric_id = FabricId{1};
  config.name = "runtime-test";
  config.state_dir = persistent ? dir.path() : std::filesystem::path{};
  config.enable_persistence = persistent;
  config.worker_threads = workers;
  config.authority_ttl_ms = 5000;
  return config;
}

Command make_command(const AuthorityToken& token, TopologyGeneration generation, Epoch epoch,
                     RequestId request_id, CommandKind kind) {
  Command command;
  command.request_id = request_id;
  command.token = token;
  command.expected_generation = generation;
  command.expected_epoch = epoch;
  command.kind = kind;
  return command;
}

}  // namespace

SLF_TEST(runtime_start_stop_is_idempotent) {
  TempDir dir{"runtime-lifecycle"};
  ControllerRuntime runtime(config_for(dir));
  SLF_EXPECT(!runtime.started());
  // Commands before start are refused with a distinct code.
  Command command;
  command.request_id = RequestId{1};
  command.expected_generation = TopologyGeneration{1};
  command.expected_epoch = Epoch{1};
  const CommandResult early = runtime.apply(command);
  SLF_EXPECT_EQ(early.code, StatusCode::NotStarted);

  SLF_EXPECT(runtime.start().ok());
  SLF_EXPECT(runtime.started());
  SLF_EXPECT(runtime.start().ok());  // idempotent
  const auto first = runtime.status();
  SLF_EXPECT(first.started);
  SLF_EXPECT(!first.controller.is_zero());
  SLF_EXPECT(first.persistence_enabled);
  SLF_EXPECT(first.topology_digest.is_zero() || !first.topology_digest.is_zero());

  SLF_EXPECT(runtime.stop().ok());
  SLF_EXPECT(runtime.stop().ok());  // idempotent
  SLF_EXPECT(!runtime.started());
  // After stop, commands are refused again rather than silently applied.
  SLF_EXPECT_EQ(runtime.apply(command).code, StatusCode::NotStarted);
  SLF_EXPECT(runtime.restart().ok());
  SLF_EXPECT(runtime.started());
  SLF_EXPECT(runtime.stop().ok());
}

SLF_TEST(runtime_authority_lifecycle_and_fencing) {
  TempDir dir{"runtime-authority"};
  ControllerRuntime runtime(config_for(dir));
  SLF_EXPECT(runtime.start().ok());
  const auto installed = make_two_by_two(FabricOptions{.min_eligible_spines = 2});
  SLF_EXPECT(installed.ok());
  SLF_EXPECT(runtime.install_fabric(installed.value(), true).ok());

  // A command without a lease is refused.
  const CommandResult unleased = runtime.apply(
      make_command(AuthorityToken{}, runtime.status().generation, runtime.status().epoch, RequestId{1},
                   CommandKind::SetFabricState));
  SLF_EXPECT_EQ(unleased.code, StatusCode::Refused);

  const auto lease = runtime.acquire_authority("tester", 1000);
  SLF_EXPECT(lease.ok());
  SLF_EXPECT(!lease.value().token.lease.is_zero());
  SLF_EXPECT(runtime.validate_authority(lease.value().token).ok());

  // Applying a command advances the generation and refreshes the token.
  const auto before = runtime.status();
  const CommandResult applied = runtime.apply(
      make_command(lease.value().token, before.generation, before.epoch, RequestId{2},
                   CommandKind::SetFabricState));
  SLF_EXPECT_EQ(applied.code, StatusCode::Ok);
  SLF_EXPECT(applied.mutated);
  SLF_EXPECT(!applied.deduplicated);
  SLF_EXPECT_EQ(applied.generation, TopologyGeneration{before.generation.value + 1});
  SLF_EXPECT_EQ(applied.refreshed_token.generation, applied.generation);

  // The old token is now stale: same incarnation, older generation.
  const AuthorityCheck stale = runtime.validate_authority(lease.value().token);
  SLF_EXPECT_EQ(stale.code, StatusCode::Stale);

  // Exactly-once: repeating the same request id replays the recorded result.
  const CommandResult replayed = runtime.apply(
      make_command(lease.value().token, before.generation, before.epoch, RequestId{2},
                   CommandKind::SetFabricState));
  SLF_EXPECT_EQ(replayed.code, StatusCode::Ok);
  SLF_EXPECT(replayed.deduplicated);
  SLF_EXPECT_EQ(replayed.generation, applied.generation);
  SLF_EXPECT_EQ(runtime.status().generation, applied.generation);

  // A new lease works, and revoking it refuses further use.
  const auto second = runtime.acquire_authority("tester");
  SLF_EXPECT(second.ok());
  SLF_EXPECT(runtime.revoke_authority().ok());
  SLF_EXPECT_EQ(runtime.validate_authority(second.value().token).code, StatusCode::Refused);
  SLF_EXPECT_CODE(runtime.revoke_authority(), StatusCode::NotFound);

  // Restart: fresh incarnation and a bumped epoch fence everything that came
  // before, including tokens minted for the same generation.
  const ControllerIncarnation previous = runtime.status().controller;
  const Epoch previous_epoch = runtime.status().epoch;
  SLF_EXPECT(runtime.restart().ok());
  const auto after = runtime.status();
  SLF_EXPECT_NE(after.controller, previous);
  SLF_EXPECT(after.epoch.value > previous_epoch.value);
  const CommandResult fenced = runtime.apply(
      make_command(second.value().token, after.generation, after.epoch, RequestId{3},
                   CommandKind::SetFabricState));
  SLF_EXPECT_EQ(fenced.code, StatusCode::Fenced);
  SLF_EXPECT_EQ(runtime.status().generation, applied.generation);

  // The persisted state survived the restart.
  SLF_EXPECT(!after.topology_digest.is_zero());
  SLF_EXPECT_EQ(after.leaf_count, 2U);
  SLF_EXPECT(after.recovery.usable());
  SLF_EXPECT(runtime.stop().ok());
}

SLF_TEST(runtime_command_effects_and_validation) {
  TempDir dir{"runtime-commands"};
  ControllerRuntime runtime(config_for(dir));
  SLF_EXPECT(runtime.start().ok());
  const auto installed = make_two_by_two(FabricOptions{.min_eligible_spines = 2});
  SLF_EXPECT(installed.ok());
  SLF_EXPECT(runtime.install_fabric(installed.value(), true).ok());
  const auto lease = runtime.acquire_authority("tester");
  SLF_EXPECT(lease.ok());

  // A first command that names a link that does not exist is refused with the
  // identity code, and does not advance anything.
  Command bogus = make_command(lease.value().token, runtime.status().generation, runtime.status().epoch,
                               RequestId{9}, CommandKind::SetLinkAdmin);
  SLF_EXPECT_EQ(runtime.apply(bogus).code, StatusCode::NotFound);
  SLF_EXPECT_EQ(runtime.status().generation, TopologyGeneration{1});

  // SetLinkAdmin disables a link; the reply carries the refreshed token for the
  // generation it produced.
  Command disable = make_command(lease.value().token, runtime.status().generation, runtime.status().epoch,
                                 RequestId{10}, CommandKind::SetLinkAdmin);
  disable.link = LinkId{1};
  disable.link_admin = LinkAdmin::Disabled;
  CommandResult disable_result = runtime.apply(disable);
  SLF_EXPECT_EQ(disable_result.code, StatusCode::Ok);
  SLF_EXPECT_EQ(disable_result.generation, TopologyGeneration{2});
  SLF_EXPECT_EQ(runtime.status().generation, TopologyGeneration{2});

  // The refreshed token is what authorises the next command.
  Command remove = make_command(disable_result.refreshed_token, disable_result.generation,
                                disable_result.epoch, RequestId{12}, CommandKind::RemoveLink);
  remove.link = LinkId{2};
  const CommandResult removed = runtime.apply(remove);
  SLF_EXPECT_EQ(removed.code, StatusCode::Ok);
  const auto after_remove = runtime.status();
  SLF_EXPECT_EQ(after_remove.link_count, 3U);
  SLF_EXPECT_EQ(after_remove.history_link_count, 1U);

  // Unknown identities are reported, not ignored.
  Command missing = make_command(removed.refreshed_token, removed.generation, removed.epoch, RequestId{13},
                                 CommandKind::SetLinkAdmin);
  missing.link = LinkId{99};
  SLF_EXPECT_EQ(runtime.apply(missing).code, StatusCode::NotFound);

  Command unknown_leaf = make_command(removed.refreshed_token, removed.generation, removed.epoch,
                                      RequestId{14}, CommandKind::SetLeafState);
  unknown_leaf.node = NodeKey{LeafId{42}};
  SLF_EXPECT_EQ(runtime.apply(unknown_leaf).code, StatusCode::NotFound);

  // A command that expects the wrong coordinate is refused as stale rather than
  // applied to a fabric it was not written against.
  const auto context = runtime.status();
  Command wrong_epoch = make_command(removed.refreshed_token, context.generation, Epoch{99}, RequestId{15},
                                     CommandKind::SetFabricState);
  SLF_EXPECT_EQ(runtime.apply(wrong_epoch).code, StatusCode::Stale);

  // Evidence submission is dynamic: it is accepted but does not advance the
  // topology generation.
  Command evidence = make_command(removed.refreshed_token, removed.generation, removed.epoch, RequestId{16},
                                  CommandKind::SubmitEvidence);
  evidence.evidence.link = LinkId{3};
  evidence.evidence.observed = LinkObservation::Up;
  evidence.evidence.generation = removed.generation;
  evidence.evidence.observed_seq = SequenceNumber{1};
  const CommandResult evidence_result = runtime.apply(evidence);
  SLF_EXPECT_EQ(evidence_result.code, StatusCode::Ok);
  SLF_EXPECT_EQ(evidence_result.generation, removed.generation);
  SLF_EXPECT_EQ(runtime.status().evidence_count, 1U);
  SLF_EXPECT(runtime.stop().ok());
}

SLF_TEST(runtime_queries_and_path_authority) {
  TempDir dir{"runtime-paths"};
  ControllerRuntime runtime(config_for(dir));
  SLF_EXPECT(runtime.start().ok());
  const auto installed = make_two_by_two(FabricOptions{.require_domain_diversity = true,
                                                       .min_eligible_spines = 2});
  SLF_EXPECT(installed.ok());
  SLF_EXPECT(runtime.install_fabric(installed.value(), true).ok());
  const auto lease = runtime.acquire_authority("tester");
  SLF_EXPECT(lease.ok());

  const auto constraints = PathConstraints::defaults_for(installed.value().options);
  const auto assessment = runtime.assess_paths(NodeKey{LeafId{1}}, NodeKey{LeafId{2}}, constraints);
  SLF_EXPECT(assessment.ok());
  SLF_EXPECT_EQ(assessment.value().verdict, EligibilityVerdict::Eligible);
  SLF_EXPECT_EQ(assessment.value().controller, runtime.status().controller);

  const auto spines = runtime.eligible_spines(NodeKey{LeafId{1}}, constraints);
  SLF_EXPECT(spines.ok());
  SLF_EXPECT_EQ(spines.value().size(), 2U);
  const auto not_a_leaf = runtime.eligible_spines(NodeKey{SpineId{1}}, constraints);
  SLF_EXPECT_CODE(not_a_leaf, StatusCode::Invalid);

  const auto authority = runtime.authorize_path(NodeKey{LeafId{1}}, NodeKey{LeafId{2}}, constraints, 1000);
  SLF_EXPECT(authority.ok());
  SLF_EXPECT_EQ(authority.value().spines.size(), 2U);
  SLF_EXPECT(runtime.validate_path_authority_ref(authority.value()).ok());

  // Tampering with the authority body is detected by the authority digest.
  PathAuthority tampered = authority.value();
  tampered.spines.push_back(SpineId{9});
  SLF_EXPECT_EQ(runtime.validate_path_authority_ref(tampered).code, StatusCode::IntegrityError);

  // A restart fences the authority even though the topology is unchanged.
  SLF_EXPECT(runtime.restart().ok());
  SLF_EXPECT_EQ(runtime.validate_path_authority_ref(authority.value()).code, StatusCode::Fenced);

  // Multi-lease semantics: an old lease is refused after a new one is minted.
  const auto fresh = runtime.acquire_authority("tester");
  SLF_EXPECT(fresh.ok());
  PathAuthority stale_lease = authority.value();
  stale_lease.controller = runtime.status().controller;
  stale_lease.epoch = runtime.status().epoch;
  stale_lease.authority_digest = compute_path_authority_digest(stale_lease);
  SLF_EXPECT_EQ(runtime.validate_path_authority_ref(stale_lease).code, StatusCode::Refused);

  // Spine-to-spine eligibility is explicitly unsupported, not guessed.
  const auto spine_pair = runtime.assess_paths(NodeKey{SpineId{1}}, NodeKey{SpineId{2}}, constraints);
  SLF_EXPECT(spine_pair.ok());
  SLF_EXPECT_EQ(spine_pair.value().verdict, EligibilityVerdict::Indeterminate);
  SLF_EXPECT_EQ(spine_pair.value().code, StatusCode::Unsupported);
  SLF_EXPECT(runtime.stop().ok());
}

SLF_TEST(runtime_recovered_evidence_is_historical) {
  TempDir dir{"runtime-recovery"};
  RuntimeConfig config = config_for(dir, true, 0);
  {
    ControllerRuntime runtime(config);
    SLF_EXPECT(runtime.start().ok());
    auto fabric = make_two_by_two(FabricOptions{.min_eligible_spines = 1,
                                                .evidence_policy = EvidencePolicy::RequireFresh});
    SLF_EXPECT(fabric.ok());
    FabricBuilder builder(FabricId{1}, fabric.value().name, fabric.value().generation, Epoch{1},
                          fabric.value().options);
    for (const auto& leaf : fabric.value().leaves) {
      SLF_EXPECT(builder.add_leaf(leaf).ok());
    }
    for (const auto& spine : fabric.value().spines) {
      SLF_EXPECT(builder.add_spine(spine).ok());
    }
    for (const auto& port : fabric.value().ports) {
      SLF_EXPECT(builder.add_port(port).ok());
    }
    for (const auto& link : fabric.value().links) {
      SLF_EXPECT(builder.add_link(link).ok());
    }
    for (const auto& evidence :
         observe_all_links(fabric.value(), runtime.status().controller, runtime.status().epoch,
                           LinkObservation::Up)) {
      SLF_EXPECT(builder.add_evidence(evidence).ok());
    }
    const auto observed = builder.freeze();
    SLF_EXPECT(observed.ok());
    SLF_EXPECT(runtime.install_fabric(observed.value(), true).ok());

    const auto constraints = PathConstraints::defaults_for(observed.value().options);
    const auto live = runtime.assess_paths(NodeKey{LeafId{1}}, NodeKey{LeafId{2}}, constraints);
    SLF_EXPECT(live.ok());
    SLF_EXPECT_EQ(live.value().verdict, EligibilityVerdict::Eligible);
    SLF_EXPECT(runtime.stop().ok());
  }

  // Reopen: the evidence was observed by the previous incarnation, so it is
  // historical and the path is indeterminate until it is re-observed.
  ControllerRuntime reopened(config);
  SLF_EXPECT(reopened.start().ok());
  const auto status = reopened.status();
  SLF_EXPECT(status.recovery.usable());
  SLF_EXPECT(status.recovery.requires_reobservation);
  const auto constraints = PathConstraints::defaults_for(FabricOptions{.evidence_policy =
                                                                          EvidencePolicy::RequireFresh});
  const auto after = reopened.assess_paths(NodeKey{LeafId{1}}, NodeKey{LeafId{2}}, constraints);
  SLF_EXPECT(after.ok());
  SLF_EXPECT_EQ(after.value().verdict, EligibilityVerdict::Indeterminate);
  SLF_EXPECT_EQ(after.value().eligible_spines.size(), 0U);
  SLF_EXPECT(reopened.stop().ok());
}

SLF_TEST(runtime_snapshot_and_compaction) {
  TempDir dir{"runtime-compaction"};
  ControllerRuntime runtime(config_for(dir));
  SLF_EXPECT(runtime.start().ok());
  const auto installed = make_two_by_two();
  SLF_EXPECT(installed.ok());
  SLF_EXPECT(runtime.install_fabric(installed.value(), true).ok());
  SLF_EXPECT(runtime.compact().ok());
  SLF_EXPECT(std::filesystem::exists(default_snapshot_path(dir.path())));
  SLF_EXPECT(runtime.journal_record_count() > 0U);
  SLF_EXPECT(runtime.stop().ok());

  // The snapshot is used on the next start, and the topology survives.
  ControllerRuntime reopened(config_for(dir));
  SLF_EXPECT(reopened.start().ok());
  const auto status = reopened.status();
  SLF_EXPECT(status.recovery.snapshot_used);
  SLF_EXPECT_EQ(status.link_count, 4U);
  SLF_EXPECT_EQ(status.topology_digest, installed.value().topology_digest);
  SLF_EXPECT(reopened.stop().ok());
}

SLF_TEST(runtime_worker_threads_are_reaped) {
  TempDir dir{"runtime-workers"};
  {
    ControllerRuntime runtime(config_for(dir, true, 2));
    SLF_EXPECT(runtime.start().ok());
    SLF_EXPECT_EQ(runtime.status().worker_threads, 2U);
    const auto installed = make_two_by_two();
    SLF_EXPECT(installed.ok());
    SLF_EXPECT(runtime.install_fabric(installed.value(), true).ok());
    // Let the sweeper and compactor run several cycles, then stop repeatedly:
    // a stop that deadlocks or leaks a worker would hang or trip the sanitizer.
    platform::sleep_ms(60);
    SLF_EXPECT(runtime.compact().ok());
    SLF_EXPECT(runtime.stop().ok());
    SLF_EXPECT(runtime.stop().ok());
  }
  {
    ControllerRuntime runtime(config_for(dir, true, 0));
    SLF_EXPECT(runtime.start().ok());
    SLF_EXPECT_EQ(runtime.status().worker_threads, 0U);
    SLF_EXPECT(runtime.stop().ok());
  }
}
