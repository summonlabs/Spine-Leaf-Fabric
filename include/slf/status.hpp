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
#include <cstdlib>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace slf {

/// Typed outcome codes.
///
/// The vocabulary is deliberately fine grained: distinct failure conditions must
/// never collapse into a single "not ok". In particular c Unknown, c Unsupported,
/// c Stale, c Conflicting, c Incomplete, c Indeterminate, c Refused,
/// c Cancelled and c Invalid are separate codes with separate meanings, and a
/// missing piece of evidence is never reported as success.
enum class StatusCode : std::uint16_t {
  /// Operation succeeded and carries complete evidence.
  Ok = 0,

  /// Evidence is absent. The runtime does not know, and does not guess.
  Unknown,

  /// Input is malformed, out of range, or violates a modelled invariant.
  Invalid,

  /// A referenced entity does not exist in the queried generation.
  NotFound,

  /// Two records claim the same identity.
  DuplicateIdentity,

  /// An edge references an endpoint that does not exist.
  DanglingEdge,

  /// An adjacency exists that the active policy forbids (for example a
  /// same-tier spine-to-spine or leaf-to-leaf link when peer links are off).
  IllegalAdjacency,

  /// A record's role belongs to a different tier than the record's tier.
  ContradictoryRole,

  /// A reference points at a different topology generation than the record
  /// that carries it (for example a link bound to a future generation).
  CrossGeneration,

  /// The artifact is well formed but belongs to a superseded epoch,
  /// incarnation, generation, or attempt.
  Stale,

  /// Two pieces of retained evidence disagree and neither is authoritative.
  Conflicting,

  /// Required evidence for the query is not present yet.
  Incomplete,

  /// Evidence exists but is not sufficient to decide either way.
  Indeterminate,

  /// The operation is outside the modelled scope of this runtime.
  Unsupported,

  /// The operation was understood and deliberately refused (authority,
  /// policy, or lifecycle state).
  Refused,

  /// The supplied authority was superseded by a newer controller incarnation
  /// or a newer epoch.
  Fenced,

  /// The caller cancelled the operation.
  Cancelled,

  /// A bounded resource is exhausted (counts, capacity, connections, queue).
  Exhausted,

  /// Checked arithmetic detected overflow or underflow.
  Overflow,

  /// An integrity check (digest, checksum, chain hash) failed.
  IntegrityError,

  /// Bytes are present but cannot be interpreted as the expected structure.
  Corrupt,

  /// A record or frame ends before its declared length.
  Truncated,

  /// The artifact was produced by an incompatible format or protocol version.
  IncompatibleVersion,

  /// An operating-system level input/output operation failed.
  IoError,

  /// The resource is temporarily unavailable; retrying may succeed.
  Busy,

  /// The object has been closed or stopped and cannot serve the request.
  Closed,

  /// The identity already exists.
  AlreadyExists,

  /// The runtime has not been started.
  NotStarted,

  /// A protocol deadline expired. This is always a failure, never a success.
  DeadlineExpired,

  /// Internal invariant violation. Reaching this code is a defect.
  Internal,
};

/// Human readable, stable, lower-case name of a status code. The spelling is
/// part of the CLI and protocol surface and must not change casually.
[[nodiscard]] std::string_view to_string(StatusCode code) noexcept;

/// True only for c StatusCode::Ok.
[[nodiscard]] constexpr bool is_ok(StatusCode code) noexcept {
  return code == StatusCode::Ok;
}

/// True for codes that describe an operational refusal or an inability to
/// decide, as opposed to malformed input. Used by classification helpers.
[[nodiscard]] constexpr bool is_indeterminate(StatusCode code) noexcept {
  return code == StatusCode::Unknown || code == StatusCode::Indeterminate ||
         code == StatusCode::Incomplete || code == StatusCode::Unsupported;
}

/// True for codes that mean "this artifact belongs to an older era".
[[nodiscard]] constexpr bool is_stale_family(StatusCode code) noexcept {
  return code == StatusCode::Stale || code == StatusCode::Fenced;
}

/// A status value: a code plus an optional human readable detail string.
/// Copyable, cheap for the success case, and never silently convertible to a
/// success when it carries a failure code.
class Status {
 public:
  Status() noexcept = default;
  Status(StatusCode code, std::string detail) : code_(code), detail_(std::move(detail)) {}
  explicit Status(StatusCode code) : code_(code) {}

  /// Success factory. Named \c success rather than \c ok because the
  /// instance predicate \c ok() has no parameters, and C++ forbids overloading
  /// a static and a non-static member function with the same parameter list.
  [[nodiscard]] static Status success() noexcept { return Status{}; }

  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::Ok; }
  [[nodiscard]] std::string_view detail() const noexcept { return detail_; }

  /// "conflicting: two records disagree" - stable, lower case, single line.
  [[nodiscard]] std::string to_string() const;

 private:
  StatusCode code_{StatusCode::Ok};
  std::string detail_{};
};

[[nodiscard]] bool operator==(const Status& lhs, StatusCode rhs) noexcept;
[[nodiscard]] bool operator==(const Status& lhs, const Status& rhs) noexcept;

/// Outcome of an operation that produces a value on success.
///
/// The value is only reachable after checking c ok(), or through c value()
/// which returns a reference to a static sentinel for the failure case only
/// when c SLF_UNCHECKED_VALUE_ACCESS is defined; otherwise it aborts. There is
/// no implicit conversion to the value type.
template <class T>
class [[nodiscard]] Outcome {
 public:
  Outcome(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
  Outcome(Status status) : storage_(std::in_place_index<1>, std::move(status)) {}

  [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const Status& status() const noexcept {
    if (storage_.index() == 1) {
      return std::get<1>(storage_);
    }
    return ok_status_;
  }

  [[nodiscard]] StatusCode code() const noexcept { return status().code(); }

  T& value() & {
    if (storage_.index() == 0) {
      return std::get<0>(storage_);
    }
    on_bad_access();
    std::abort();
  }
  const T& value() const& {
    if (storage_.index() == 0) {
      return std::get<0>(storage_);
    }
    on_bad_access();
    std::abort();
  }
  T&& value() && {
    if (storage_.index() == 0) {
      return std::move(std::get<0>(storage_));
    }
    on_bad_access();
    std::abort();
  }

  [[nodiscard]] T* operator->() { return &value(); }
  [[nodiscard]] const T* operator->() const { return &value(); }
  [[nodiscard]] T& operator*() { return value(); }
  [[nodiscard]] const T& operator*() const { return value(); }

  /// Returns the value on success, otherwise p fallback. Never throws.
  [[nodiscard]] T value_or(T fallback) const {
    if (storage_.index() == 0) {
      return std::get<0>(storage_);
    }
    return fallback;
  }

 private:
  void on_bad_access() const noexcept;

  std::variant<T, Status> storage_;
  Status ok_status_{};
};

template <class T>
void Outcome<T>::on_bad_access() const noexcept {
  // Defensive: no exceptions are thrown by this library. A bad access is a
  // programming defect; aborting is deterministic and cannot be mistaken for a
  // successful result.
}

}  // namespace slf
