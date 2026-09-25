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

#include "slf/version.hpp"

namespace slf {

std::string_view build_info() noexcept {
#if defined(_MSC_VER)
  return "spine-leaf-fabric 1.0.0 (msvc)";
#elif defined(__clang__)
  return "spine-leaf-fabric 1.0.0 (clang)";
#elif defined(__GNUC__)
  return "spine-leaf-fabric 1.0.0 (gcc)";
#else
  return "spine-leaf-fabric 1.0.0";
#endif
}

}  // namespace slf
