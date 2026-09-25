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

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <utility>

#include "slf/builder.hpp"
#include "slf/bytes.hpp"
#include "slf/checked.hpp"
#include "slf/platform.hpp"

namespace slf {
namespace {

/// Collects journal records during recovery so the runtime can replay them in
/// order without holding any lock while the store reads files.
class ReplayCollector final : public ReplaySink {
 public:
  [[nodiscard]] Status on_record(const RecoveredRecord& record) override {
    if (records.size() >= 1000000U) {
      return Status(StatusCode::Exhausted, "replay bound reached");
    }
    records.push_back(record);
    return Status::success();
  }
  std::vector<RecoveredRecord> records;
};

[[nodiscard]] bool is_topology_command(CommandKind kind) noexcept {
  return kind != CommandKind::SubmitEvidence && kind != CommandKind::ClearEvidence;
}

[[nodiscard]] TopologyGeneration next_generation(TopologyGeneration current) noexcept {
  return TopologyGeneration{current.value + 1};
}

}  // namespace

std::string_view to_string(FaultPoint point) noexcept {
  switch (point) {
    case FaultPoint::None: return "none";
    case FaultPoint::BeforeCommit: return "before_commit";
    case FaultPoint::AfterCommitBeforeAck: return "after_commit_before_ack";
    case FaultPoint::AfterAckBeforeSnapshot: return "after_ack_before_snapshot";
  }
  return "invalid";
}

std::optional<FaultPoint> parse_fault_point(std::string_view text) noexcept {
  if (text == "none") return FaultPoint::None;
  if (text == "before_commit") return FaultPoint::BeforeCommit;
  if (text == "after_commit_before_ack") return FaultPoint::AfterCommitBeforeAck;
  if (text == "after_ack_before_snapshot") return FaultPoint::AfterAckBeforeSnapshot;
  return std::nullopt;
}

std::string RuntimeStatus::to_string() const {
  std::ostringstream out;
  out << "runtime: " << (started ? "started" : "stopped") << " fabric=" << slf::to_string(fabric_id)
      << " name=\"" << name << "\"";
  if (!site.empty()) {
    out << " site=\"" << site << "\"";
  }
  out << "\n";
  out << "  state=" << slf::to_string(fabric_state) << " health=" << slf::to_string(health)
      << " generation=" << generation.value << " epoch=" << epoch.value << "\n";
  out << "  controller=" << slf::to_string(controller) << " uptime_ms=" << uptime_ms
      << " workers=" << worker_threads << " persistence=" << (persistence_enabled ? "on" : "off")
      << "\n";
  out << "  topology_digest=" << topology_digest.short_hex(16) << " evidence_digest="
      << (evidence_count == 0 ? std::string("none") : evidence_digest.short_hex(16)) << "\n";
  out << "  topology: leaves=" << leaf_count << " spines=" << spine_count << " ports=" << port_count
      << " links=" << link_count << " history_links=" << history_link_count
      << " evidence=" << evidence_count << "\n";
  out << "  commands: applied=" << commands_applied << " refused=" << commands_refused
      << " fenced=" << commands_fenced << " deduplicated=" << commands_duplicated
      << " queries=" << queries_served << "\n";
  out << "  lease=" << active_lease.value << " expires_at_ms=" << lease_expires_at_unix_ms
      << " fault_point=" << slf::to_string(fault_point) << "\n";
  out << "  recovery: " << recovery.to_string();
  return out.str();
}

Digest RuntimeStatus::digest() const {
  DigestBuilder builder;
  builder.add_string("slf.runtime_status.v1");
  builder.add_u8(started ? 1U : 0U);
  builder.add_u64(fabric_id.value);
  builder.add_string(name);
  builder.add_string(site);
  builder.add_u8(static_cast<std::uint8_t>(fabric_state));
  builder.add_u64(controller.hi);
  builder.add_u64(controller.lo);
  builder.add_u64(epoch.value);
  builder.add_u64(generation.value);
  builder.add_bytes(std::span<const std::byte>(topology_digest.bytes));
  builder.add_bytes(std::span<const std::byte>(evidence_digest.bytes));
  builder.add_u8(static_cast<std::uint8_t>(health));
  builder.add_u32(leaf_count);
  builder.add_u32(spine_count);
  builder.add_u32(port_count);
  builder.add_u32(link_count);
  builder.add_u32(history_link_count);
  builder.add_u32(evidence_count);
  builder.add_u64(commands_applied);
  builder.add_u64(commands_refused);
  builder.add_u64(commands_fenced);
  builder.add_u64(commands_duplicated);
  builder.add_u64(queries_served);
  builder.add_u32(worker_threads);
  builder.add_u8(persistence_enabled ? 1U : 0U);
  builder.add_u64(active_lease.value);
  builder.add_u64(lease_expires_at_unix_ms);
  builder.add_u8(static_cast<std::uint8_t>(fault_point));
  builder.add_u8(static_cast<std::uint8_t>(recovery.status));
  builder.add_u64(recovery.records_valid);
  builder.add_u64(recovery.bytes_dropped);
  return builder.finish();
}

ControllerRuntime::ControllerRuntime(RuntimeConfig config) : config_(std::move(config)) {}

ControllerRuntime::~ControllerRuntime() {
  const Status status = stop();
  (void)status;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Status ControllerRuntime::start() {
  std::unique_lock<std::mutex> writer(writer_mutex_);
  if (started_.load(std::memory_order_acquire)) {
    return Status::success();
  }
  stop_requested_.store(false, std::memory_order_release);
  incarnation_ = mint_incarnation();
  epoch_ = Epoch{1};
  recovery_ = RecoveryReport{};
  records_since_snapshot_ = 0;
  dedup_.clear();
  command_history_.clear();

  std::shared_ptr<const Fabric> recovered_fabric;
  std::uint64_t next_sequence = 1;
  std::uint32_t next_segment = 1;
  Epoch recovered_epoch{};
  std::uint64_t recovered_commands = 0;

  if (config_.enable_persistence && !config_.state_dir.empty()) {
    ReplayCollector collector;
    const auto outcome = recover_store(config_.state_dir, config_.store, &collector);
    if (!outcome.ok()) {
      return outcome.status();
    }
    recovery_ = outcome.value().report;
    // A missing journal is the normal first-boot case: there is simply no state
    // to recover yet. Only a store that exists and cannot be interpreted is
    // fatal, and in that case this runtime refuses to start rather than
    // silently beginning from an empty topology.
    switch (recovery_.status) {
      case RecoveryStatus::Corrupt:
      case RecoveryStatus::ChainBroken:
      case RecoveryStatus::IncompatibleVersion:
      case RecoveryStatus::IoError:
        return Status(recovery_.code == StatusCode::Ok ? StatusCode::Corrupt : recovery_.code,
                      "persisted state cannot be used: " + recovery_.detail);
      case RecoveryStatus::Missing:
      case RecoveryStatus::Empty:
      case RecoveryStatus::Clean:
      case RecoveryStatus::CrashWithoutTrailer:
      case RecoveryStatus::TruncatedTail:
        break;
    }
    if (outcome.value().snapshot_accepted && outcome.value().snapshot_fabric.has_value()) {
      recovered_fabric = std::make_shared<const Fabric>(*outcome.value().snapshot_fabric);
      next_sequence = outcome.value().snapshot_watermark.value + 1;
    }
    for (const auto& segment : outcome.value().segments) {
      next_segment = std::max(next_segment, segment.segment_index + 1U);
      next_sequence = std::max(next_sequence, segment.last_sequence.value + 1);
    }
    recovered_epoch = recovery_.last_epoch;

    for (const auto& record : collector.records) {
      if (record.epoch.value > recovered_epoch.value) {
        recovered_epoch = record.epoch;
      }
      switch (record.type) {
        case RecordType::FabricSnapshot: {
          const auto fabric = deserialize_fabric(record.payload, config_.limits);
          if (!fabric.ok()) {
            recovery_.status = RecoveryStatus::Corrupt;
            recovery_.code = fabric.code();
            recovery_.detail = "persisted fabric snapshot could not be parsed: " +
                               std::string(fabric.status().detail());
            return fabric.status();
          }
          recovered_fabric = std::make_shared<const Fabric>(fabric.value());
          break;
        }
        case RecordType::CommandCommit: {
          ByteReader reader(record.payload, config_.store.max_record_bytes);
          const auto command_bytes = reader.block(config_.store.max_record_bytes);
          const auto result_bytes = reader.block(config_.store.max_record_bytes);
          if (!command_bytes.ok() || !result_bytes.ok()) {
            recovery_.status = RecoveryStatus::Corrupt;
            recovery_.code = StatusCode::Corrupt;
            recovery_.detail = "persisted command commit record is malformed";
            return Status(StatusCode::Corrupt, recovery_.detail);
          }
          const auto command = deserialize_command(command_bytes.value());
          const auto result = deserialize_command_result(result_bytes.value());
          if (!command.ok() || !result.ok()) {
            recovery_.status = RecoveryStatus::Corrupt;
            recovery_.code = StatusCode::Corrupt;
            recovery_.detail = "persisted command or result could not be decoded";
            return Status(StatusCode::Corrupt, recovery_.detail);
          }
          CommandResult stored = result.value();
          stored.deduplicated = false;
          if (dedup_.size() < config_.max_dedup_entries) {
            dedup_.emplace_back(command.value().request_id, stored);
          }
          // Re-apply the committed effect so the recovered topology is exactly
          // the one that was committed, at the generation the record carries.
          if (recovered_fabric != nullptr) {
            const auto reapplied = apply_command_to(*recovered_fabric, command.value(),
                                                    stored.generation, record.epoch, record.incarnation);
            if (!reapplied.ok()) {
              recovery_.status = RecoveryStatus::Corrupt;
              recovery_.code = reapplied.code();
              recovery_.detail = "committed command could not be replayed: " +
                                 std::string(reapplied.status().detail());
              return Status(reapplied.code(), recovery_.detail);
            }
            recovered_fabric = std::make_shared<const Fabric>(reapplied.value());
          }
          ++recovered_commands;
          break;
        }
        case RecordType::EvidenceSubmit:
        case RecordType::Fence:
        case RecordType::IncarnationStart:
        case RecordType::Marker:
          break;
      }
    }
    // The state directory is bounded: when another segment would exceed the
    // configured maximum, a snapshot covering the recovered state is written
    // first and the segments it supersedes are pruned. Refusing to start is the
    // alternative, and losing durable state is not.
    if (recovered_fabric != nullptr && !outcome.value().segments.empty() &&
        outcome.value().segments.size() + 1U > config_.store.max_segments) {
      const SequenceNumber watermark = outcome.value().segments.back().last_sequence;
      const Status snapshot =
          SnapshotFile::write(default_snapshot_path(config_.state_dir), *recovered_fabric, watermark, config_.store);
      if (!snapshot.ok()) {
        return Status(snapshot.code(),
                      "state directory is at its segment bound and compaction failed: " +
                          std::string(snapshot.detail()));
      }
      for (const auto& segment : outcome.value().segments) {
        (void)platform::remove_file(segment.path);
      }
      recovery_.detail += "; pruned " + std::to_string(outcome.value().segments.size()) +
                          " segment(s) covered by a new snapshot";
    }
    if (recovered_fabric != nullptr && !recovered_fabric->evidence.empty()) {
      // Evidence can arrive either as an EvidenceSubmit record or inside a
      // fabric snapshot. Either way it was observed by a previous incarnation,
      // so it is historical and must be re-observed before it supports
      // eligibility.
      recovery_.dynamic_evidence_recovered = true;
      recovery_.requires_reobservation = true;
    }
    recovery_.detail += "; replayed " + std::to_string(recovered_commands) + " committed command(s)";
    if (recovery_.dynamic_evidence_recovered) {
      recovery_.requires_reobservation = true;
      recovery_.detail += "; dynamic evidence recovered from a previous incarnation is historical";
    }
  }

  if (recovered_fabric == nullptr) {
    FabricBuilder builder(config_.fabric_id, config_.name, TopologyGeneration{1}, epoch_, config_.options,
                          config_.limits);
    (void)builder.set_site(config_.site);
    auto built = builder.freeze();
    if (!built.ok()) {
      return built.status();
    }
    recovered_fabric = std::make_shared<const Fabric>(built.value());
  }
  fabric_ = recovered_fabric;
  generation_ = fabric_->generation;
  if (config_.recover_evidence_as_historical) {
    // Default and only production-safe policy: recovered observations keep the
    // incarnation that produced them, so they are historical and cannot support
    // eligibility until they are re-observed.
  }
  const auto index = TopologyIndex::build(fabric_);
  if (!index.ok()) {
    return index.status();
  }
  {
    std::lock_guard<std::mutex> snapshot_guard(snapshot_mutex_);
    index_ = std::make_shared<const TopologyIndex>(index.value());
  }

  // A restart always advances the epoch, which fences every token, lease, and
  // path authority minted by the previous incarnation.
  epoch_ = Epoch{recovered_epoch.value + 1};

  if (config_.enable_persistence && !config_.state_dir.empty()) {
    auto store = FabricStore::create(config_.state_dir, config_.store, incarnation_, next_segment,
                                     SequenceNumber{next_sequence});
    if (!store.ok()) {
      return store.status();
    }
    store_ = std::move(store.value());
    const Status start_record = store_->append(RecordType::IncarnationStart, {}, incarnation_, epoch_,
                                               generation_);
    if (!start_record.ok()) {
      return start_record;
    }
    const Status snapshot_record = store_->append_fabric(*fabric_, incarnation_, epoch_);
    if (!snapshot_record.ok()) {
      return snapshot_record;
    }
    records_since_snapshot_ = 0;
  }

  lease_.reset();
  started_at_ms_ = platform::monotonic_ms();
  started_.store(true, std::memory_order_release);
  const Status workers = start_workers_locked();
  if (!workers.ok()) {
    started_.store(false, std::memory_order_release);
    return workers;
  }
  return Status::success();
}

Status ControllerRuntime::start_workers_locked() {
  workers_.clear();
  const std::uint32_t requested = config_.worker_threads;
  if (requested == 0) {
    return Status::success();
  }
  if (requested >= 1) {
    workers_.emplace_back([this](std::stop_token token) { sweeper_loop(token); });
  }
  if (requested >= 2) {
    workers_.emplace_back([this](std::stop_token token) { compactor_loop(token); });
  }
  return Status::success();
}

void ControllerRuntime::stop_workers_locked() noexcept {
  // Requesting the stop first lets every worker leave its wait promptly. The
  // join happens in stop() after the writer lock is released: a worker may be
  // waiting for that lock, so joining while holding it would deadlock.
  for (auto& worker : workers_) {
    worker.request_stop();
  }
}

Status ControllerRuntime::stop() {
  // The whole stop sequence is serialised by stop_mutex_: the worker vector is
  // joined and emptied here, so two concurrent stops would otherwise join the
  // same threads twice. Work is never done while holding it except the short
  // bookkeeping sections below.
  std::lock_guard<std::mutex> stop_guard(stop_mutex_);
  {
    std::lock_guard<std::mutex> guard(writer_mutex_);
    if (!started_.load(std::memory_order_acquire) && workers_.empty()) {
      return Status::success();
    }
    stop_requested_.store(true, std::memory_order_release);
    stop_workers_locked();
  }
  // Join without holding any state lock: a worker may be waiting for
  // writer_mutex_, so joining while holding it would deadlock.
  std::vector<std::jthread> workers;
  {
    std::lock_guard<std::mutex> guard(writer_mutex_);
    workers.swap(workers_);
  }
  for (auto& worker : workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers.clear();

  std::lock_guard<std::mutex> guard(writer_mutex_);
  if (!started_.load(std::memory_order_acquire)) {
    return Status::success();
  }
  started_.store(false, std::memory_order_release);
  lease_.reset();
  if (store_ != nullptr) {
    const Status closed = store_->close_clean();
    store_.reset();
    if (!closed.ok()) {
      return closed;
    }
  }
  return Status::success();
}

Status ControllerRuntime::restart() {
  const Status stopped = stop();
  if (!stopped.ok()) {
    return stopped;
  }
  return start();
}

bool ControllerRuntime::started() const noexcept { return started_.load(std::memory_order_acquire); }

void ControllerRuntime::sweeper_loop(std::stop_token token) {
  while (!token.stop_requested()) {
    platform::sleep_ms(config_.sweep_interval_ms);
    if (token.stop_requested() || stop_requested_.load(std::memory_order_acquire)) {
      return;
    }
    std::shared_ptr<const Fabric> fabric;
    {
      std::lock_guard<std::mutex> guard(snapshot_mutex_);
      fabric = fabric_;
    }
    if (fabric == nullptr) {
      continue;
    }
    FabricHealthReport report;
    {
      std::lock_guard<std::mutex> guard(writer_mutex_);
      if (!started_.load(std::memory_order_acquire) || stop_requested_.load(std::memory_order_acquire)) {
        return;
      }
      const auto snapshot_index = current_index();
      if (snapshot_index != nullptr) {
        EligibilityEngine engine(*snapshot_index, incarnation_);
        const auto health = engine.health(PathConstraints::defaults_for(fabric->options), incarnation_, epoch_);
        if (health.ok()) {
          report = health.value();
        } else {
          report.health = FabricHealth::Indeterminate;
          report.code = health.code();
          report.summary = std::string(health.status().detail());
        }
      }
      if (lease_.has_value() && lease_->token.expired_at(platform::now_unix_ms())) {
        // An expired lease stops authorising anything; tokens that reference it
        // are refused from this point on.
        lease_.reset();
      }
    }
    std::lock_guard<std::mutex> guard(health_mutex_);
    health_ = std::make_shared<const FabricHealthReport>(std::move(report));
  }
}

void ControllerRuntime::compactor_loop(std::stop_token token) {
  while (!token.stop_requested()) {
    platform::sleep_ms(config_.sweep_interval_ms * 4U);
    if (token.stop_requested() || stop_requested_.load(std::memory_order_acquire)) {
      return;
    }
    std::uint64_t records = 0;
    {
      std::lock_guard<std::mutex> guard(writer_mutex_);
      records = records_since_snapshot_;
    }
    if (records >= config_.compaction_record_threshold) {
      const Status status = compact();
      (void)status;
    }
  }
}

// ---------------------------------------------------------------------------
// Snapshot access
// ---------------------------------------------------------------------------

std::shared_ptr<const Fabric> ControllerRuntime::current_fabric() const {
  std::lock_guard<std::mutex> guard(snapshot_mutex_);
  return fabric_;
}

std::shared_ptr<const TopologyIndex> ControllerRuntime::current_index() const {
  std::lock_guard<std::mutex> guard(snapshot_mutex_);
  return index_;
}

// ---------------------------------------------------------------------------
// Authority
// ---------------------------------------------------------------------------

AuthorityContext ControllerRuntime::authority_context() const {
  AuthorityContext context;
  std::lock_guard<std::mutex> guard(writer_mutex_);
  context.controller = incarnation_;
  context.epoch = epoch_;
  context.generation = generation_;
  if (fabric_ != nullptr) {
    context.topology_digest = fabric_->topology_digest;
  }
  if (lease_.has_value()) {
    context.active_lease = lease_->lease;
  }
  context.now_unix_ms = platform::now_unix_ms();
  return context;
}

Outcome<AuthorityLease> ControllerRuntime::acquire_authority(std::string requester, std::uint64_t ttl_ms) {
  std::lock_guard<std::mutex> guard(writer_mutex_);
  if (!started_.load(std::memory_order_acquire)) {
    return Status(StatusCode::NotStarted, "runtime has not been started");
  }
  if (requester.empty() || requester.size() > 256U || !is_valid_utf8(requester)) {
    return Status(StatusCode::Invalid, "requester name is missing, too long, or not valid UTF-8");
  }
  const std::uint64_t ttl = ttl_ms == 0 ? config_.authority_ttl_ms : ttl_ms;
  if (ttl == 0) {
    return Status(StatusCode::Invalid, "authority lease time to live must be non-zero");
  }
  const std::uint64_t now = platform::now_unix_ms();
  std::uint64_t expires = 0;
  if (!checked_add(now, ttl, expires)) {
    return Status(StatusCode::Overflow, "lease expiry overflowed");
  }
  AuthorityLease lease;
  lease.requester = std::move(requester);
  ++lease_counter_;
  lease.lease = LeaseId{lease_counter_};
  lease.token.controller = incarnation_;
  lease.token.epoch = epoch_;
  lease.token.generation = generation_;
  lease.token.lease = lease.lease;
  if (fabric_ != nullptr) {
    lease.token.topology_digest = fabric_->topology_digest;
  }
  lease.token.issued_at_unix_ms = now;
  lease.token.expires_at_unix_ms = expires;
  lease_ = lease;
  if (store_ != nullptr) {
    ByteWriter writer(4096);
    (void)writer.u64(lease.lease.value);
    (void)writer.u64(lease.token.issued_at_unix_ms);
    (void)writer.u64(lease.token.expires_at_unix_ms);
    (void)writer.string(lease.requester);
    const Status appended = store_->append(RecordType::Fence, writer.buffer(), incarnation_, epoch_, generation_);
    if (!appended.ok()) {
      lease_.reset();
      return appended;
    }
    ++records_since_snapshot_;
  }
  return lease;
}

Status ControllerRuntime::revoke_authority() {
  std::lock_guard<std::mutex> guard(writer_mutex_);
  if (!lease_.has_value()) {
    return Status(StatusCode::NotFound, "no lease is active");
  }
  lease_.reset();
  if (store_ != nullptr) {
    const Status appended = store_->append(RecordType::Fence, {}, incarnation_, epoch_, generation_);
    if (!appended.ok()) {
      return appended;
    }
    ++records_since_snapshot_;
  }
  return Status::success();
}

AuthorityCheck ControllerRuntime::validate_authority(const AuthorityToken& token) const {
  return validate_token(token, authority_context());
}

AuthorityCheck ControllerRuntime::validate_path_authority_ref(const PathAuthority& authority) const {
  return validate_path_authority(authority, authority_context());
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void ControllerRuntime::hit_fault(FaultPoint point) const {
  if (fault_point_ == FaultPoint::None || fault_point_ != point) {
    return;
  }
  // Hard, unwinding-free termination. This is deliberate fault injection used
  // by the crash-recovery tests; std::_Exit skips destructors and flushes, which
  // is exactly the boundary being tested.
  std::_Exit(kFaultExitCode);
}

void ControllerRuntime::set_fault_point(FaultPoint point) noexcept { fault_point_ = point; }

CommandResult ControllerRuntime::apply(const Command& command) {
  std::lock_guard<std::mutex> guard(writer_mutex_);
  return apply_locked(command);
}

CommandResult ControllerRuntime::apply_locked(const Command& command) {
  CommandResult result;
  result.request_id = command.request_id;
  result.generation = generation_;
  result.epoch = epoch_;
  if (fabric_ != nullptr) {
    result.topology_digest = fabric_->topology_digest;
  }
  if (!started_.load(std::memory_order_acquire)) {
    result.code = StatusCode::NotStarted;
    result.detail = "runtime has not been started";
    ++commands_refused_;
    return result;
  }
  if (command.request_id.is_zero()) {
    result.code = StatusCode::Invalid;
    result.detail = "a governed command must carry a non-zero request id";
    ++commands_refused_;
    return result;
  }
  for (const auto& [request_id, recorded] : dedup_) {
    if (request_id == command.request_id) {
      CommandResult replay = recorded;
      replay.deduplicated = true;
      ++commands_duplicated_;
      return replay;
    }
  }
  if (dedup_.size() >= config_.max_dedup_entries) {
    // Refusing is the conservative choice: forgetting a committed request id
    // would let a client retry apply the same mutation twice.
    result.code = StatusCode::Exhausted;
    result.detail = "the exactly-once window is full; compact or extend max_dedup_entries";
    ++commands_refused_;
    return result;
  }

  const AuthorityCheck check = validate_token(command.token, authority_context_locked());
  if (!check.ok()) {
    result.code = check.code;
    result.detail = check.detail;
    if (is_stale_family(check.code)) {
      ++commands_fenced_;
    } else {
      ++commands_refused_;
    }
    return result;
  }
  if (command.expected_generation != generation_ || command.expected_epoch != epoch_) {
    result.code = StatusCode::Stale;
    result.detail = "command expected generation " + std::to_string(command.expected_generation.value) +
                    " epoch " + std::to_string(command.expected_epoch.value) + " but the live coordinate is " +
                    std::to_string(generation_.value) + "/" + std::to_string(epoch_.value);
    ++commands_fenced_;
    return result;
  }

  const bool topology_change = is_topology_command(command.kind);
  const TopologyGeneration target_generation =
      topology_change ? next_generation(generation_) : generation_;

  // Preconditions that depend only on the current topology are checked here so
  // that a refusal names the offending identity.
  switch (command.kind) {
    case CommandKind::SetFabricState:
    case CommandKind::SetOptions:
    case CommandKind::SubmitEvidence:
    case CommandKind::ClearEvidence:
      break;
    case CommandKind::SetLeafState: {
      if (command.node.tier != Tier::Leaf) {
        result.code = StatusCode::Invalid;
        result.detail = "SetLeafState requires a leaf node reference";
        ++commands_refused_;
        return result;
      }
      if (fabric_->find_leaf(LeafId{command.node.index}) == nullptr) {
        result.code = StatusCode::NotFound;
        result.detail = "leaf " + to_string(command.node) + " does not exist";
        ++commands_refused_;
        return result;
      }
      break;
    }
    case CommandKind::SetSpineState: {
      if (command.node.tier != Tier::Spine) {
        result.code = StatusCode::Invalid;
        result.detail = "SetSpineState requires a spine node reference";
        ++commands_refused_;
        return result;
      }
      if (fabric_->find_spine(SpineId{command.node.index}) == nullptr) {
        result.code = StatusCode::NotFound;
        result.detail = "spine " + to_string(command.node) + " does not exist";
        ++commands_refused_;
        return result;
      }
      break;
    }
    case CommandKind::SetPortAdmin:
      if (fabric_->find_port(command.port) == nullptr) {
        result.code = StatusCode::NotFound;
        result.detail = "port " + to_string(command.port) + " does not exist";
        ++commands_refused_;
        return result;
      }
      break;
    case CommandKind::SetLinkAdmin:
    case CommandKind::RemoveLink:
      if (fabric_->find_link(command.link) == nullptr) {
        result.code = StatusCode::NotFound;
        result.detail = "link " + to_string(command.link) + " does not exist";
        ++commands_refused_;
        return result;
      }
      break;
    case CommandKind::AddLink:
      if (command.new_link.id.is_zero()) {
        result.code = StatusCode::Invalid;
        result.detail = "AddLink requires a non-zero link identity";
        ++commands_refused_;
        return result;
      }
      break;
  }

  // The effect is applied to a copy, re-validated from scratch by the builder,
  // and only then published. Nothing is visible until it validates and the
  // commit record is durable.
  auto built = apply_command_to(*fabric_, command, target_generation, epoch_, incarnation_);
  if (!built.ok()) {
    result.code = built.code();
    result.detail = std::string(built.status().detail());
    ++commands_refused_;
    return result;
  }
  auto mutated_handle = std::make_shared<const Fabric>(std::move(built.value()));
  auto index = TopologyIndex::build(mutated_handle);
  if (!index.ok()) {
    result.code = index.code();
    result.detail = std::string(index.status().detail());
    ++commands_refused_;
    return result;
  }

  hit_fault(FaultPoint::BeforeCommit);

  result.mutated = true;
  result.generation = mutated_handle->generation;
  result.epoch = epoch_;
  result.topology_digest = mutated_handle->topology_digest;
  result.code = StatusCode::Ok;
  result.detail = std::string("applied ") + std::string(to_string(command.kind));

  if (store_ != nullptr) {
    const Status appended = store_->append_command(command, result, incarnation_, epoch_);
    if (!appended.ok()) {
      result.code = appended.code();
      result.detail = "durable commit failed: " + std::string(appended.detail());
      result.mutated = false;
      ++commands_refused_;
      return result;
    }
    ++records_since_snapshot_;
  }

  hit_fault(FaultPoint::AfterCommitBeforeAck);

  {
    std::lock_guard<std::mutex> snapshot_guard(snapshot_mutex_);
    // The index shares ownership of exactly this fabric object, so a reader that
    // only holds the index keeps the topology it is reading alive.
    fabric_ = mutated_handle;
    index_ = std::make_shared<const TopologyIndex>(std::move(index.value()));
    generation_ = fabric_->generation;
  }
  if (lease_.has_value()) {
    lease_->token.generation = generation_;
    lease_->token.topology_digest = fabric_->topology_digest;
    result.refreshed_token = lease_->token;
  }
  if (command_history_.size() >= config_.max_command_history) {
    command_history_.erase(command_history_.begin());
  }
  command_history_.push_back(result.to_string());
  dedup_.emplace_back(command.request_id, result);
  ++commands_applied_;

  hit_fault(FaultPoint::AfterAckBeforeSnapshot);
  return result;
}

/// Applies one command's effect to \p current and validates the result.
///
/// This function is pure with respect to the runtime: the same call is used to
/// apply a live command and to reconstruct state while replaying a committed
/// record after a restart, which is what makes recovery produce exactly the
/// state that was committed. \p observer names the incarnation that produced
/// any evidence carried by the command, so replayed evidence stays attributed
/// to the incarnation that actually observed it.
Outcome<Fabric> ControllerRuntime::apply_command_to(const Fabric& current, const Command& command,
                                                    TopologyGeneration target_generation, Epoch epoch,
                                                    ControllerIncarnation observer) const {
  Fabric candidate = current;
  candidate.generation = target_generation;
  candidate.epoch = epoch;
  switch (command.kind) {
    case CommandKind::SetFabricState:
      candidate.state = command.fabric_state;
      break;
    case CommandKind::SetOptions:
      candidate.options = command.options;
      break;
    case CommandKind::SetLeafState:
      for (auto& leaf : candidate.leaves) {
        if (leaf.id == LeafId{command.node.index}) {
          leaf.state = command.node_state;
        }
      }
      break;
    case CommandKind::SetSpineState:
      for (auto& spine : candidate.spines) {
        if (spine.id == SpineId{command.node.index}) {
          spine.state = command.node_state;
        }
      }
      break;
    case CommandKind::SetPortAdmin:
      for (auto& port : candidate.ports) {
        if (port.id == command.port) {
          port.admin = command.port_admin;
        }
      }
      break;
    case CommandKind::SetLinkAdmin:
      for (auto& link : candidate.links) {
        if (link.id == command.link) {
          link.admin = command.link_admin;
        }
      }
      break;
    case CommandKind::RemoveLink: {
      const auto it = std::find_if(candidate.links.begin(), candidate.links.end(),
                                   [&command](const LinkRecord& link) { return link.id == command.link; });
      if (it != candidate.links.end()) {
        LinkRecord retired = *it;
        // The retired link keeps a generation older than the new one: a
        // historical record must not claim to belong to the current topology.
        retired.generation = current.generation;
        candidate.history_links.push_back(std::move(retired));
        candidate.links.erase(it);
      }
      break;
    }
    case CommandKind::AddLink:
      candidate.links.push_back(command.new_link);
      break;
    case CommandKind::SubmitEvidence: {
      LinkEvidence record = command.evidence;
      record.observer = observer;
      record.epoch = epoch;
      record.generation = target_generation;
      candidate.evidence.push_back(record);
      break;
    }
    case CommandKind::ClearEvidence: {
      const auto it = std::remove_if(candidate.evidence.begin(), candidate.evidence.end(),
                                     [&command](const LinkEvidence& record) {
                                       return record.link == command.link;
                                     });
      candidate.evidence.erase(it, candidate.evidence.end());
      break;
    }
  }
  for (auto& record : candidate.leaves) {
    record.generation = target_generation;
  }
  for (auto& record : candidate.spines) {
    record.generation = target_generation;
  }
  for (auto& record : candidate.ports) {
    record.generation = target_generation;
  }
  for (auto& record : candidate.domains) {
    record.generation = target_generation;
  }
  for (auto& record : candidate.links) {
    record.generation = target_generation;
  }
  return builder_from_modified(candidate);
}

Outcome<Fabric> ControllerRuntime::builder_from_modified(const Fabric& base) const {
  FabricBuilder builder(base.id, base.name, base.generation, base.epoch, base.options, config_.limits);
  (void)builder.set_site(base.site);
  (void)builder.set_state(base.state);
  for (const auto& record : base.domains) {
    const Status status = builder.add_failure_domain(record);
    if (!status.ok()) {
      return status;
    }
  }
  for (const auto& record : base.leaves) {
    const Status status = builder.add_leaf(record);
    if (!status.ok()) {
      return status;
    }
  }
  for (const auto& record : base.spines) {
    const Status status = builder.add_spine(record);
    if (!status.ok()) {
      return status;
    }
  }
  for (const auto& record : base.ports) {
    const Status status = builder.add_port(record);
    if (!status.ok()) {
      return status;
    }
  }
  for (const auto& record : base.history_links) {
    const Status status = builder.add_history_link(record);
    if (!status.ok()) {
      return status;
    }
  }
  for (const auto& record : base.links) {
    const Status status = builder.add_link(record);
    if (!status.ok()) {
      return status;
    }
  }
  for (const auto& record : base.evidence) {
    const Status status = builder.add_evidence(record);
    if (!status.ok()) {
      return status;
    }
  }
  return builder.freeze();
}

AuthorityContext ControllerRuntime::authority_context_locked() const {
  AuthorityContext context;
  context.controller = incarnation_;
  context.epoch = epoch_;
  context.generation = generation_;
  if (fabric_ != nullptr) {
    context.topology_digest = fabric_->topology_digest;
  }
  if (lease_.has_value()) {
    context.active_lease = lease_->lease;
  }
  context.now_unix_ms = platform::now_unix_ms();
  return context;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

Outcome<PathAssessment> ControllerRuntime::assess_paths(NodeKey from, NodeKey to,
                                                       const PathConstraints& constraints,
                                                       CancellationToken token) const {
  const auto index = current_index();
  if (index == nullptr) {
    return Status(StatusCode::NotStarted, "runtime has no topology loaded");
  }
  EligibilityEngine engine(*index, incarnation_);
  Outcome<PathAssessment> assessment = [&]() -> Outcome<PathAssessment> {
    if (from.tier == Tier::Leaf && to.tier == Tier::Leaf) {
      return engine.assess_leaf_pair(LeafId{from.index}, LeafId{to.index}, constraints, token);
    }
    if (from.tier == Tier::Leaf && to.tier == Tier::Spine) {
      return engine.assess_leaf_to_spine(LeafId{from.index}, SpineId{to.index}, constraints);
    }
    if (from.tier == Tier::Spine && to.tier == Tier::Leaf) {
      return engine.assess_leaf_to_spine(LeafId{to.index}, SpineId{from.index}, constraints);
    }
    PathAssessment unsupported;
    unsupported.from = from;
    unsupported.to = to;
    unsupported.generation = index->fabric().generation;
    unsupported.epoch = index->fabric().epoch;
    unsupported.controller = incarnation_;
    unsupported.topology_digest = index->fabric().topology_digest;
    unsupported.cls = index->structural_class(from, to);
    unsupported.verdict = EligibilityVerdict::Indeterminate;
    unsupported.code = StatusCode::Unsupported;
    unsupported.explanation.push_back("spine-to-spine path eligibility is outside the modelled scope");
    return unsupported;
  }();
  ++queries_served_;
  return assessment;
}

Outcome<std::vector<SpineEligibility>> ControllerRuntime::eligible_spines(
    NodeKey leaf, const PathConstraints& constraints, CancellationToken token) const {
  if (leaf.tier != Tier::Leaf) {
    return Status(StatusCode::Invalid, "eligible spine queries require a leaf node reference");
  }
  const auto index = current_index();
  if (index == nullptr) {
    return Status(StatusCode::NotStarted, "runtime has no topology loaded");
  }
  EligibilityEngine engine(*index, incarnation_);
  auto result = engine.eligible_spines_for_leaf(LeafId{leaf.index}, constraints, token);
  ++queries_served_;
  return result;
}

Outcome<FabricHealthReport> ControllerRuntime::health(const PathConstraints& constraints) const {
  const auto index = current_index();
  if (index == nullptr) {
    return Status(StatusCode::NotStarted, "runtime has no topology loaded");
  }
  EligibilityEngine engine(*index, incarnation_);
  auto report = engine.health(constraints, incarnation_, epoch_);
  ++queries_served_;
  return report;
}

Outcome<PathAuthority> ControllerRuntime::authorize_path(NodeKey from, NodeKey to,
                                                         const PathConstraints& constraints,
                                                         std::uint64_t ttl_ms) {
  const auto assessment = assess_paths(from, to, constraints);
  if (!assessment.ok()) {
    return assessment.status();
  }
  std::lock_guard<std::mutex> guard(writer_mutex_);
  if (!lease_.has_value()) {
    return Status(StatusCode::Refused, "no active authority lease; acquire authority first");
  }
  return mint_path_authority(assessment.value(), lease_->token, platform::now_unix_ms(), ttl_ms);
}

Status ControllerRuntime::install_fabric(Fabric fabric, bool replace) {
  std::lock_guard<std::mutex> guard(writer_mutex_);
  if (!started_.load(std::memory_order_acquire)) {
    return Status(StatusCode::NotStarted, "runtime has not been started");
  }
  if (!replace && fabric_ != nullptr && !fabric_->leaves.empty()) {
    return Status(StatusCode::AlreadyExists, "the runtime already holds a topology");
  }
  return install_fabric_locked(std::move(fabric));
}

Status ControllerRuntime::install_fabric_locked(Fabric&& fabric) {
  auto published = std::make_shared<const Fabric>(std::move(fabric));
  auto index = TopologyIndex::build(published);
  if (!index.ok()) {
    return index.status();
  }
  // Commit first, publish second: a caller is never told that installation
  // failed while the topology is already live for readers.
  if (store_ != nullptr) {
    const Status appended = store_->append_fabric(*published, incarnation_, epoch_);
    if (!appended.ok()) {
      return appended;
    }
    records_since_snapshot_ = 0;
  }
  {
    std::lock_guard<std::mutex> snapshot_guard(snapshot_mutex_);
    fabric_ = published;
    index_ = std::make_shared<const TopologyIndex>(std::move(index.value()));
    generation_ = published->generation;
  }
  return Status::success();
}

Status ControllerRuntime::compact() {
  std::lock_guard<std::mutex> guard(writer_mutex_);
  if (!started_.load(std::memory_order_acquire)) {
    return Status(StatusCode::NotStarted, "runtime has not been started");
  }
  if (store_ == nullptr) {
    return Status(StatusCode::Unsupported, "persistence is disabled for this runtime");
  }
  const Status written = SnapshotFile::write(default_snapshot_path(config_.state_dir), *fabric_,
                                             store_->last_sequence(), config_.store);
  if (!written.ok()) {
    return written;
  }
  records_since_snapshot_ = 0;
  return Status::success();
}

std::uint64_t ControllerRuntime::journal_record_count() const {
  std::lock_guard<std::mutex> guard(writer_mutex_);
  return store_ == nullptr ? 0 : store_->record_count();
}

RecoveryReport ControllerRuntime::last_recovery() const {
  std::lock_guard<std::mutex> guard(writer_mutex_);
  return recovery_;
}

RuntimeStatus ControllerRuntime::status() const {
  RuntimeStatus out;
  std::shared_ptr<const Fabric> fabric;
  std::shared_ptr<const FabricHealthReport> health_report;
  {
    std::lock_guard<std::mutex> guard(writer_mutex_);
    out.started = started_.load(std::memory_order_acquire);
    out.fabric_id = config_.fabric_id;
    out.name = config_.name;
    out.site = config_.site;
    out.controller = incarnation_;
    out.epoch = epoch_;
    out.generation = generation_;
    out.persistence_enabled = config_.enable_persistence && !config_.state_dir.empty();
    out.recovery = recovery_;
    out.worker_threads = static_cast<std::uint32_t>(workers_.size());
    out.fault_point = fault_point_;
    out.commands_applied = commands_applied_.load(std::memory_order_relaxed);
    out.commands_refused = commands_refused_.load(std::memory_order_relaxed);
    out.commands_fenced = commands_fenced_.load(std::memory_order_relaxed);
    out.commands_duplicated = commands_duplicated_.load(std::memory_order_relaxed);
    out.queries_served = queries_served_.load(std::memory_order_relaxed);
    out.uptime_ms = started_at_ms_ == 0 ? 0 : platform::monotonic_ms() - started_at_ms_;
    if (lease_.has_value()) {
      out.active_lease = lease_->lease;
      out.lease_expires_at_unix_ms = lease_->token.expires_at_unix_ms;
    }
    fabric = fabric_;
  }
  {
    std::lock_guard<std::mutex> guard(health_mutex_);
    health_report = health_;
  }
  if (fabric != nullptr) {
    out.fabric_state = fabric->state;
    out.name = fabric->name;
    out.site = fabric->site;
    out.topology_digest = fabric->topology_digest;
    out.evidence_digest = fabric->evidence_digest;
    out.leaf_count = static_cast<std::uint32_t>(fabric->leaves.size());
    out.spine_count = static_cast<std::uint32_t>(fabric->spines.size());
    out.port_count = static_cast<std::uint32_t>(fabric->ports.size());
    out.link_count = static_cast<std::uint32_t>(fabric->links.size());
    out.history_link_count = static_cast<std::uint32_t>(fabric->history_links.size());
    out.evidence_count = static_cast<std::uint32_t>(fabric->evidence.size());
  }
  if (health_report != nullptr) {
    out.health = health_report->health;
  } else if (fabric != nullptr) {
    out.health = fabric->state == FabricState::Unformed ? FabricHealth::Unformed : FabricHealth::Indeterminate;
  }
  return out;
}

}  // namespace slf
