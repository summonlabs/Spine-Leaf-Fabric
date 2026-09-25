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

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "slf/eligibility.hpp"
#include "slf/governance.hpp"
#include "slf/model.hpp"
#include "slf/store.hpp"

namespace slf {

/// Injection points used by the crash-recovery tests. c None is the production
/// default; every other value makes the process terminate immediately
/// (c std::_Exit) at that exact boundary without unwinding, so durable-commit
/// ambiguity can be tested for real.
enum class FaultPoint : std::uint8_t {
  None = 0,
  /// Terminate before the commit record reaches the journal.
  BeforeCommit = 1,
  /// Terminate after the commit record is durable but before the reply is
  /// produced. A client that retries must not see the mutation applied twice.
  AfterCommitBeforeAck = 2,
  /// Terminate after the reply was produced but before the snapshot flush.
  AfterAckBeforeSnapshot = 3,
};

[[nodiscard]] std::string_view to_string(FaultPoint point) noexcept;
[[nodiscard]] std::optional<FaultPoint> parse_fault_point(std::string_view text) noexcept;

/// Exit code used by injected faults. Distinctive so tests can recognise it.
inline constexpr int kFaultExitCode = 70;

struct RuntimeConfig {
  FabricId fabric_id{};
  std::string name{"fabric"};
  std::string site;
  FabricOptions options{};
  BuilderLimits limits{};
  /// State directory. When empty, persistence is disabled and everything is
  /// in-memory only (used by the offline tooling paths).
  std::filesystem::path state_dir;
  bool enable_persistence{true};
  /// Number of background workers (sweeper and compactor). Zero means no
  /// background workers; the runtime then only reacts to explicit calls.
  std::uint32_t worker_threads{2};
  /// Default lease time to live for minted authority.
  std::uint64_t authority_ttl_ms{30000};
  /// Bounded dedup window for exactly-once command semantics.
  std::uint32_t max_dedup_entries{4096};
  /// Bounded command history retained in memory.
  std::uint32_t max_command_history{4096};
  /// Snapshot is written when this many records accumulated since the last one.
  std::uint64_t compaction_record_threshold{512};
  /// Sweeper period.
  std::uint64_t sweep_interval_ms{25};
  StoreConfig store{};
  /// Recovery policy for dynamic evidence: always true in production. When
  /// false (tests only) recovered evidence keeps its recorded freshness, which
  /// is used to prove that the default path really does fence it.
  bool recover_evidence_as_historical{true};
};

/// Immutable runtime status snapshot. Produced under lock and returned by value,
/// so reporting never exposes a half-updated view or a dangling reference.
struct RuntimeStatus {
  bool started{false};
  FabricId fabric_id{};
  std::string name;
  std::string site;
  FabricState fabric_state{FabricState::Unformed};
  ControllerIncarnation controller{};
  Epoch epoch{};
  TopologyGeneration generation{};
  Digest topology_digest{};
  Digest evidence_digest{};
  FabricHealth health{FabricHealth::Unformed};
  std::uint32_t leaf_count{0};
  std::uint32_t spine_count{0};
  std::uint32_t port_count{0};
  std::uint32_t link_count{0};
  std::uint32_t history_link_count{0};
  std::uint32_t evidence_count{0};
  std::uint64_t commands_applied{0};
  std::uint64_t commands_refused{0};
  std::uint64_t commands_fenced{0};
  std::uint64_t commands_duplicated{0};
  std::uint64_t queries_served{0};
  std::uint32_t worker_threads{0};
  bool persistence_enabled{false};
  RecoveryReport recovery{};
  LeaseId active_lease{};
  std::uint64_t lease_expires_at_unix_ms{0};
  std::uint64_t uptime_ms{0};
  FaultPoint fault_point{FaultPoint::None};

  [[nodiscard]] std::string to_string() const;
  /// Canonical digest over the status fields. Two identical statuses hash
  /// identically, and any field change changes the digest.
  [[nodiscard]] Digest digest() const;
};

/// The controller runtime: owns the durable state, the authority coordinate,
/// the immutable topology snapshot, and the background workers.
///
/// Thread safety and lock order are documented in c docs/concurrency.md. In
/// short: c writer_mutex_ serialises every mutation, c snapshot_mutex_
/// publishes immutable snapshots, the store has its own leaf mutex, and no
/// lock is ever held while joining a worker or invoking external code. There is
/// no read-to-write upgrade anywhere because readers never take a write lock:
/// they copy a c shared_ptr to an immutable snapshot.
class ControllerRuntime {
 public:
  explicit ControllerRuntime(RuntimeConfig config);
  ~ControllerRuntime();
  ControllerRuntime(const ControllerRuntime&) = delete;
  ControllerRuntime& operator=(const ControllerRuntime&) = delete;

  /// Recovers persisted state (conservatively), mints a fresh incarnation,
  /// bumps the epoch, fences all previously issued authority, and starts the
  /// workers. Returns c StatusCode::Corrupt when the persisted state cannot be
  /// used at all.
  [[nodiscard]] Status start();

  /// Graceful stop: stops workers without holding state they need, writes the
  /// clean-shutdown trailer, and closes the store. Idempotent.
  [[nodiscard]] Status stop();

  /// stop() followed by start(): the returned incarnation differs and the epoch
  /// is bumped, so authority minted before the restart is fenced.
  [[nodiscard]] Status restart();

  [[nodiscard]] bool started() const noexcept;

  /// Installs the initial fabric. Only accepted while the runtime has no
  /// topology yet, or when the caller explicitly asks to replace it.
  [[nodiscard]] Status install_fabric(Fabric fabric, bool replace);

  [[nodiscard]] RuntimeStatus status() const;

  [[nodiscard]] AuthorityContext authority_context() const;

  /// Acquires a lease and mints a token for the current coordinate.
  [[nodiscard]] Outcome<AuthorityLease> acquire_authority(std::string requester, std::uint64_t ttl_ms = 0);
  /// Invalidates the current lease. Every token minted for it becomes c Refused.
  [[nodiscard]] Status revoke_authority();

  [[nodiscard]] AuthorityCheck validate_authority(const AuthorityToken& token) const;
  [[nodiscard]] AuthorityCheck validate_path_authority_ref(const PathAuthority& authority) const;

  /// Applies a governed mutation. Never throws; every refusal is a typed code.
  [[nodiscard]] CommandResult apply(const Command& command);

  [[nodiscard]] Outcome<PathAssessment> assess_paths(NodeKey from, NodeKey to, const PathConstraints& constraints,
                                                     CancellationToken token = {}) const;
  /// Eligibility of every spine with respect to one leaf.
  [[nodiscard]] Outcome<std::vector<SpineEligibility>> eligible_spines(NodeKey leaf,
                                                                      const PathConstraints& constraints,
                                                                      CancellationToken token = {}) const;
  [[nodiscard]] Outcome<FabricHealthReport> health(const PathConstraints& constraints = {}) const;
  [[nodiscard]] Outcome<PathAuthority> authorize_path(NodeKey from, NodeKey to, const PathConstraints& constraints,
                                                      std::uint64_t ttl_ms = 0);

  /// Writes a snapshot if the journal grew past the compaction threshold.
  [[nodiscard]] Status compact();

  /// Test-only fault injection. Never persists across a restart.
  void set_fault_point(FaultPoint point) noexcept;

  /// Test-only: number of records in the current journal segment.
  [[nodiscard]] std::uint64_t journal_record_count() const;

  /// Test-only: forces the recovery report to be treated as if the process had
  /// died (used to verify fencing of recovered evidence).
  [[nodiscard]] RecoveryReport last_recovery() const;

 private:
  struct State;
  [[nodiscard]] Status start_workers_locked();
  void stop_workers_locked() noexcept;
  void sweeper_loop(std::stop_token token);
  void compactor_loop(std::stop_token token);
  void hit_fault(FaultPoint point) const;
  [[nodiscard]] CommandResult apply_locked(const Command& command);
  [[nodiscard]] AuthorityContext authority_context_locked() const;
  [[nodiscard]] Outcome<Fabric> builder_from_modified(const Fabric& base) const;
  /// Pure effect application shared by the live path and by journal replay.
  [[nodiscard]] Outcome<Fabric> apply_command_to(const Fabric& current, const Command& command,
                                                 TopologyGeneration target_generation, Epoch epoch,
                                                 ControllerIncarnation observer) const;
  [[nodiscard]] Status install_fabric_locked(Fabric&& fabric);
  [[nodiscard]] std::shared_ptr<const TopologyIndex> current_index() const;
  [[nodiscard]] std::shared_ptr<const Fabric> current_fabric() const;

  RuntimeConfig config_;

  // Serialises every mutation of durable state.
  mutable std::mutex writer_mutex_;
  // Serialises start/stop/restart as a whole, so the worker vector is only
  // appended to or joined by one lifecycle operation at a time.
  mutable std::mutex stop_mutex_;
  // Publishes immutable snapshots. Leaf lock: never acquired before
  // writer_mutex_ in any path.
  mutable std::mutex snapshot_mutex_;
  // Guards the cached health report.
  mutable std::mutex health_mutex_;

  std::shared_ptr<const Fabric> fabric_;
  std::shared_ptr<const TopologyIndex> index_;
  std::shared_ptr<const FabricHealthReport> health_;

  ControllerIncarnation incarnation_{};
  Epoch epoch_{};
  TopologyGeneration generation_{};
  std::optional<AuthorityLease> lease_{};
  std::uint64_t lease_counter_{0};

  std::unique_ptr<FabricStore> store_;
  RecoveryReport recovery_{};

  std::vector<std::jthread> workers_;
  std::atomic<bool> started_{false};
  std::atomic<bool> stop_requested_{false};
  FaultPoint fault_point_{FaultPoint::None};

  std::uint64_t started_at_ms_{0};
  std::uint64_t records_since_snapshot_{0};
  // Monotonic counters. They are read for reporting only, so a status snapshot
  // may observe them at marginally different instants; nothing decides on them.
  mutable std::atomic<std::uint64_t> commands_applied_{0};
  mutable std::atomic<std::uint64_t> commands_refused_{0};
  mutable std::atomic<std::uint64_t> commands_fenced_{0};
  mutable std::atomic<std::uint64_t> commands_duplicated_{0};
  mutable std::atomic<std::uint64_t> queries_served_{0};
  std::vector<std::pair<RequestId, CommandResult>> dedup_;
  std::vector<std::string> command_history_;
};

}  // namespace slf
