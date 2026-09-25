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
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "slf/eligibility.hpp"
#include "slf/model.hpp"

namespace slf {

/// Current authority coordinate of a controller. A token is valid only against
/// the coordinate it was minted for; every field participates in the check, so
/// a restart (new incarnation), a recovery (new epoch), or a topology change
/// (new generation) fences all previously issued authority.
struct AuthorityContext {
  ControllerIncarnation controller{};
  Epoch epoch{};
  TopologyGeneration generation{};
  Digest topology_digest{};
  /// The lease the controller currently recognises, if any.
  std::optional<LeaseId> active_lease{};
  std::uint64_t now_unix_ms{0};
};

/// A time-bounded grant of write authority.
struct AuthorityToken {
  ControllerIncarnation controller{};
  Epoch epoch{};
  TopologyGeneration generation{};
  LeaseId lease{};
  Digest topology_digest{};
  std::uint64_t issued_at_unix_ms{0};
  std::uint64_t expires_at_unix_ms{0};

  [[nodiscard]] bool expired_at(std::uint64_t now_unix_ms) const noexcept {
    return now_unix_ms >= expires_at_unix_ms;
  }
};

/// Result of validating a token or a derived authority reference.
struct AuthorityCheck {
  StatusCode code{StatusCode::Ok};
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return code == StatusCode::Ok; }
};

/// Validates p token against p context. Distinguishes:
///  * c Ok             - the token matches the live coordinate and is unexpired
///  * c Fenced         - a newer controller incarnation exists
///  * c Stale          - same incarnation, older epoch or generation
///  * c Conflicting    - same coordinate but a different topology digest
///  * c DeadlineExpired- the lease has expired
///  * c Refused        - no lease is active or the lease id is not recognised
[[nodiscard]] AuthorityCheck validate_token(const AuthorityToken& token, const AuthorityContext& context);

/// A minted lease: the token plus bookkeeping fields.
struct AuthorityLease {
  AuthorityToken token{};
  std::string requester;
  LeaseId lease{};

  [[nodiscard]] std::uint64_t ttl_ms(std::uint64_t now_unix_ms) const noexcept {
    return token.expires_at_unix_ms > now_unix_ms ? token.expires_at_unix_ms - now_unix_ms : 0;
  }
};

/// Reference to a previously computed path decision. It is bound to the
/// coordinate (controller, epoch, generation, digest) that produced it and to
/// the exact spine set that was authorised. Re-validating it later detects
/// reincarnation, epoch bumps, generation changes, and topology drift.
struct PathAuthority {
  ControllerIncarnation controller{};
  Epoch epoch{};
  TopologyGeneration generation{};
  Digest topology_digest{};
  LeaseId lease{};
  NodeKey from{};
  NodeKey to{};
  ReachabilityClass cls{ReachabilityClass::Unreachable};
  EligibilityVerdict verdict{EligibilityVerdict::Ineligible};
  std::vector<SpineId> spines{};
  std::uint64_t issued_at_unix_ms{0};
  std::uint64_t expires_at_unix_ms{0};
  /// Digest over the authority body; detects field-level tampering or a
  /// truncated record.
  Digest authority_digest{};
};

/// Computes the authority digest over the canonical encoding of p authority.
[[nodiscard]] Digest compute_path_authority_digest(const PathAuthority& authority);

/// Mints a path authority from an assessment. Fails with
/// c StatusCode::Indeterminate when the assessment is not c Eligible: this
/// runtime never issues authority for a path it cannot prove.
[[nodiscard]] Outcome<PathAuthority> mint_path_authority(const PathAssessment& assessment,
                                                         const AuthorityToken& token,
                                                         std::uint64_t now_unix_ms,
                                                         std::uint64_t ttl_ms);

/// Validates a path authority against the live coordinate. Detects:
///  * c IntegrityError    - the authority digest does not match the body
///  * c Fenced            - minted by a previous controller incarnation
///  * c Stale             - older epoch or generation
///  * c Conflicting       - same generation, different topology digest
///  * c DeadlineExpired   - the authority itself has expired
///  * c Refused           - the lease is not the live lease
[[nodiscard]] AuthorityCheck validate_path_authority(const PathAuthority& authority,
                                                     const AuthorityContext& context);

/// Mutation kinds accepted by the governor.
enum class CommandKind : std::uint8_t {
  SetFabricState = 0,
  SetLeafState = 1,
  SetSpineState = 2,
  SetPortAdmin = 3,
  SetLinkAdmin = 4,
  AddLink = 5,
  RemoveLink = 6,
  SetOptions = 7,
  SubmitEvidence = 8,
  ClearEvidence = 9,
};

[[nodiscard]] std::string_view to_string(CommandKind kind) noexcept;
[[nodiscard]] std::optional<CommandKind> parse_command_kind(std::string_view text) noexcept;

/// A governed mutation request. It carries its authority token, the generation
/// and epoch the caller believes are current, and a request id used for
/// exactly-once semantics across retries.
struct Command {
  RequestId request_id{};
  AuthorityToken token{};
  TopologyGeneration expected_generation{};
  Epoch expected_epoch{};
  CommandKind kind{CommandKind::SetFabricState};

  FabricState fabric_state{FabricState::Operational};
  NodeKey node{};
  NodeState node_state{NodeState::Active};
  PortId port{};
  PortAdmin port_admin{PortAdmin::Enabled};
  LinkId link{};
  LinkAdmin link_admin{LinkAdmin::Enabled};
  LinkRecord new_link{};
  LinkEvidence evidence{};
  FabricOptions options{};
};

/// Result of a governed mutation. c deduplicated is true when the runtime
/// recognised c request_id from a previously committed request and replayed
/// the recorded result instead of applying the mutation twice. This is what
/// makes a client retry after a crash-before-ack safe.
struct CommandResult {
  StatusCode code{StatusCode::Ok};
  std::string detail;
  RequestId request_id{};
  TopologyGeneration generation{};
  Epoch epoch{};
  Digest topology_digest{};
  bool deduplicated{false};
  bool mutated{false};
  AuthorityToken refreshed_token{};

  [[nodiscard]] bool ok() const noexcept { return code == StatusCode::Ok; }
  [[nodiscard]] std::string to_string() const;
};

/// Canonical encoding of a command (used by the journal and the wire codec).
[[nodiscard]] std::vector<std::byte> serialize_command(const Command& command);
[[nodiscard]] Outcome<Command> deserialize_command(const std::span<const std::byte>& data);
[[nodiscard]] std::vector<std::byte> serialize_command_result(const CommandResult& result);
[[nodiscard]] Outcome<CommandResult> deserialize_command_result(const std::span<const std::byte>& data);

/// Canonical encoding of a path authority (used by the wire codec).
[[nodiscard]] std::vector<std::byte> serialize_path_authority(const PathAuthority& authority);
[[nodiscard]] Outcome<PathAuthority> deserialize_path_authority(const std::span<const std::byte>& data);

}  // namespace slf
