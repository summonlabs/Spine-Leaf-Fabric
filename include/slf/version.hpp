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
#include <string_view>

namespace slf {

/// Library version. Bumped for every public release.
inline constexpr std::string_view kVersion = "1.0.0";

/// Binary/semantic interface version for on-disk and on-wire compatibility.
/// Persistence formats and transport frames carry this number and refuse to
/// interpret data written by an incompatible version.
inline constexpr std::uint32_t kInterfaceVersion = 1;

/// Persistence format version for journal and snapshot files.
inline constexpr std::uint32_t kPersistenceFormatVersion = 1;

/// Wire protocol version carried in every transport frame.
inline constexpr std::uint16_t kProtocolVersion = 1;

/// Returns a single-line build identification string (never used for logic).
std::string_view build_info() noexcept;

}  // namespace slf
