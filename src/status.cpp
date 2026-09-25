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

#include "slf/status.hpp"

namespace slf {

std::string_view to_string(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::Ok: return "ok";
    case StatusCode::Unknown: return "unknown";
    case StatusCode::Invalid: return "invalid";
    case StatusCode::NotFound: return "not_found";
    case StatusCode::DuplicateIdentity: return "duplicate_identity";
    case StatusCode::DanglingEdge: return "dangling_edge";
    case StatusCode::IllegalAdjacency: return "illegal_adjacency";
    case StatusCode::ContradictoryRole: return "contradictory_role";
    case StatusCode::CrossGeneration: return "cross_generation";
    case StatusCode::Stale: return "stale";
    case StatusCode::Conflicting: return "conflicting";
    case StatusCode::Incomplete: return "incomplete";
    case StatusCode::Indeterminate: return "indeterminate";
    case StatusCode::Unsupported: return "unsupported";
    case StatusCode::Refused: return "refused";
    case StatusCode::Fenced: return "fenced";
    case StatusCode::Cancelled: return "cancelled";
    case StatusCode::Exhausted: return "exhausted";
    case StatusCode::Overflow: return "overflow";
    case StatusCode::IntegrityError: return "integrity_error";
    case StatusCode::Corrupt: return "corrupt";
    case StatusCode::Truncated: return "truncated";
    case StatusCode::IncompatibleVersion: return "incompatible_version";
    case StatusCode::IoError: return "io_error";
    case StatusCode::Busy: return "busy";
    case StatusCode::Closed: return "closed";
    case StatusCode::AlreadyExists: return "already_exists";
    case StatusCode::NotStarted: return "not_started";
    case StatusCode::DeadlineExpired: return "deadline_expired";
    case StatusCode::Internal: return "internal";
  }
  return "internal";
}

std::string Status::to_string() const {
  std::string out(slf::to_string(code_));
  if (!detail_.empty()) {
    out += ": ";
    out += detail_;
  }
  return out;
}

bool operator==(const Status& lhs, StatusCode rhs) noexcept { return lhs.code() == rhs; }
bool operator==(const Status& lhs, const Status& rhs) noexcept {
  return lhs.code() == rhs.code() && lhs.detail() == rhs.detail();
}

}  // namespace slf
