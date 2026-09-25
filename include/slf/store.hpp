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

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "slf/governance.hpp"
#include "slf/model.hpp"
#include "slf/status.hpp"

namespace slf {

/// Journal record types. The numeric values are part of the on-disk format.
enum class RecordType : std::uint32_t {
  /// Marks a controller incarnation and epoch transition.
  IncarnationStart = 1,
  /// A committed governed command together with its recorded result.
  CommandCommit = 2,
  /// A full fabric snapshot stored inline in the journal.
  FabricSnapshot = 3,
  /// Submitted link evidence.
  EvidenceSubmit = 4,
  /// An explicit fence: everything issued before this record is void.
  Fence = 5,
  /// An operator/administrative marker with free-form text.
  Marker = 6,
};

[[nodiscard]] std::string_view to_string(RecordType type) noexcept;

/// Classification of a recovery attempt. Only c Clean means "the writer shut
/// down normally"; every other usable status means the process died and the
/// recovered state must be treated as a conservative reconstruction.
enum class RecoveryStatus : std::uint8_t {
  /// No journal file exists.
  Missing = 0,
  /// The journal exists but holds no records.
  Empty = 1,
  /// Header, all records, chain, and trailer verified.
  Clean = 2,
  /// Valid records but no trailer: the writer died (or was killed) before it
  /// could close cleanly. The valid prefix is trustworthy and replayable.
  CrashWithoutTrailer = 3,
  /// A final record was only partially written. The partial record is dropped.
  TruncatedTail = 4,
  /// Sequence or chain hash did not continue: replay, reordering, or a
  /// duplicated record.
  ChainBroken = 5,
  /// Structurally invalid bytes.
  Corrupt = 6,
  /// Written by an incompatible format version.
  IncompatibleVersion = 7,
  /// An operating-system level read failure.
  IoError = 8,
};

[[nodiscard]] std::string_view to_string(RecoveryStatus status) noexcept;

/// Verdict of one recovered record.
struct RecoveredRecord {
  RecordType type{RecordType::Marker};
  SequenceNumber seq{};
  TopologyGeneration generation{};
  Epoch epoch{};
  ControllerIncarnation incarnation{};
  std::vector<std::byte> payload;
  bool from_snapshot{false};
  Digest chain{};
};

/// Structured recovery report. Every field is evidence about what survived, and
/// nothing in this structure is inferred optimistically.
struct RecoveryReport {
  RecoveryStatus status{RecoveryStatus::Missing};
  StatusCode code{StatusCode::Ok};
  std::string detail;
  bool journal_present{false};
  bool snapshot_present{false};
  bool snapshot_used{false};
  bool snapshot_corrupt{false};
  std::uint64_t records_valid{0};
  std::uint64_t records_rejected{0};
  std::uint64_t bytes_read{0};
  std::uint64_t bytes_dropped{0};
  /// True when dynamic (observed) evidence was recovered. Recovered dynamic
  /// evidence is historical by definition: it was observed by a previous
  /// incarnation and must be re-observed before it can support eligibility.
  bool dynamic_evidence_recovered{false};
  bool requires_reobservation{false};
  Epoch last_epoch{};
  TopologyGeneration last_generation{};
  ControllerIncarnation last_incarnation{};
  Digest last_chain{};

  /// True when the recovered prefix may be applied. Never implies freshness.
  [[nodiscard]] bool usable() const noexcept {
    return status == RecoveryStatus::Clean || status == RecoveryStatus::CrashWithoutTrailer ||
           status == RecoveryStatus::TruncatedTail || status == RecoveryStatus::Empty;
  }
  [[nodiscard]] std::string to_string() const;
};

/// Persistence limits. Every one of them is enforced before allocation.
struct StoreConfig {
  /// Maximum size of a single record payload.
  std::uint64_t max_record_bytes{1U * 1024U * 1024U};
  /// Maximum number of records in one segment before rotation.
  std::uint64_t max_records_per_segment{100000};
  /// Maximum number of retained segments. Older segments are only deleted once
  /// a newer segment exists and a snapshot covers them.
  std::uint32_t max_segments{4};
  /// Maximum total bytes read during recovery.
  std::uint64_t max_recovery_bytes{256U * 1024U * 1024U};
  /// Maximum accepted snapshot payload size.
  std::uint64_t max_snapshot_bytes{64U * 1024U * 1024U};
  /// Flush to stable storage on every commit. Disabling this is a test-only
  /// option and is recorded in the report.
  bool sync_on_commit{true};
};

/// Versioned, integrity-checked, bounded journal with snapshot support.
///
/// Thread safety: a single internal mutex serialises appends, rotation, and
/// close. The store never invokes a caller callback while holding that mutex,
/// so a callback can freely call back into the store without deadlocking.
class FabricStore {
 public:
  /// Creates the journal segment p segment_index and writes its header.
  ///
  /// Sequence numbers are global across segments: p first_sequence continues
  /// the numbering of the previous segment so that a snapshot sequence
  /// watermark stays meaningful across rotation. The call refuses to touch an
  /// existing file (AlreadyExists) rather than appending to or truncating state
  /// written by someone else.
  [[nodiscard]] static Outcome<std::unique_ptr<FabricStore>> create(const std::filesystem::path& directory,
                                                                  StoreConfig config,
                                                                  ControllerIncarnation incarnation,
                                                                  std::uint32_t segment_index,
                                                                  SequenceNumber first_sequence);

  ~FabricStore();
  FabricStore(const FabricStore&) = delete;
  FabricStore& operator=(const FabricStore&) = delete;

  /// Appends a record. The chain hash binds it to every preceding record.
  [[nodiscard]] Status append(RecordType type, std::span<const std::byte> payload,
                              ControllerIncarnation incarnation, Epoch epoch, TopologyGeneration generation);

  /// Convenience overloads.
  [[nodiscard]] Status append_marker(std::string_view text, ControllerIncarnation incarnation, Epoch epoch,
                                     TopologyGeneration generation);
  [[nodiscard]] Status append_fabric(const Fabric& fabric, ControllerIncarnation incarnation, Epoch epoch);
  [[nodiscard]] Status append_command(const Command& command, const CommandResult& result,
                                      ControllerIncarnation incarnation, Epoch epoch);

  /// Writes the clean-shutdown trailer, flushes, and closes the file.
  /// Idempotent: a second call is a no-op returning c Ok.
  [[nodiscard]] Status close_clean();

  /// Flushes buffered bytes and, when c sync_on_commit is set, forces them to
  /// stable storage.
  [[nodiscard]] Status sync();

  [[nodiscard]] bool closed() const noexcept;
  [[nodiscard]] std::uint64_t record_count() const noexcept;
  [[nodiscard]] SequenceNumber last_sequence() const noexcept;
  [[nodiscard]] Digest chain() const noexcept;
  [[nodiscard]] const std::filesystem::path& path() const noexcept;
  [[nodiscard]] StoreConfig config() const noexcept;

  /// Injects a torn write for tests: truncates the underlying file by
  /// p bytes, simulating a process that died mid-record.
  [[nodiscard]] Status inject_truncation_for_test(std::uint64_t bytes);

 private:
  [[nodiscard]] Status sync_locked();

  struct Impl;
  explicit FabricStore(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

/// Journal layout constants (exposed for tests and tooling).
struct JournalFormat {
  static constexpr std::size_t kHeaderBytes = 128;
  static constexpr std::size_t kRecordHeaderBytes = 112;
  static constexpr std::size_t kTrailerBytes = 96;
  static constexpr std::string_view kHeaderMagic = "SLFJRN01";
  static constexpr std::string_view kTrailerMagic = "SLFEND01";
  static constexpr std::string_view kSnapshotMagic = "SLFSNP01";
  static constexpr std::uint32_t kRecordMagic = 0x534C4652U;  // 'SLFR'
};

/// Snapshot payload file: header plus the canonical fabric encoding.
struct SnapshotFile {
  static constexpr std::size_t kHeaderBytes = 128;

  /// Writes a snapshot atomically: the payload goes to a temporary file which
  /// is flushed, then renamed over the destination. A crash therefore leaves
  /// either the old snapshot or the new one, never a torn one.
  [[nodiscard]] static Status write(const std::filesystem::path& path, const Fabric& fabric,
                                    SequenceNumber covered_seq, const StoreConfig& config);

  [[nodiscard]] static Outcome<Fabric> read(const std::filesystem::path& path, const StoreConfig& config,
                                            SequenceNumber* covered_seq_out, std::string* detail_out);
};

/// Reads and verifies a journal file without applying anything.
///
/// Records are returned in order. A partially written final record is dropped
/// and reported; a chain break, bad digest, or bad structure stops the scan
/// conservatively at that point and is reported.
[[nodiscard]] Outcome<RecoveryReport> recover_journal(const std::filesystem::path& path, const StoreConfig& config,
                                                      std::vector<RecoveredRecord>* records_out);

/// Replay callback interface used by the runtime. Implementations must be
/// prepared to see a conservative prefix and must decide what to do with
/// historical dynamic evidence.
class ReplaySink {
 public:
  virtual ~ReplaySink() = default;
  virtual Status on_record(const RecoveredRecord& record) = 0;
};

/// Recovers a store directory: verifies the snapshot, replays the journal after
/// the snapshot sequence, and reports exactly what survived.
/// One journal segment as found on disk.
struct SegmentInfo {
  std::filesystem::path path;
  std::uint32_t segment_index{0};
  std::uint64_t bytes{0};
  SequenceNumber first_sequence{};
  SequenceNumber last_sequence{};
  std::uint64_t records{0};
  RecoveryStatus status{RecoveryStatus::Missing};
  bool usable{false};
};

/// Result of a full store recovery: what survived, which segments were read,
/// and the snapshot that was accepted (if any). When a snapshot is accepted the
/// records handed to the sink are only those newer than the snapshot watermark;
/// the fabric itself is returned to the caller instead of being replayed.
struct RecoveryOutcome {
  RecoveryReport report{};
  std::vector<SegmentInfo> segments{};
  bool snapshot_accepted{false};
  SequenceNumber snapshot_watermark{};
  std::optional<Fabric> snapshot_fabric{};
  /// Segment paths that are entirely covered by an accepted snapshot and may
  /// therefore be pruned without losing durable state.
  std::vector<std::filesystem::path> prunable_segments{};
};

[[nodiscard]] Outcome<RecoveryOutcome> recover_store(const std::filesystem::path& directory, const StoreConfig& config,
                                                     ReplaySink* sink);

/// Default file names inside a state directory.
[[nodiscard]] std::filesystem::path default_journal_path(const std::filesystem::path& directory,
                                                        std::uint32_t segment_index);
[[nodiscard]] std::filesystem::path default_snapshot_path(const std::filesystem::path& directory);

/// Lists journal segments in ascending segment order. Returns at most
/// max_segments entries; a directory holding more segments than that is
/// reported as Exhausted rather than being silently truncated, because dropping
/// journal segments would drop durable state.
[[nodiscard]] Outcome<std::vector<SegmentInfo>> list_journal_segments(const std::filesystem::path& directory,
                                                                     std::uint32_t max_segments);

}  // namespace slf
