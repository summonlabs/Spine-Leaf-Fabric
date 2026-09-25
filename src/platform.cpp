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

#include "slf/platform.hpp"

#include <chrono>
#include <cstdio>
#include <limits>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <io.h>
#  include <process.h>
#else
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <cerrno>
#endif

namespace slf::platform {
namespace {

[[nodiscard]] Status last_os_error(std::string_view what) {
  return Status(StatusCode::IoError, std::string(what) + " failed at the operating system level");
}

#if defined(_WIN32)
/// Quotes one argument following the rules the Microsoft C runtime uses when it
/// parses a command line.
[[nodiscard]] std::string quote_argument(const std::string& argument) {
  if (!argument.empty() && argument.find_first_of(" \t\n\v\"") == std::string::npos) {
    return argument;
  }
  std::string out;
  out.push_back('"');
  std::size_t backslashes = 0;
  for (const char c : argument) {
    if (c == '\\') {
      ++backslashes;
      continue;
    }
    if (c == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out.push_back('"');
      backslashes = 0;
      continue;
    }
    out.append(backslashes, '\\');
    backslashes = 0;
    out.push_back(c);
  }
  out.append(backslashes * 2, '\\');
  out.push_back('"');
  return out;
}
#endif

}  // namespace

std::uint64_t now_unix_ms() noexcept {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

std::uint64_t monotonic_ms() noexcept {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::uint32_t current_pid() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint32_t>(::getpid());
#endif
}

void sleep_ms(std::uint64_t ms) noexcept {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

Status sync_file(void* file_handle) noexcept {
  if (file_handle == nullptr) {
    return Status(StatusCode::Invalid, "sync_file called with a null handle");
  }
  auto* file = static_cast<std::FILE*>(file_handle);
  if (std::fflush(file) != 0) {
    return last_os_error("fflush");
  }
#if defined(_WIN32)
  const int fd = ::_fileno(file);
  if (fd < 0) {
    return Status(StatusCode::IoError, "invalid file descriptor");
  }
  if (::_commit(fd) != 0) {
    return last_os_error("_commit");
  }
#else
  const int fd = ::fileno(file);
  if (fd < 0) {
    return Status(StatusCode::IoError, "invalid file descriptor");
  }
  if (::fdatasync(fd) != 0) {
    return last_os_error("fdatasync");
  }
#endif
  return Status::success();
}

Status remove_file(const std::filesystem::path& path) noexcept {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (ec) {
    return Status(StatusCode::IoError, "remove failed: " + ec.message());
  }
  return Status::success();
}

Status replace_file(const std::filesystem::path& source, const std::filesystem::path& destination) noexcept {
#if defined(_WIN32)
  if (::MoveFileExA(source.string().c_str(), destination.string().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return last_os_error("MoveFileEx");
  }
  return Status::success();
#else
  if (::rename(source.string().c_str(), destination.string().c_str()) != 0) {
    return last_os_error("rename");
  }
  return Status::success();
#endif
}

Outcome<std::uint64_t> directory_bytes(const std::filesystem::path& directory,
                                       std::uint32_t max_files) noexcept {
  std::error_code ec;
  if (!std::filesystem::is_directory(directory, ec)) {
    return Status(StatusCode::NotFound, "not a directory");
  }
  std::uint64_t total = 0;
  std::uint32_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    if (ec) {
      return Status(StatusCode::IoError, "directory iteration failed: " + ec.message());
    }
    if (count >= max_files) {
      return Status(StatusCode::Exhausted, "directory entry bound reached");
    }
    ++count;
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const auto size = entry.file_size(ec);
    if (ec) {
      continue;
    }
    std::uint64_t next = 0;
    if (size > (std::numeric_limits<std::uint64_t>::max)() - total) {
      return Status(StatusCode::Overflow, "directory size overflow");
    }
    next = total + size;
    total = next;
  }
  return total;
}

ChildProcess::~ChildProcess() { reset(); }

ChildProcess::ChildProcess(ChildProcess&& other) noexcept {
  valid_ = other.valid_;
  pid_ = other.pid_;
  handle_ = other.handle_;
  other.valid_ = false;
  other.pid_ = 0;
  other.handle_ = nullptr;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    reset();
    valid_ = other.valid_;
    pid_ = other.pid_;
    handle_ = other.handle_;
    other.valid_ = false;
    other.pid_ = 0;
    other.handle_ = nullptr;
  }
  return *this;
}

void ChildProcess::reset() noexcept {
  if (!valid_) {
    return;
  }
#if defined(_WIN32)
  auto handle = static_cast<HANDLE>(handle_);
  if (handle != nullptr) {
    ::CloseHandle(handle);
  }
  handle_ = nullptr;
#else
  pid_ = 0;
#endif
  valid_ = false;
}

Outcome<ChildProcess> ChildProcess::spawn(const ProcessOptions& options) {
#if defined(_WIN32)
  std::string command_line = quote_argument(options.executable.string());
  for (const auto& argument : options.arguments) {
    command_line.push_back(' ');
    command_line += quote_argument(argument);
  }
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  DWORD flags = CREATE_NO_WINDOW;
  // The string must outlive the CreateProcessA call: taking c_str() from a
  // temporary would hand the API a pointer to freed memory.
  const std::string working_directory_storage = options.working_directory.string();
  const char* working_directory = working_directory_storage.empty()
                                      ? nullptr
                                      : working_directory_storage.c_str();
  const BOOL created = ::CreateProcessA(nullptr, command_line.data(), nullptr, nullptr,
                                        options.inherit_output ? TRUE : FALSE, flags,
                                        nullptr, working_directory, &startup, &info);
  if (created == 0) {
    return Status(StatusCode::IoError, "CreateProcess failed with error " +
                                           std::to_string(::GetLastError()));
  }
  ::CloseHandle(info.hThread);
  ChildProcess process;
  process.valid_ = true;
  process.pid_ = static_cast<std::uint32_t>(info.dwProcessId);
  process.handle_ = info.hProcess;
  return process;
#else
  std::vector<std::string> storage;
  storage.push_back(options.executable.string());
  for (const auto& argument : options.arguments) {
    storage.push_back(argument);
  }
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (auto& item : storage) {
    argv.push_back(item.data());
  }
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    return last_os_error("fork");
  }
  if (pid == 0) {
    if (!options.working_directory.empty()) {
      if (::chdir(options.working_directory.string().c_str()) != 0) {
        ::_exit(127);
      }
    }
    if (options.inherit_output) {
      ::execv(argv[0], argv.data());
    } else {
      const int devnull = ::open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        ::dup2(devnull, STDOUT_FILENO);
        ::dup2(devnull, STDERR_FILENO);
      }
      ::execv(argv[0], argv.data());
    }
    ::_exit(127);
  }
  ChildProcess process;
  process.valid_ = true;
  process.pid_ = static_cast<std::uint32_t>(pid);
  process.handle_ = nullptr;
  return process;
#endif
}

bool ChildProcess::running() {
  if (!valid_) {
    return false;
  }
#if defined(_WIN32)
  auto handle = static_cast<HANDLE>(handle_);
  if (handle == nullptr) {
    return false;
  }
  return ::WaitForSingleObject(handle, 0) == WAIT_TIMEOUT;
#else
  int status = 0;
  const pid_t result = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  if (result == 0) {
    return true;
  }
  return false;
#endif
}

Status ChildProcess::kill() {
  if (!valid_) {
    return Status(StatusCode::Invalid, "kill on an invalid process handle");
  }
#if defined(_WIN32)
  auto handle = static_cast<HANDLE>(handle_);
  if (handle == nullptr) {
    return Status(StatusCode::Invalid, "no process handle");
  }
  if (::TerminateProcess(handle, 137) == 0) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_ACCESS_DENIED && ::WaitForSingleObject(handle, 0) == WAIT_OBJECT_0) {
      return Status::success();
    }
    return Status(StatusCode::IoError, "TerminateProcess failed with error " + std::to_string(error));
  }
  return Status::success();
#else
  if (::kill(static_cast<pid_t>(pid_), SIGKILL) != 0 && errno != ESRCH) {
    return last_os_error("kill");
  }
  return Status::success();
#endif
}

Outcome<int> ChildProcess::wait(std::uint64_t timeout_ms) {
  if (!valid_) {
    return Status(StatusCode::Invalid, "wait on an invalid process handle");
  }
#if defined(_WIN32)
  auto handle = static_cast<HANDLE>(handle_);
  if (handle == nullptr) {
    return Status(StatusCode::Invalid, "no process handle");
  }
  const DWORD timeout = timeout_ms > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<DWORD>(timeout_ms);
  const DWORD result = ::WaitForSingleObject(handle, timeout);
  if (result == WAIT_TIMEOUT) {
    return Status(StatusCode::DeadlineExpired, "child process did not exit before the deadline");
  }
  if (result != WAIT_OBJECT_0) {
    return Status(StatusCode::IoError, "WaitForSingleObject failed");
  }
  DWORD exit_code = 0;
  if (::GetExitCodeProcess(handle, &exit_code) == 0) {
    return Status(StatusCode::IoError, "GetExitCodeProcess failed");
  }
  return static_cast<int>(exit_code);
#else
  const std::uint64_t start = monotonic_ms();
  for (;;) {
    int status = 0;
    const pid_t result = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (result == static_cast<pid_t>(pid_)) {
      if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
      }
      if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
      }
      return Status(StatusCode::Indeterminate, "child terminated without an exit status");
    }
    if (result < 0 && errno != EINTR) {
      return last_os_error("waitpid");
    }
    if (timeout_ms != 0 && monotonic_ms() - start >= timeout_ms) {
      return Status(StatusCode::DeadlineExpired, "child process did not exit before the deadline");
    }
    sleep_ms(2);
  }
#endif
}

}  // namespace slf::platform
