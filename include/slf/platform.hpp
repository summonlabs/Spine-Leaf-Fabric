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
#include <filesystem>
#include <string>
#include <vector>

#include "slf/status.hpp"

namespace slf::platform {

/// Wall-clock milliseconds since the Unix epoch. Used for leases and reports,
/// never for ordering decisions.
[[nodiscard]] std::uint64_t now_unix_ms() noexcept;

/// Monotonic milliseconds. Used for deadlines and durations.
[[nodiscard]] std::uint64_t monotonic_ms() noexcept;

/// Process identifier of the current process.
[[nodiscard]] std::uint32_t current_pid() noexcept;

/// Sleeps for p ms milliseconds. Interruptible sleeps are not modelled: the
/// runtime only sleeps inside workers that check their stop token afterwards.
void sleep_ms(std::uint64_t ms) noexcept;

/// Flushes a FILE* to stable storage (FlushFileBuffers / fdatasync).
[[nodiscard]] Status sync_file(void* file_handle) noexcept;

/// Removes a file, ignoring "not found".
[[nodiscard]] Status remove_file(const std::filesystem::path& path) noexcept;

/// Atomically replaces p destination with p source (MoveFileEx / rename).
[[nodiscard]] Status replace_file(const std::filesystem::path& source,
                                  const std::filesystem::path& destination) noexcept;

/// Directory size in bytes, bounded: stops counting after p max_files entries.
[[nodiscard]] Outcome<std::uint64_t> directory_bytes(const std::filesystem::path& directory,
                                                     std::uint32_t max_files) noexcept;

// ---- Child processes (used for the real multiprocess proofs) ---------------

struct ProcessOptions {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  std::filesystem::path working_directory;
  /// When false the child's stdout/stderr are discarded.
  bool inherit_output{false};
};

/// A child process handle. c kill() is a hard termination (TerminateProcess /
/// SIGKILL) with no chance for the child to flush or run destructors, which is
/// exactly what the crash-recovery proofs need.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  [[nodiscard]] static Outcome<ChildProcess> spawn(const ProcessOptions& options);

  [[nodiscard]] bool running();
  [[nodiscard]] std::uint32_t pid() const noexcept { return pid_; }
  /// Hard kill. Idempotent.
  [[nodiscard]] Status kill();
  /// Waits up to p timeout_ms for exit. Returns c DeadlineExpired when the
  /// child is still running: that is a failure of the caller's expectation, not
  /// a success.
  [[nodiscard]] Outcome<int> wait(std::uint64_t timeout_ms);
  [[nodiscard]] bool valid() const noexcept { return valid_; }

 private:
  void reset() noexcept;

  bool valid_{false};
  std::uint32_t pid_{0};
  void* handle_{nullptr};
};

}  // namespace slf::platform
