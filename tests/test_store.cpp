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
#include <filesystem>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "slf/builder.hpp"
#include "slf/spec.hpp"
#include "slf_test.hpp"

using namespace slf;
using namespace slf::test;

namespace {

/// Writes p count records into a fresh journal and returns the directory.
struct JournalFixture {
  TempDir dir{"journal"};
  StoreConfig config;
  ControllerIncarnation incarnation{mint_incarnation()};

  [[nodiscard]] Outcome<std::unique_ptr<FabricStore>> create(std::uint32_t segment = 1,
                                                            SequenceNumber first = SequenceNumber{1}) {
    return FabricStore::create(dir.path(), config, incarnation, segment, first);
  }
};

[[nodiscard]] std::vector<std::byte> file_bytes(const std::filesystem::path& path) {
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    return {};
  }
  std::vector<std::byte> data;
  std::byte buffer[4096];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    data.insert(data.end(), buffer, buffer + read);
  }
  std::fclose(file);
  return data;
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& data) {
  std::FILE* file = std::fopen(path.string().c_str(), "wb");
  if (file == nullptr) {
    return;
  }
  if (!data.empty()) {
    (void)std::fwrite(data.data(), 1, data.size(), file);
  }
  std::fclose(file);
}

// Fixed payloads used by several journal tests.
const std::vector<std::byte> kOneByte{std::byte{0x01}};
const std::vector<std::byte> kSevenByte{std::byte{0x07}};
const std::vector<std::byte> kAbByte{std::byte{0xAB}};
}  // namespace

SLF_TEST(store_clean_roundtrip) {
  JournalFixture fixture;
  auto store = fixture.create();
  SLF_EXPECT(store.ok());
  for (int i = 0; i < 5; ++i) {
    const std::vector<std::byte> payload{static_cast<std::byte>(i), static_cast<std::byte>(i + 1)};
    SLF_EXPECT(store.value()->append(RecordType::Marker, payload, fixture.incarnation, Epoch{1},
                                     TopologyGeneration{1})
                   .ok());
  }
  SLF_EXPECT_EQ(store.value()->record_count(), 5U);
  SLF_EXPECT_EQ(store.value()->last_sequence(), SequenceNumber{5});
  SLF_EXPECT(!store.value()->chain().is_zero());
  const std::filesystem::path path = store.value()->path();
  SLF_EXPECT(store.value()->close_clean().ok());

  std::vector<RecoveredRecord> records;
  const auto report = recover_journal(path, fixture.config, &records);
  SLF_EXPECT(report.ok());
  SLF_EXPECT_EQ(report.value().status, RecoveryStatus::Clean);
  SLF_EXPECT_EQ(report.value().records_valid, 5U);
  SLF_EXPECT_EQ(records.size(), 5U);
  SLF_EXPECT_EQ(records.front().seq, SequenceNumber{1});
  SLF_EXPECT_EQ(records.back().seq, SequenceNumber{5});
  SLF_EXPECT_EQ(records.front().type, RecordType::Marker);
  SLF_EXPECT_EQ(records.front().incarnation, fixture.incarnation);
  SLF_EXPECT(report.value().usable());
  SLF_EXPECT(report.value().to_string().find("clean") != std::string::npos);

  // A second clean close is a no-op.
  SLF_EXPECT(store.value()->close_clean().ok());
  // Appending to a closed store is refused.
  SLF_EXPECT_CODE(store.value()->append(RecordType::Marker, std::span<const std::byte>(), fixture.incarnation, Epoch{1},
                                        TopologyGeneration{1}),
                  StatusCode::Closed);
}

SLF_TEST(store_refuses_to_hijack_an_existing_segment) {
  JournalFixture fixture;
  auto first = fixture.create();
  SLF_EXPECT(first.ok());
  SLF_EXPECT(first.value()->close_clean().ok());
  auto second = fixture.create();
  SLF_EXPECT_CODE(second, StatusCode::AlreadyExists);
}

SLF_TEST(store_crash_without_trailer_is_usable_but_not_clean) {
  JournalFixture fixture;
  auto store = fixture.create();
  SLF_EXPECT(store.ok());
  for (int i = 0; i < 3; ++i) {
    SLF_EXPECT(store.value()->append(RecordType::Marker, std::span<const std::byte>(), fixture.incarnation, Epoch{2},
                                     TopologyGeneration{1})
                   .ok());
  }
  const std::filesystem::path path = store.value()->path();
  // Simulate a killed process: no trailer is ever written.
  store.value().reset();

  std::vector<RecoveredRecord> records;
  const auto report = recover_journal(path, fixture.config, &records);
  SLF_EXPECT(report.ok());
  SLF_EXPECT_EQ(report.value().status, RecoveryStatus::CrashWithoutTrailer);
  SLF_EXPECT_EQ(report.value().records_valid, 3U);
  SLF_EXPECT_EQ(records.size(), 3U);
  SLF_EXPECT(report.value().usable());
  SLF_EXPECT_EQ(report.value().last_epoch, Epoch{2});
}

SLF_TEST(store_torn_tail_is_dropped_not_accepted) {
  JournalFixture fixture;
  auto store = fixture.create();
  SLF_EXPECT(store.ok());
  for (int i = 0; i < 3; ++i) {
    SLF_EXPECT(store.value()->append(RecordType::Marker, kAbByte, fixture.incarnation, Epoch{1},
                                     TopologyGeneration{1})
                   .ok());
  }
  const std::filesystem::path path = store.value()->path();
  const auto good = file_bytes(path);
  store.value().reset();

  // Truncate inside the final record: the prefix must still be recovered and the
  // partial record must be dropped.
  for (std::size_t cut = 1; cut <= 64; ++cut) {
    std::vector<std::byte> torn(good.begin(), good.end() - static_cast<std::ptrdiff_t>(cut));
    write_bytes(path, torn);
    std::vector<RecoveredRecord> records;
    const auto report = recover_journal(path, fixture.config, &records);
    SLF_EXPECT(report.ok());
    SLF_EXPECT(report.value().status == RecoveryStatus::TruncatedTail ||
               report.value().status == RecoveryStatus::Clean);
    SLF_EXPECT(report.value().records_valid <= 3U);
    SLF_EXPECT_EQ(records.size(), static_cast<std::size_t>(report.value().records_valid));
    SLF_EXPECT(report.value().bytes_dropped > 0U);
    // A dropped record is never reported as a valid one.
    SLF_EXPECT(report.value().records_valid < 3U);
  }
  write_bytes(path, good);
  std::vector<RecoveredRecord> restored;
  SLF_EXPECT_EQ(recover_journal(path, fixture.config, &restored).value().records_valid, 3U);
}

SLF_TEST(store_corruption_is_detected_at_every_offset) {
  JournalFixture fixture;
  auto store = fixture.create();
  SLF_EXPECT(store.ok());
  for (int i = 0; i < 2; ++i) {
    const std::vector<std::byte> payload(8, std::byte{0x5A});
    SLF_EXPECT(store.value()->append(RecordType::Marker, payload, fixture.incarnation, Epoch{1},
                                     TopologyGeneration{1})
                   .ok());
  }
  const std::filesystem::path path = store.value()->path();
  const auto good = file_bytes(path);
  store.value().reset();

  std::size_t integrity_failures = 0;
  for (std::size_t offset = 0; offset < good.size(); ++offset) {
    std::vector<std::byte> corrupted = good;
    corrupted[offset] = static_cast<std::byte>(static_cast<unsigned>(corrupted[offset]) ^ 0xFFU);
    write_bytes(path, corrupted);
    std::vector<RecoveredRecord> records;
    const auto report = recover_journal(path, fixture.config, &records);
    if (!report.ok()) {
      continue;
    }
    // Flipping any byte either breaks a digest, breaks the chain, or lands in a
    // field that is covered by a digest - it can never produce a "clean" report
    // with all records intact.
    if (report.value().status == RecoveryStatus::Clean) {
      SLF_EXPECT(false);
    }
    if (report.value().code == StatusCode::IntegrityError) {
      ++integrity_failures;
    }
    SLF_EXPECT(report.value().records_valid <= 2U);
  }
  SLF_EXPECT(integrity_failures > 0U);
  write_bytes(path, good);
}

SLF_TEST(store_truncated_and_incompatible_headers) {
  JournalFixture fixture;
  auto store = fixture.create();
  SLF_EXPECT(store.ok());
  SLF_EXPECT(store.value()->append(RecordType::Marker, std::span<const std::byte>(), fixture.incarnation, Epoch{1},
                                   TopologyGeneration{1})
                 .ok());
  const std::filesystem::path path = store.value()->path();
  const auto good = file_bytes(path);
  store.value().reset();

  // An empty file is reported as empty rather than as valid state.
  write_bytes(path, {});
  const auto empty = recover_journal(path, fixture.config, nullptr);
  SLF_EXPECT(empty.ok());
  SLF_EXPECT_EQ(empty.value().status, RecoveryStatus::Empty);

  // A short file cannot contain a header.
  write_bytes(path, std::vector<std::byte>(20, std::byte{0x11}));
  const auto short_file = recover_journal(path, fixture.config, nullptr);
  SLF_EXPECT(short_file.ok());
  SLF_EXPECT_EQ(short_file.value().status, RecoveryStatus::Corrupt);
  SLF_EXPECT_EQ(short_file.value().code, StatusCode::Truncated);

  // Wrong magic.
  std::vector<std::byte> wrong_magic = good;
  wrong_magic[0] = std::byte{'X'};
  write_bytes(path, wrong_magic);
  SLF_EXPECT_EQ(recover_journal(path, fixture.config, nullptr).value().status, RecoveryStatus::Corrupt);

  // Future format version.
  std::vector<std::byte> future = good;
  future[8] = std::byte{9};
  future[9] = std::byte{0};
  write_bytes(path, future);
  const auto unsupported = recover_journal(path, fixture.config, nullptr);
  SLF_EXPECT(unsupported.ok());
  SLF_EXPECT_EQ(unsupported.value().status, RecoveryStatus::IncompatibleVersion);
  SLF_EXPECT(!unsupported.value().usable());

  write_bytes(path, good);
  SLF_EXPECT_EQ(recover_journal(path, fixture.config, nullptr).value().status,
                RecoveryStatus::CrashWithoutTrailer);
}

SLF_TEST(store_replay_and_reordering_are_refused) {
  JournalFixture fixture;
  auto store = fixture.create();
  SLF_EXPECT(store.ok());
  for (int i = 0; i < 3; ++i) {
    SLF_EXPECT(store.value()->append(RecordType::Marker, kOneByte, fixture.incarnation, Epoch{1},
                                     TopologyGeneration{1})
                   .ok());
  }
  const std::filesystem::path path = store.value()->path();
  const auto good = file_bytes(path);
  store.value().reset();

  // Splice a duplicate of the first record after itself: the sequence chain no
  // longer continues, which is reported as a chain break rather than replaying
  // the same mutation twice.
  const std::size_t record_bytes = good.size() - JournalFormat::kHeaderBytes;
  const std::size_t one_record = record_bytes / 3U;
  std::vector<std::byte> replayed(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(JournalFormat::kHeaderBytes + one_record));
  replayed.insert(replayed.end(),
                  good.begin() + static_cast<std::ptrdiff_t>(JournalFormat::kHeaderBytes),
                  good.begin() + static_cast<std::ptrdiff_t>(JournalFormat::kHeaderBytes + one_record));
  replayed.insert(replayed.end(), good.begin() + static_cast<std::ptrdiff_t>(JournalFormat::kHeaderBytes + one_record),
                  good.end());
  write_bytes(path, replayed);
  const auto report = recover_journal(path, fixture.config, nullptr);
  SLF_EXPECT(report.ok());
  SLF_EXPECT_EQ(report.value().status, RecoveryStatus::ChainBroken);
  // The spliced record breaks the chain hash (integrity) before the sequence
  // check is even reached: replay is refused either way, never applied twice.
  SLF_EXPECT(report.value().code == StatusCode::Conflicting ||
             report.value().code == StatusCode::IntegrityError);
  SLF_EXPECT_EQ(report.value().records_valid, 1U);

  // Reordering the two later records breaks the chain as well.
  std::vector<std::byte> swapped;
  swapped.insert(swapped.end(), good.begin(), good.begin() + static_cast<std::ptrdiff_t>(JournalFormat::kHeaderBytes + one_record));
  swapped.insert(swapped.end(), good.begin() + static_cast<std::ptrdiff_t>(JournalFormat::kHeaderBytes + 2U * one_record), good.end());
  swapped.insert(swapped.end(), good.begin() + static_cast<std::ptrdiff_t>(JournalFormat::kHeaderBytes + one_record),
                 good.begin() + static_cast<std::ptrdiff_t>(JournalFormat::kHeaderBytes + 2U * one_record));
  write_bytes(path, swapped);
  const auto reordered = recover_journal(path, fixture.config, nullptr);
  SLF_EXPECT(reordered.ok());
  SLF_EXPECT_EQ(reordered.value().status, RecoveryStatus::ChainBroken);
  write_bytes(path, good);
}

SLF_TEST(store_record_bounds_and_recovery_bounds) {
  StoreConfig config;
  config.max_record_bytes = 16;
  config.max_recovery_bytes = 4096;
  TempDir dir{"bounds"};
  const ControllerIncarnation incarnation = mint_incarnation();
  auto store = FabricStore::create(dir.path(), config, incarnation, 1, SequenceNumber{1});
  SLF_EXPECT(store.ok());
  const std::vector<std::byte> too_big(17, std::byte{0});
  SLF_EXPECT_CODE(store.value()->append(RecordType::Marker, too_big, incarnation, Epoch{1},
                                        TopologyGeneration{1}),
                  StatusCode::Exhausted);

  // An over-sized declared payload in a corrupt file is refused before
  // allocation.
  const std::vector<std::byte> ok_payload(8, std::byte{1});
  SLF_EXPECT(store.value()->append(RecordType::Marker, ok_payload, incarnation, Epoch{1},
                                   TopologyGeneration{1})
                 .ok());
  const std::filesystem::path path = store.value()->path();
  (void)store.value()->close_clean();
  std::vector<std::byte> bytes = file_bytes(path);
  // payload_len lives at offset 24 of the first record.
  bytes[JournalFormat::kHeaderBytes + 24] = std::byte{0xFF};
  bytes[JournalFormat::kHeaderBytes + 25] = std::byte{0xFF};
  write_bytes(path, bytes);
  const auto report = recover_journal(path, config, nullptr);
  SLF_EXPECT(report.ok());
  SLF_EXPECT_EQ(report.value().status, RecoveryStatus::Corrupt);
  SLF_EXPECT_EQ(report.value().code, StatusCode::Exhausted);

  // A journal larger than the recovery bound is refused rather than partially
  // interpreted as complete state.
  StoreConfig tiny = config;
  tiny.max_recovery_bytes = 32;
  write_bytes(path, std::vector<std::byte>(64, std::byte{0}));
  const auto oversized = recover_journal(path, tiny, nullptr);
  SLF_EXPECT(oversized.ok());
  SLF_EXPECT_EQ(oversized.value().status, RecoveryStatus::Corrupt);
  SLF_EXPECT_EQ(oversized.value().code, StatusCode::Exhausted);
}

SLF_TEST(store_segment_rotation_and_pruning) {
  TempDir dir{"rotation"};
  StoreConfig config;
  config.max_records_per_segment = 2;
  const ControllerIncarnation incarnation = mint_incarnation();
  auto first = FabricStore::create(dir.path(), config, incarnation, 1, SequenceNumber{1});
  SLF_EXPECT(first.ok());
  SLF_EXPECT(first.value()->append(RecordType::Marker, std::span<const std::byte>(), incarnation, Epoch{1}, TopologyGeneration{1}).ok());
  SLF_EXPECT(first.value()->append(RecordType::Marker, std::span<const std::byte>(), incarnation, Epoch{1}, TopologyGeneration{1}).ok());
  SLF_EXPECT_CODE(first.value()->append(RecordType::Marker, std::span<const std::byte>(), incarnation, Epoch{1}, TopologyGeneration{1}),
                  StatusCode::Exhausted);
  SLF_EXPECT(first.value()->close_clean().ok());

  auto second = FabricStore::create(dir.path(), config, incarnation, 2, SequenceNumber{3});
  SLF_EXPECT(second.ok());
  SLF_EXPECT(second.value()->append(RecordType::Marker, std::span<const std::byte>(), incarnation, Epoch{2}, TopologyGeneration{1}).ok());
  SLF_EXPECT(second.value()->close_clean().ok());

  const auto segments = list_journal_segments(dir.path(), 4);
  SLF_EXPECT(segments.ok());
  SLF_EXPECT_EQ(segments.value().size(), 2U);
  SLF_EXPECT_EQ(segments.value()[0].segment_index, 1U);
  SLF_EXPECT_EQ(segments.value()[1].segment_index, 2U);
  SLF_EXPECT_EQ(segments.value()[0].path.filename().string(), std::string("journal-00000001.slf"));

  // More segments than the bound is an explicit refusal, not silent truncation.
  SLF_EXPECT_CODE(list_journal_segments(dir.path(), 1), StatusCode::Exhausted);
  SLF_EXPECT(!std::filesystem::exists(dir.path() / "journal-00000003.slf"));
}

SLF_TEST(store_snapshot_write_read_and_recovery) {
  TempDir dir{"snapshot"};
  const auto fabric = make_two_by_two();
  SLF_EXPECT(fabric.ok());
  StoreConfig config;
  SLF_EXPECT(SnapshotFile::write(default_snapshot_path(dir.path()), fabric.value(), SequenceNumber{4}, config)
                 .ok());
  SequenceNumber covered{};
  std::string detail;
  const auto read = SnapshotFile::read(default_snapshot_path(dir.path()), config, &covered, &detail);
  SLF_EXPECT(read.ok());
  SLF_EXPECT_EQ(covered, SequenceNumber{4});
  SLF_EXPECT_EQ(read.value().topology_digest, fabric.value().topology_digest);

  // A torn snapshot is rejected, never partially applied.
  const std::filesystem::path path = default_snapshot_path(dir.path());
  const auto good = file_bytes(path);
  for (std::size_t cut = 1; cut < 40; ++cut) {
    write_bytes(path, std::vector<std::byte>(good.begin(), good.end() - static_cast<std::ptrdiff_t>(cut)));
    SLF_EXPECT(!SnapshotFile::read(path, config, nullptr, nullptr).ok());
  }
  // A flipped payload byte breaks the payload digest.
  std::vector<std::byte> corrupted = good;
  corrupted[corrupted.size() - 1] = static_cast<std::byte>(static_cast<unsigned>(corrupted.back()) ^ 0x01U);
  write_bytes(path, corrupted);
  SLF_EXPECT_CODE(SnapshotFile::read(path, config, nullptr, nullptr), StatusCode::IntegrityError);
  write_bytes(path, good);

  // Whole-store recovery: snapshot accepted, newer records replayed.
  const ControllerIncarnation incarnation = mint_incarnation();
  auto store = FabricStore::create(dir.path(), config, incarnation, 1, SequenceNumber{5});
  SLF_EXPECT(store.ok());
  SLF_EXPECT(store.value()->append(RecordType::Marker, kSevenByte, incarnation, Epoch{1},
                                   TopologyGeneration{1})
                 .ok());
  SLF_EXPECT(store.value()->close_clean().ok());

  const auto recovery = recover_store(dir.path(), config, nullptr);
  SLF_EXPECT(recovery.ok());
  SLF_EXPECT(recovery.value().snapshot_accepted);
  SLF_EXPECT(recovery.value().snapshot_fabric.has_value());
  SLF_EXPECT_EQ(recovery.value().snapshot_watermark, SequenceNumber{4});
  SLF_EXPECT_EQ(recovery.value().snapshot_fabric->topology_digest, fabric.value().topology_digest);
  SLF_EXPECT_EQ(recovery.value().segments.size(), 1U);
  SLF_EXPECT_EQ(recovery.value().report.records_valid, 1U);
}

SLF_TEST(store_missing_and_empty_directories) {
  TempDir dir{"missing"};
  StoreConfig config;
  const auto missing = recover_store(dir.path(), config, nullptr);
  SLF_EXPECT(missing.ok());
  SLF_EXPECT_EQ(missing.value().report.status, RecoveryStatus::Missing);
  SLF_EXPECT(!missing.value().report.usable());

  const auto absent = recover_store(dir.path() / "does-not-exist", config, nullptr);
  SLF_EXPECT(absent.ok());
  SLF_EXPECT_EQ(absent.value().report.status, RecoveryStatus::Missing);
}
