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

#include "slf/store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>

#include "slf/bytes.hpp"
#include "slf/checked.hpp"
#include "slf/governance.hpp"
#include "slf/platform.hpp"

namespace slf {
namespace {

[[nodiscard]] std::string four_digit_segment(std::uint32_t index) {
  std::string digits = std::to_string(index);
  while (digits.size() < 8) {
    digits.insert(digits.begin(), '0');
  }
  return digits;
}

[[nodiscard]] std::optional<std::uint32_t> parse_segment_index(const std::string& name) {
  // journal-00000007.slf
  constexpr std::string_view prefix = "journal-";
  constexpr std::string_view suffix = ".slf";
  if (name.size() != prefix.size() + 8 + suffix.size()) {
    return std::nullopt;
  }
  if (name.compare(0, prefix.size(), prefix) != 0) {
    return std::nullopt;
  }
  if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
    return std::nullopt;
  }
  std::uint32_t value = 0;
  for (std::size_t i = prefix.size(); i < prefix.size() + 8; ++i) {
    const char c = name[i];
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = value * 10U + static_cast<std::uint32_t>(c - '0');
  }
  return value;
}

[[nodiscard]] std::vector<std::byte> digest_to_bytes(const Digest& digest) {
  return std::vector<std::byte>(digest.bytes.begin(), digest.bytes.end());
}

/// Reads at most max_bytes from a file. Returns the data actually read and the
/// file size, so a truncated read is detectable.
[[nodiscard]] Outcome<std::vector<std::byte>> read_file_bounded(const std::filesystem::path& path,
                                                                std::uint64_t max_bytes,
                                                                std::uint64_t* file_size_out) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return Status(StatusCode::NotFound, "cannot stat " + path.string() + ": " + ec.message());
  }
  *file_size_out = size;
  const std::uint64_t to_read = size < max_bytes ? size : max_bytes;
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    return Status(StatusCode::IoError, "cannot open " + path.string());
  }
  std::vector<std::byte> data(static_cast<std::size_t>(to_read));
  std::size_t read = 0;
  if (to_read > 0) {
    read = std::fread(data.data(), 1, data.size(), file);
  }
  std::fclose(file);
  if (read != data.size()) {
    return Status(StatusCode::IoError, "short read on " + path.string());
  }
  return data;
}

[[nodiscard]] Status write_all(std::FILE* file, const void* data, std::size_t size) {
  if (size == 0) {
    return Status::success();
  }
  if (std::fwrite(data, 1, size, file) != size) {
    return Status(StatusCode::IoError, "short write to the journal");
  }
  return Status::success();
}

}  // namespace

std::string_view to_string(RecordType type) noexcept {
  switch (type) {
    case RecordType::IncarnationStart: return "incarnation_start";
    case RecordType::CommandCommit: return "command_commit";
    case RecordType::FabricSnapshot: return "fabric_snapshot";
    case RecordType::EvidenceSubmit: return "evidence_submit";
    case RecordType::Fence: return "fence";
    case RecordType::Marker: return "marker";
  }
  return "invalid";
}

std::string_view to_string(RecoveryStatus status) noexcept {
  switch (status) {
    case RecoveryStatus::Missing: return "missing";
    case RecoveryStatus::Empty: return "empty";
    case RecoveryStatus::Clean: return "clean";
    case RecoveryStatus::CrashWithoutTrailer: return "crash_without_trailer";
    case RecoveryStatus::TruncatedTail: return "truncated_tail";
    case RecoveryStatus::ChainBroken: return "chain_broken";
    case RecoveryStatus::Corrupt: return "corrupt";
    case RecoveryStatus::IncompatibleVersion: return "incompatible_version";
    case RecoveryStatus::IoError: return "io_error";
  }
  return "invalid";
}

std::string RecoveryReport::to_string() const {
  std::ostringstream out;
  out << slf::to_string(status) << " (" << slf::to_string(code) << "): " << detail << "\n";
  out << "  journal=" << (journal_present ? "present" : "absent")
      << " snapshot=" << (snapshot_present ? (snapshot_used ? "used" : "present") : "absent")
      << " snapshot_corrupt=" << (snapshot_corrupt ? "true" : "false") << "\n";
  out << "  records_valid=" << records_valid << " records_rejected=" << records_rejected
      << " bytes_read=" << bytes_read << " bytes_dropped=" << bytes_dropped << "\n";
  out << "  dynamic_evidence_recovered=" << (dynamic_evidence_recovered ? "true" : "false")
      << " requires_reobservation=" << (requires_reobservation ? "true" : "false") << "\n";
  out << "  last_epoch=" << last_epoch.value << " last_generation=" << last_generation.value
      << " last_incarnation=" << slf::to_string(last_incarnation)
      << " last_chain=" << last_chain.short_hex(16) << "\n";
  return out.str();
}

// ---------------------------------------------------------------------------
// Journal header/record/trailer encoding
// ---------------------------------------------------------------------------

namespace {

/// Journal header layout (128 bytes):
///   0..7   magic
///   8..11  format version
///   12..15 header bytes
///   16..23 created time (unix ms)
///   24..31 controller incarnation high
///   32..39 controller incarnation low
///   40..43 segment index
///   44..47 reserved
///   48..55 first sequence number in this segment
///   56..63 reserved
///   64..95 SHA-256 over bytes 0..63
///   96..127 reserved
[[nodiscard]] std::vector<std::byte> encode_journal_header(ControllerIncarnation incarnation,
                                                           std::uint32_t segment_index,
                                                           std::uint64_t created_unix_ms,
                                                           SequenceNumber first_sequence) {
  std::vector<std::byte> header(JournalFormat::kHeaderBytes, std::byte{0});
  std::memcpy(header.data(), JournalFormat::kHeaderMagic.data(), JournalFormat::kHeaderMagic.size());
  ByteWriter writer;
  (void)writer.u32(kPersistenceFormatVersion);
  (void)writer.u32(static_cast<std::uint32_t>(JournalFormat::kHeaderBytes));
  (void)writer.u64(created_unix_ms);
  (void)writer.u64(incarnation.hi);
  (void)writer.u64(incarnation.lo);
  (void)writer.u32(segment_index);
  (void)writer.u32(0);
  (void)writer.u64(first_sequence.value);
  (void)writer.u64(0);
  std::copy(writer.buffer().begin(), writer.buffer().end(), header.begin() + 8);
  const Digest digest = Digest::of(std::span<const std::byte>(header.data(), 64));
  std::copy(digest.bytes.begin(), digest.bytes.end(), header.begin() + 64);
  return header;
}

struct JournalHeader {
  std::uint32_t format_version{0};
  std::uint64_t created_unix_ms{0};
  ControllerIncarnation incarnation{};
  std::uint32_t segment_index{0};
  SequenceNumber first_sequence{};
  Digest digest{};
};

[[nodiscard]] Outcome<JournalHeader> decode_journal_header(std::span<const std::byte> bytes) {
  if (bytes.size() < JournalFormat::kHeaderBytes) {
    return Status(StatusCode::Truncated, "journal header is shorter than the fixed size");
  }
  if (std::memcmp(bytes.data(), JournalFormat::kHeaderMagic.data(), JournalFormat::kHeaderMagic.size()) != 0) {
    return Status(StatusCode::Corrupt, "journal header magic does not match");
  }
  // The version is inspected before the digest so that a file written by an
  // incompatible version is reported as such rather than as mere corruption.
  {
    std::uint32_t declared_version = 0;
    for (unsigned i = 0; i < 4; ++i) {
      declared_version |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[8 + i])) << (8U * i);
    }
    if (declared_version != kPersistenceFormatVersion) {
      return Status(StatusCode::IncompatibleVersion,
                    "journal format version " + std::to_string(declared_version) + " is not supported");
    }
  }
  const Digest expected = Digest::of(bytes.subspan(0, 64));
  Digest stored;
  std::copy(bytes.begin() + 64, bytes.begin() + 96, stored.bytes.begin());
  if (!(expected == stored)) {
    return Status(StatusCode::IntegrityError, "journal header digest does not match its contents");
  }
  ByteReader reader(bytes.subspan(8, 56));
  const auto version = reader.u32();
  const auto header_bytes = reader.u32();
  const auto created = reader.u64();
  const auto inc_hi = reader.u64();
  const auto inc_lo = reader.u64();
  const auto segment = reader.u32();
  const auto reserved = reader.u32();
  const auto first_sequence = reader.u64();
  const auto reserved_two = reader.u64();
  if (!version.ok() || !header_bytes.ok() || !created.ok() || !inc_hi.ok() || !inc_lo.ok() ||
      !segment.ok() || !reserved.ok() || !first_sequence.ok() || !reserved_two.ok()) {
    return Status(StatusCode::Corrupt, "journal header fields are incomplete");
  }
  if (first_sequence.value() == 0) {
    return Status(StatusCode::Corrupt, "journal header declares sequence zero as its first record");
  }
  if (header_bytes.value() != JournalFormat::kHeaderBytes) {
    return Status(StatusCode::Corrupt, "journal header declares an unexpected size");
  }
  if (version.value() != kPersistenceFormatVersion) {
    return Status(StatusCode::IncompatibleVersion,
                  "journal format version " + std::to_string(version.value()) + " is not supported");
  }
  JournalHeader header;
  header.format_version = version.value();
  header.created_unix_ms = created.value();
  header.incarnation = ControllerIncarnation{inc_hi.value(), inc_lo.value()};
  header.segment_index = segment.value();
  header.first_sequence = SequenceNumber{first_sequence.value()};
  header.digest = stored;
  return header;
}

}  // namespace

// ---------------------------------------------------------------------------
// FabricStore
// ---------------------------------------------------------------------------

struct FabricStore::Impl {
  std::mutex mutex;
  std::FILE* file{nullptr};
  std::filesystem::path path;
  StoreConfig config;
  ControllerIncarnation incarnation{};
  std::uint32_t segment_index{0};
  SequenceNumber last_sequence{};
  SequenceNumber first_sequence{};
  Digest chain{};
  std::uint64_t records{0};
  bool closed{false};
  bool sync_failures{false};
  bool poisoned{false};
};

FabricStore::FabricStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

FabricStore::~FabricStore() {
  if (impl_ != nullptr && impl_->file != nullptr) {
    std::fclose(impl_->file);
    impl_->file = nullptr;
  }
}

Outcome<std::unique_ptr<FabricStore>> FabricStore::create(const std::filesystem::path& directory,
                                                         StoreConfig config,
                                                         ControllerIncarnation incarnation,
                                                         std::uint32_t segment_index,
                                                         SequenceNumber first_sequence) {
  if (incarnation.is_zero()) {
    return Status(StatusCode::Invalid, "a journal segment must name the controller incarnation that owns it");
  }
  std::error_code ec;
  if (!std::filesystem::is_directory(directory, ec)) {
    std::filesystem::create_directories(directory, ec);
    if (ec) {
      return Status(StatusCode::IoError, "cannot create state directory: " + ec.message());
    }
  }
  const std::filesystem::path path = default_journal_path(directory, segment_index);
  if (std::filesystem::exists(path, ec)) {
    return Status(StatusCode::AlreadyExists, "journal segment already exists: " + path.string());
  }
  auto impl = std::make_unique<Impl>();
  impl->path = path;
  impl->config = config;
  impl->incarnation = incarnation;
  impl->segment_index = segment_index;
  impl->first_sequence = first_sequence;
  impl->last_sequence = SequenceNumber{first_sequence.value == 0 ? 0 : first_sequence.value - 1};
  impl->file = std::fopen(path.string().c_str(), "wb");
  if (impl->file == nullptr) {
    return Status(StatusCode::IoError, "cannot create journal segment " + path.string());
  }
  const std::vector<std::byte> header =
      encode_journal_header(incarnation, segment_index, platform::now_unix_ms(), first_sequence);
  const Status written = write_all(impl->file, header.data(), header.size());
  if (!written.ok()) {
    std::fclose(impl->file);
    impl->file = nullptr;
    return written;
  }
  impl->chain = Digest::of(std::span<const std::byte>(header.data(), header.size()));
  const Status flushed = platform::sync_file(impl->file);
  if (!flushed.ok()) {
    // Leave no half-created segment behind: an orphan header-only file would
    // both leak the handle and make a retry fail with AlreadyExists.
    std::fclose(impl->file);
    impl->file = nullptr;
    (void)platform::remove_file(path);
    return flushed;
  }
  return std::unique_ptr<FabricStore>(new FabricStore(std::move(impl)));
}

Status FabricStore::append(RecordType type, std::span<const std::byte> payload,
                           ControllerIncarnation incarnation, Epoch epoch,
                           TopologyGeneration generation) {
  if (incarnation.is_zero()) {
    return Status(StatusCode::Invalid, "a journal record must name the controller incarnation that wrote it");
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->closed) {
    return Status(StatusCode::Closed, "journal segment is closed");
  }
  if (payload.size() > impl_->config.max_record_bytes) {
    return Status(StatusCode::Exhausted, "record payload exceeds the configured bound");
  }
  if (impl_->poisoned) {
    // A previous write was short: the byte stream is no longer trustworthy, so
    // later appends are refused rather than written past a silent hole.
    return Status(StatusCode::IntegrityError,
                  "journal segment is poisoned by an earlier short write");
  }
  if (impl_->records >= impl_->config.max_records_per_segment) {
    return Status(StatusCode::Exhausted, "journal segment is full and must be rotated");
  }
  std::uint64_t next_sequence = 0;
  if (!checked_add(impl_->last_sequence.value, 1, next_sequence)) {
    return Status(StatusCode::Overflow, "journal sequence overflow");
  }
  std::vector<std::byte> header(JournalFormat::kRecordHeaderBytes, std::byte{0});
  ByteWriter writer;
  (void)writer.u32(JournalFormat::kRecordMagic);
  (void)writer.u32(kPersistenceFormatVersion);
  (void)writer.u64(next_sequence);
  (void)writer.u32(static_cast<std::uint32_t>(type));
  (void)writer.u32(0);
  (void)writer.u32(static_cast<std::uint32_t>(payload.size()));
  (void)writer.u32(0);
  (void)writer.u64(generation.value);
  (void)writer.u64(epoch.value);
  (void)writer.bytes(std::span<const std::byte>(impl_->chain.bytes));
  std::copy(writer.buffer().begin(), writer.buffer().end(), header.begin());
  Sha256 sha;
  sha.update(std::span<const std::byte>(header.data(), 80));
  sha.update(payload);
  const Digest record_digest{sha.finish()};
  std::copy(record_digest.bytes.begin(), record_digest.bytes.end(), header.begin() + 80);

  Status status = write_all(impl_->file, header.data(), header.size());
  if (!status.ok()) {
    impl_->poisoned = true;
    return status;
  }
  status = write_all(impl_->file, payload.data(), payload.size());
  if (!status.ok()) {
    impl_->poisoned = true;
    return status;
  }
  status = sync_locked();
  if (!status.ok()) {
    impl_->poisoned = true;
    return status;
  }
  impl_->last_sequence = SequenceNumber{next_sequence};
  impl_->chain = record_digest;
  ++impl_->records;
  return Status::success();
}

Status FabricStore::append_marker(std::string_view text, ControllerIncarnation incarnation, Epoch epoch,
                                  TopologyGeneration generation) {
  ByteWriter writer(4096);
  const Status status = writer.string(text);
  if (!status.ok()) {
    return status;
  }
  return append(RecordType::Marker, writer.buffer(), incarnation, epoch, generation);
}

Status FabricStore::append_fabric(const Fabric& fabric, ControllerIncarnation incarnation, Epoch epoch) {
  const std::vector<std::byte> payload = serialize_fabric(fabric);
  if (payload.size() > impl_->config.max_record_bytes) {
    return Status(StatusCode::Exhausted, "fabric snapshot exceeds the journal record bound");
  }
  return append(RecordType::FabricSnapshot, payload, incarnation, epoch, fabric.generation);
}

Status FabricStore::append_command(const Command& command, const CommandResult& result,
                                   ControllerIncarnation incarnation, Epoch epoch) {
  const std::vector<std::byte> command_bytes = serialize_command(command);
  const std::vector<std::byte> result_bytes = serialize_command_result(result);
  ByteWriter writer(impl_->config.max_record_bytes < 65536 ? 65536 : impl_->config.max_record_bytes);
  Status status = writer.block(command_bytes);
  if (!status.ok()) {
    return status;
  }
  status = writer.block(result_bytes);
  if (!status.ok()) {
    return status;
  }
  return append(RecordType::CommandCommit, writer.buffer(), incarnation, epoch, result.generation);
}

Status FabricStore::close_clean() {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->closed) {
    return Status::success();
  }
  if (impl_->file == nullptr) {
    impl_->closed = true;
    return Status::success();
  }
  std::vector<std::byte> trailer(JournalFormat::kTrailerBytes, std::byte{0});
  std::memcpy(trailer.data(), JournalFormat::kTrailerMagic.data(), JournalFormat::kTrailerMagic.size());
  ByteWriter writer;
  (void)writer.u32(kPersistenceFormatVersion);
  (void)writer.u32(0);
  (void)writer.u64(impl_->records);
  (void)writer.u64(impl_->last_sequence.value);
  (void)writer.u64(impl_->first_sequence.value);
  (void)writer.bytes(std::span<const std::byte>(impl_->chain.bytes));
  std::copy(writer.buffer().begin(), writer.buffer().end(), trailer.begin() + 8);
  Sha256 sha;
  sha.update(std::span<const std::byte>(trailer.data(), JournalFormat::kTrailerBytes - 32));
  const Digest trailer_digest{sha.finish()};
  std::copy(trailer_digest.bytes.begin(), trailer_digest.bytes.end(),
            trailer.begin() + JournalFormat::kTrailerBytes - 32);
  const Status written = write_all(impl_->file, trailer.data(), trailer.size());
  if (!written.ok()) {
    return written;
  }
  const Status flushed = platform::sync_file(impl_->file);
  std::fclose(impl_->file);
  impl_->file = nullptr;
  impl_->closed = true;
  impl_->chain = trailer_digest;
  return flushed;
}

Status FabricStore::sync() {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return sync_locked();
}

Status FabricStore::sync_locked() {
  if (impl_->file == nullptr) {
    return Status(StatusCode::Closed, "journal segment is closed");
  }
  if (std::fflush(impl_->file) != 0) {
    impl_->sync_failures = true;
    return Status(StatusCode::IoError, "fflush failed");
  }
  if (!impl_->config.sync_on_commit) {
    return Status::success();
  }
  const Status status = platform::sync_file(impl_->file);
  if (!status.ok()) {
    impl_->sync_failures = true;
  }
  return status;
}

bool FabricStore::closed() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->closed;
}

std::uint64_t FabricStore::record_count() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->records;
}

SequenceNumber FabricStore::last_sequence() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->last_sequence;
}

Digest FabricStore::chain() const noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->chain;
}

const std::filesystem::path& FabricStore::path() const noexcept { return impl_->path; }

StoreConfig FabricStore::config() const noexcept { return impl_->config; }

Status FabricStore::inject_truncation_for_test(std::uint64_t bytes) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->file == nullptr) {
    return Status(StatusCode::Closed, "journal segment is already closed");
  }
  if (std::fflush(impl_->file) != 0) {
    return Status(StatusCode::IoError, "fflush failed before truncation");
  }
  std::fclose(impl_->file);
  impl_->file = nullptr;
  impl_->closed = true;
  std::error_code ec;
  const std::uint64_t size = std::filesystem::file_size(impl_->path, ec);
  if (ec) {
    return Status(StatusCode::IoError, "cannot size the journal: " + ec.message());
  }
  if (bytes > size) {
    return Status(StatusCode::Invalid, "truncation larger than the file");
  }
  std::filesystem::resize_file(impl_->path, size - bytes, ec);
  if (ec) {
    return Status(StatusCode::IoError, "cannot truncate the journal: " + ec.message());
  }
  return Status::success();
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

Outcome<RecoveryReport> recover_journal(const std::filesystem::path& path, const StoreConfig& config,
                                        std::vector<RecoveredRecord>* records_out) {
  RecoveryReport report;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    report.status = RecoveryStatus::Missing;
    report.code = StatusCode::NotFound;
    report.detail = "no journal segment at " + path.string();
    return report;
  }
  report.journal_present = true;
  std::uint64_t file_size = 0;
  const auto data = read_file_bounded(path, config.max_recovery_bytes, &file_size);
  if (!data.ok()) {
    report.status = RecoveryStatus::IoError;
    report.code = data.code();
    report.detail = std::string(data.status().detail());
    return report;
  }
  if (file_size > config.max_recovery_bytes) {
    report.status = RecoveryStatus::Corrupt;
    report.code = StatusCode::Exhausted;
    report.detail = "journal is larger than the recovery bound; refusing to interpret a prefix as complete state";
    return report;
  }
  const std::vector<std::byte>& bytes = data.value();
  report.bytes_read = bytes.size();
  if (bytes.empty()) {
    report.status = RecoveryStatus::Empty;
    report.code = StatusCode::Ok;
    report.detail = "journal segment holds no bytes";
    return report;
  }
  const auto header = decode_journal_header(std::span<const std::byte>(bytes.data(),
                                                                      std::min<std::size_t>(bytes.size(), JournalFormat::kHeaderBytes)));
  if (!header.ok()) {
    report.status = header.code() == StatusCode::IncompatibleVersion ? RecoveryStatus::IncompatibleVersion
                                                                    : RecoveryStatus::Corrupt;
    report.code = header.code();
    report.detail = std::string(header.status().detail());
    report.bytes_dropped = bytes.size();
    return report;
  }
  report.last_incarnation = header.value().incarnation;
  Digest chain = Digest::of(std::span<const std::byte>(bytes.data(), JournalFormat::kHeaderBytes));
  report.last_chain = chain;
  SequenceNumber expected_sequence = header.value().first_sequence;
  std::size_t offset = JournalFormat::kHeaderBytes;
  report.status = RecoveryStatus::CrashWithoutTrailer;
  report.code = StatusCode::Ok;
  report.detail = "records verified, no clean-shutdown trailer";

  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    if (remaining >= JournalFormat::kTrailerMagic.size() &&
        std::memcmp(bytes.data() + offset, JournalFormat::kTrailerMagic.data(),
                    JournalFormat::kTrailerMagic.size()) == 0) {
      if (remaining < JournalFormat::kTrailerBytes) {
        report.status = RecoveryStatus::TruncatedTail;
        report.code = StatusCode::Truncated;
        report.detail = "clean-shutdown trailer is incomplete";
        report.bytes_dropped += remaining;
        break;
      }
      Sha256 sha;
      sha.update(std::span<const std::byte>(bytes.data() + offset, JournalFormat::kTrailerBytes - 32));
      const Digest computed{sha.finish()};
      Digest stored;
      std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(offset + JournalFormat::kTrailerBytes - 32),
                bytes.begin() + static_cast<std::ptrdiff_t>(offset + JournalFormat::kTrailerBytes),
                stored.bytes.begin());
      if (!(computed == stored)) {
        report.status = RecoveryStatus::Corrupt;
        report.code = StatusCode::IntegrityError;
        report.detail = "clean-shutdown trailer digest does not match";
        report.bytes_dropped += remaining;
        break;
      }
      if (offset + JournalFormat::kTrailerBytes != bytes.size()) {
        report.status = RecoveryStatus::Corrupt;
        report.code = StatusCode::Invalid;
        report.detail = "bytes appear after the clean-shutdown trailer";
        report.bytes_dropped += bytes.size() - (offset + JournalFormat::kTrailerBytes);
        break;
      }
      report.status = RecoveryStatus::Clean;
      report.code = StatusCode::Ok;
      report.detail = "journal verified: header, record chain, and clean-shutdown trailer";
      offset = bytes.size();
      break;
    }
    if (remaining < JournalFormat::kRecordHeaderBytes) {
      report.status = RecoveryStatus::TruncatedTail;
      report.code = StatusCode::Truncated;
      report.detail = "final record header is incomplete (" + std::to_string(remaining) + " bytes)";
      report.bytes_dropped += remaining;
      break;
    }
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint64_t sequence = 0;
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
    std::uint32_t payload_len = 0;
    std::uint32_t reserved = 0;
    std::uint64_t generation = 0;
    std::uint64_t epoch = 0;
    const auto* base = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
    auto load_u32 = [base](std::size_t at) {
      std::uint32_t value = 0;
      for (unsigned i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(base[at + i]) << (8U * i);
      }
      return value;
    };
    auto load_u64 = [base](std::size_t at) {
      std::uint64_t value = 0;
      for (unsigned i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(base[at + i]) << (8U * i);
      }
      return value;
    };
    magic = load_u32(0);
    version = load_u32(4);
    sequence = load_u64(8);
    type = load_u32(16);
    flags = load_u32(20);
    payload_len = load_u32(24);
    reserved = load_u32(28);
    generation = load_u64(32);
    epoch = load_u64(40);
    (void)flags;
    (void)reserved;
    if (magic != JournalFormat::kRecordMagic) {
      report.status = RecoveryStatus::Corrupt;
      report.code = StatusCode::Corrupt;
      report.detail = "record magic does not match at offset " + std::to_string(offset);
      report.bytes_dropped += remaining;
      break;
    }
    if (version != kPersistenceFormatVersion) {
      report.status = RecoveryStatus::IncompatibleVersion;
      report.code = StatusCode::IncompatibleVersion;
      report.detail = "record format version " + std::to_string(version) + " is not supported";
      report.bytes_dropped += remaining;
      break;
    }
    if (payload_len > config.max_record_bytes) {
      report.status = RecoveryStatus::Corrupt;
      report.code = StatusCode::Exhausted;
      report.detail = "record declares a payload larger than the configured bound";
      report.bytes_dropped += remaining;
      break;
    }
    const std::size_t record_total = JournalFormat::kRecordHeaderBytes + payload_len;
    if (remaining < record_total) {
      report.status = RecoveryStatus::TruncatedTail;
      report.code = StatusCode::Truncated;
      report.detail = "final record is incomplete: declared " + std::to_string(record_total) +
                      " bytes, " + std::to_string(remaining) + " available";
      report.bytes_dropped += remaining;
      break;
    }
    Digest stored_chain;
    std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(offset + 48),
              bytes.begin() + static_cast<std::ptrdiff_t>(offset + 80), stored_chain.bytes.begin());
    if (!(stored_chain == chain)) {
      report.status = RecoveryStatus::ChainBroken;
      report.code = StatusCode::IntegrityError;
      report.detail = "record chain does not continue at offset " + std::to_string(offset);
      report.bytes_dropped += remaining;
      break;
    }
    Sha256 sha;
    sha.update(std::span<const std::byte>(bytes.data() + offset, 80));
    sha.update(std::span<const std::byte>(bytes.data() + offset + JournalFormat::kRecordHeaderBytes, payload_len));
    const Digest computed{sha.finish()};
    Digest stored;
    std::copy(bytes.begin() + static_cast<std::ptrdiff_t>(offset + 80),
              bytes.begin() + static_cast<std::ptrdiff_t>(offset + 112), stored.bytes.begin());
    if (!(computed == stored)) {
      report.status = RecoveryStatus::Corrupt;
      report.code = StatusCode::IntegrityError;
      report.detail = "record digest does not match its contents at offset " + std::to_string(offset);
      report.bytes_dropped += remaining;
      break;
    }
    if (sequence != expected_sequence.value) {
      report.status = RecoveryStatus::ChainBroken;
      report.code = StatusCode::Conflicting;
      report.detail = "record sequence " + std::to_string(sequence) + " does not continue from " +
                      std::to_string(expected_sequence.value) + " (replay or reordering)";
      report.bytes_dropped += remaining;
      break;
    }
    if (type == 0 || type > static_cast<std::uint32_t>(RecordType::Marker)) {
      report.status = RecoveryStatus::Corrupt;
      report.code = StatusCode::Invalid;
      report.detail = "record carries an unknown record type";
      report.bytes_dropped += remaining;
      break;
    }
    if (records_out != nullptr) {
      if (records_out->size() >= 1000000U) {
        report.status = RecoveryStatus::Corrupt;
        report.code = StatusCode::Exhausted;
        report.detail = "record count exceeds the in-memory replay bound";
        report.bytes_dropped += remaining;
        break;
      }
      RecoveredRecord record;
      record.type = static_cast<RecordType>(type);
      record.seq = SequenceNumber{sequence};
      record.generation = TopologyGeneration{generation};
      record.epoch = Epoch{epoch};
      record.incarnation = header.value().incarnation;
      record.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset + JournalFormat::kRecordHeaderBytes),
                            bytes.begin() + static_cast<std::ptrdiff_t>(offset + record_total));
      record.chain = computed;
      records_out->push_back(std::move(record));
    }
    chain = computed;
    report.last_chain = chain;
    report.last_epoch = Epoch{epoch};
    report.last_generation = TopologyGeneration{generation};
    ++report.records_valid;
    ++expected_sequence.value;
    offset += record_total;
    if (type == static_cast<std::uint32_t>(RecordType::EvidenceSubmit)) {
      report.dynamic_evidence_recovered = true;
      report.requires_reobservation = true;
    }
  }
  if (report.status == RecoveryStatus::Clean) {
    report.requires_reobservation = report.dynamic_evidence_recovered;
  }
  return report;
}

Outcome<std::vector<SegmentInfo>> list_journal_segments(const std::filesystem::path& directory,
                                                       std::uint32_t max_segments) {
  std::error_code ec;
  if (!std::filesystem::is_directory(directory, ec)) {
    return Status(StatusCode::NotFound, "state directory does not exist");
  }
  std::vector<SegmentInfo> segments;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    if (ec) {
      return Status(StatusCode::IoError, "directory iteration failed: " + ec.message());
    }
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    const auto index = parse_segment_index(name);
    if (!index.has_value()) {
      continue;
    }
    SegmentInfo info;
    info.path = entry.path();
    info.segment_index = *index;
    info.bytes = entry.file_size(ec);
    segments.push_back(std::move(info));
  }
  std::sort(segments.begin(), segments.end(), [](const SegmentInfo& a, const SegmentInfo& b) {
    return a.segment_index < b.segment_index;
  });
  if (segments.size() > max_segments) {
    return Status(StatusCode::Exhausted,
                  "state directory holds " + std::to_string(segments.size()) +
                      " journal segments, more than the configured maximum of " +
                      std::to_string(max_segments));
  }
  return segments;
}

Outcome<RecoveryOutcome> recover_store(const std::filesystem::path& directory, const StoreConfig& config,
                                       ReplaySink* sink) {
  RecoveryOutcome outcome;
  RecoveryReport& report = outcome.report;
  std::error_code ec;
  if (!std::filesystem::is_directory(directory, ec)) {
    report.status = RecoveryStatus::Missing;
    report.code = StatusCode::NotFound;
    report.detail = "state directory does not exist: " + directory.string();
    return outcome;
  }

  const std::filesystem::path snapshot_path = default_snapshot_path(directory);
  if (std::filesystem::exists(snapshot_path, ec)) {
    report.snapshot_present = true;
    SequenceNumber covered{};
    std::string detail;
    const auto fabric = SnapshotFile::read(snapshot_path, config, &covered, &detail);
    if (fabric.ok()) {
      report.snapshot_used = true;
      outcome.snapshot_accepted = true;
      outcome.snapshot_watermark = covered;
      outcome.snapshot_fabric = fabric.value();
      report.detail = "snapshot accepted (covers records up to sequence " +
                      std::to_string(covered.value) + ")";
    } else {
      report.snapshot_corrupt = true;
      report.detail = "snapshot rejected: " + detail;
    }
  }

  const auto segments = list_journal_segments(directory, config.max_segments);
  if (!segments.ok()) {
    report.status = segments.code() == StatusCode::Exhausted ? RecoveryStatus::Corrupt
                                                            : RecoveryStatus::Missing;
    report.code = segments.code();
    report.detail = std::string(segments.status().detail());
    return outcome;
  }
  outcome.segments = segments.value();
  if (outcome.segments.empty()) {
    if (!report.snapshot_used) {
      report.status = RecoveryStatus::Missing;
      report.code = StatusCode::NotFound;
      if (report.detail.empty()) {
        report.detail = "no journal segment and no snapshot in " + directory.string();
      }
    } else {
      report.status = RecoveryStatus::Clean;
      report.code = StatusCode::Ok;
    }
    return outcome;
  }

  SequenceNumber expected_next_sequence{};
  bool saw_clean = true;
  bool any_records = false;
  for (auto& segment : outcome.segments) {
    std::vector<RecoveredRecord> records;
    const auto segment_report = recover_journal(segment.path, config, &records);
    if (!segment_report.ok()) {
      report.status = RecoveryStatus::IoError;
      report.code = segment_report.code();
      report.detail = std::string(segment_report.status().detail());
      return Status(report.code, report.detail);
    }
    segment.status = segment_report.value().status;
    segment.usable = segment_report.value().usable();
    segment.records = segment_report.value().records_valid;
    if (!records.empty()) {
      segment.first_sequence = records.front().seq;
      segment.last_sequence = records.back().seq;
    }
    any_records = any_records || !records.empty();
    report.records_valid += segment_report.value().records_valid;
    report.records_rejected += segment_report.value().records_rejected;
    report.bytes_read += segment_report.value().bytes_read;
    report.bytes_dropped += segment_report.value().bytes_dropped;
    if (segment_report.value().status != RecoveryStatus::Clean) {
      saw_clean = false;
    }
    if (segment_report.value().dynamic_evidence_recovered) {
      report.dynamic_evidence_recovered = true;
      report.requires_reobservation = true;
    }
    if (segment_report.value().last_epoch.value > report.last_epoch.value) {
      report.last_epoch = segment_report.value().last_epoch;
    }
    if (segment_report.value().last_generation.value > report.last_generation.value) {
      report.last_generation = segment_report.value().last_generation;
    }
    report.last_incarnation = segment_report.value().last_incarnation;
    report.last_chain = segment_report.value().last_chain;
    const auto severity = [](RecoveryStatus status) {
      switch (status) {
        case RecoveryStatus::Clean: return 0;
        case RecoveryStatus::Empty: return 1;
        case RecoveryStatus::Missing: return 2;
        case RecoveryStatus::CrashWithoutTrailer: return 3;
        case RecoveryStatus::TruncatedTail: return 4;
        case RecoveryStatus::ChainBroken: return 5;
        case RecoveryStatus::IncompatibleVersion: return 6;
        case RecoveryStatus::Corrupt: return 7;
        case RecoveryStatus::IoError: return 8;
      }
      return 9;
    };
    if (severity(segment_report.value().status) > severity(report.status)) {
      report.status = segment_report.value().status;
      report.code = segment_report.value().code;
    }
    if (expected_next_sequence.value != 0 && segment.first_sequence.value != 0 &&
        segment.first_sequence.value != expected_next_sequence.value) {
      report.status = RecoveryStatus::ChainBroken;
      report.code = StatusCode::Conflicting;
      report.detail = "journal segment numbering does not continue across segments";
      return Status(report.code, report.detail);
    }
    if (segment.last_sequence.value != 0) {
      expected_next_sequence = SequenceNumber{segment.last_sequence.value + 1};
    }
    if (outcome.snapshot_accepted) {
      if (segment.last_sequence.value != 0 && segment.last_sequence.value <= outcome.snapshot_watermark.value) {
        outcome.prunable_segments.push_back(segment.path);
      }
    }
    for (auto& record : records) {
      if (outcome.snapshot_accepted && record.seq.value <= outcome.snapshot_watermark.value) {
        continue;
      }
      // A record from an older segment that predates the snapshot watermark is
      // superseded; the rest are handed to the sink in segment order.
      if (sink != nullptr) {
        const Status consumed = sink->on_record(record);
        if (!consumed.ok()) {
          report.status = RecoveryStatus::Corrupt;
          report.code = consumed.code();
          report.detail = "replay sink refused a record: " + std::string(consumed.detail());
          return Status(report.code, report.detail);
        }
      }
    }
  }
  if (report.status != RecoveryStatus::Corrupt && report.status != RecoveryStatus::ChainBroken &&
      report.status != RecoveryStatus::IncompatibleVersion) {
    report.status = saw_clean ? RecoveryStatus::Clean : report.status;
    if (report.status == RecoveryStatus::Clean && !any_records && report.snapshot_used) {
      report.detail = "snapshot accepted and no newer journal records";
    }
  }
  if (report.requires_reobservation) {
    report.detail += "; recovered dynamic evidence is historical and must be re-observed";
  }
  return outcome;
}

// ---------------------------------------------------------------------------
// Snapshot file
// ---------------------------------------------------------------------------

Status SnapshotFile::write(const std::filesystem::path& path, const Fabric& fabric, SequenceNumber covered_seq,
                           const StoreConfig& config) {
  const std::vector<std::byte> payload = serialize_fabric(fabric);
  if (payload.size() > config.max_snapshot_bytes) {
    return Status(StatusCode::Exhausted, "snapshot payload exceeds the configured bound");
  }
  std::vector<std::byte> header(kHeaderBytes, std::byte{0});
  std::memcpy(header.data(), JournalFormat::kSnapshotMagic.data(), JournalFormat::kSnapshotMagic.size());
  ByteWriter writer;
  (void)writer.u32(kPersistenceFormatVersion);
  (void)writer.u32(static_cast<std::uint32_t>(kHeaderBytes));
  (void)writer.u64(payload.size());
  (void)writer.u64(fabric.generation.value);
  (void)writer.u64(fabric.epoch.value);
  (void)writer.u64(covered_seq.value);
  (void)writer.u64(0);
  std::copy(writer.buffer().begin(), writer.buffer().end(), header.begin() + 8);
  const Digest payload_digest = Digest::of(std::span<const std::byte>(payload));
  std::copy(payload_digest.bytes.begin(), payload_digest.bytes.end(), header.begin() + 64);
  const Digest header_digest = Digest::of(std::span<const std::byte>(header.data(), 96));
  std::copy(header_digest.bytes.begin(), header_digest.bytes.end(), header.begin() + 96);

  const std::filesystem::path temporary = path.string() + ".tmp";
  std::FILE* file = std::fopen(temporary.string().c_str(), "wb");
  if (file == nullptr) {
    return Status(StatusCode::IoError, "cannot create snapshot temporary file");
  }
  Status status = write_all(file, header.data(), header.size());
  if (status.ok()) {
    status = write_all(file, payload.data(), payload.size());
  }
  if (status.ok()) {
    status = platform::sync_file(file);
  }
  std::fclose(file);
  if (!status.ok()) {
    (void)platform::remove_file(temporary);
    return status;
  }
  const Status replaced = platform::replace_file(temporary, path);
  if (!replaced.ok()) {
    (void)platform::remove_file(temporary);
    return replaced;
  }
  return Status::success();
}

Outcome<Fabric> SnapshotFile::read(const std::filesystem::path& path, const StoreConfig& config,
                                   SequenceNumber* covered_seq_out, std::string* detail_out) {
  std::uint64_t file_size = 0;
  const auto data = read_file_bounded(path, config.max_snapshot_bytes + kHeaderBytes, &file_size);
  if (!data.ok()) {
    if (detail_out != nullptr) {
      *detail_out = std::string(data.status().detail());
    }
    return data.status();
  }
  const std::vector<std::byte>& bytes = data.value();
  if (bytes.size() < kHeaderBytes) {
    if (detail_out != nullptr) {
      *detail_out = "snapshot header is incomplete";
    }
    return Status(StatusCode::Truncated, "snapshot header is incomplete");
  }
  if (std::memcmp(bytes.data(), JournalFormat::kSnapshotMagic.data(), JournalFormat::kSnapshotMagic.size()) != 0) {
    if (detail_out != nullptr) {
      *detail_out = "snapshot magic does not match";
    }
    return Status(StatusCode::Corrupt, "snapshot magic does not match");
  }
  const Digest header_digest = Digest::of(std::span<const std::byte>(bytes.data(), 96));
  Digest stored_header_digest;
  std::copy(bytes.begin() + 96, bytes.begin() + 128, stored_header_digest.bytes.begin());
  if (!(header_digest == stored_header_digest)) {
    if (detail_out != nullptr) {
      *detail_out = "snapshot header digest does not match";
    }
    return Status(StatusCode::IntegrityError, "snapshot header digest does not match");
  }
  ByteReader reader(std::span<const std::byte>(bytes.data() + 8, 56));
  const auto version = reader.u32();
  const auto header_bytes = reader.u32();
  const auto payload_len = reader.u64();
  const auto generation = reader.u64();
  const auto epoch = reader.u64();
  const auto covered = reader.u64();
  const auto reserved = reader.u64();
  if (!version.ok() || !header_bytes.ok() || !payload_len.ok() || !generation.ok() || !epoch.ok() ||
      !covered.ok() || !reserved.ok()) {
    if (detail_out != nullptr) {
      *detail_out = "snapshot header fields are incomplete";
    }
    return Status(StatusCode::Corrupt, "snapshot header fields are incomplete");
  }
  if (version.value() != kPersistenceFormatVersion) {
    if (detail_out != nullptr) {
      *detail_out = "snapshot format version is not supported";
    }
    return Status(StatusCode::IncompatibleVersion, "snapshot format version is not supported");
  }
  if (header_bytes.value() != kHeaderBytes) {
    if (detail_out != nullptr) {
      *detail_out = "snapshot header declares an unexpected size";
    }
    return Status(StatusCode::Corrupt, "snapshot header declares an unexpected size");
  }
  if (payload_len.value() > config.max_snapshot_bytes) {
    if (detail_out != nullptr) {
      *detail_out = "snapshot payload exceeds the configured bound";
    }
    return Status(StatusCode::Exhausted, "snapshot payload exceeds the configured bound");
  }
  if (bytes.size() != kHeaderBytes + payload_len.value()) {
    if (detail_out != nullptr) {
      *detail_out = "snapshot length does not match its declared payload size";
    }
    return Status(StatusCode::Truncated, "snapshot length does not match its declared payload size");
  }
  const std::span<const std::byte> payload(bytes.data() + kHeaderBytes, static_cast<std::size_t>(payload_len.value()));
  const Digest payload_digest = Digest::of(payload);
  Digest stored_payload_digest;
  std::copy(bytes.begin() + 64, bytes.begin() + 96, stored_payload_digest.bytes.begin());
  if (!(payload_digest == stored_payload_digest)) {
    if (detail_out != nullptr) {
      *detail_out = "snapshot payload digest does not match";
    }
    return Status(StatusCode::IntegrityError, "snapshot payload digest does not match");
  }
  BuilderLimits limits;
  const auto fabric = deserialize_fabric(payload, limits);
  if (!fabric.ok()) {
    if (detail_out != nullptr) {
      *detail_out = "snapshot payload could not be parsed: " + std::string(fabric.status().detail());
    }
    return fabric.status();
  }
  if (fabric.value().generation.value != generation.value()) {
    if (detail_out != nullptr) {
      *detail_out = "snapshot generation does not match its header";
    }
    return Status(StatusCode::IntegrityError, "snapshot generation does not match its header");
  }
  if (covered_seq_out != nullptr) {
    *covered_seq_out = SequenceNumber{covered.value()};
  }
  if (detail_out != nullptr) {
    *detail_out = "snapshot verified";
  }
  return fabric.value();
}

std::filesystem::path default_journal_path(const std::filesystem::path& directory, std::uint32_t segment_index) {
  return directory / ("journal-" + four_digit_segment(segment_index) + ".slf");
}

std::filesystem::path default_snapshot_path(const std::filesystem::path& directory) {
  return directory / "snapshot.slf";
}

}  // namespace slf
