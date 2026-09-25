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

// Shared fixtures for the test suite: deterministic fabric construction and
// temporary directory helpers. Every fixture is plain code, not a fixture file,
// so a failing test shows exactly which records produced the failure.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "slf/builder.hpp"
#include "slf/identity.hpp"
#include "slf/model.hpp"
#include "slf/platform.hpp"

namespace slf::test {

/// Creates (and cleans) a unique temporary directory for a test.
class TempDir {
 public:
  explicit TempDir(const std::string& tag) {
    static std::uint64_t counter = 0;
    const std::uint64_t sequence = counter++;
    path_ = std::filesystem::temp_directory_path() /
            ("slf-test-" + tag + "-" + std::to_string(platform::current_pid()) + "-" +
             std::to_string(sequence) + "-" + std::to_string(platform::monotonic_ms()));
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    std::filesystem::create_directories(path_, ec);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

struct LeafSpec {
  std::uint32_t id{1};
  std::string name;
  LeafRole role{LeafRole::Tor};
  std::uint32_t domain{1};
  NodeState state{NodeState::Active};
  std::uint64_t access_mbps{400000};
  std::uint64_t role_incarnation{0};
};

struct SpineSpec {
  std::uint32_t id{1};
  std::string name;
  SpineRole role{SpineRole::FabricSpine};
  std::uint32_t domain{1};
  NodeState state{NodeState::Active};
  std::uint64_t fabric_mbps{3200000};
  std::uint64_t role_incarnation{0};
};

struct LinkSpec {
  std::uint32_t leaf{1};
  std::uint32_t spine{1};
  std::uint64_t speed_mbps{100000};
  LinkAdmin admin{LinkAdmin::Enabled};
  PortAdmin leaf_port_admin{PortAdmin::Enabled};
  PortAdmin spine_port_admin{PortAdmin::Enabled};
};

/// Standard two-tier fixture: the cartesian product of p leaves and
/// p spines with one link per pair, ports numbered deterministically.
[[nodiscard]] Outcome<Fabric> make_grid(TopologyGeneration generation,
                                        const std::vector<LeafSpec>& leaves,
                                        const std::vector<SpineSpec>& spines,
                                        const std::vector<LinkSpec>& links,
                                        FabricOptions options = {},
                                        std::uint64_t epoch = 1);

/// Two leaves, two spines in different domains, one link per pair.
[[nodiscard]] Outcome<Fabric> make_two_by_two(FabricOptions options = {});

/// Adds an evidence record for every link, observed by p observer.
[[nodiscard]] std::vector<LinkEvidence> observe_all_links(const Fabric& fabric, ControllerIncarnation observer,
                                                          Epoch epoch, LinkObservation observation,
                                                          std::uint64_t sequence_base = 1);

[[nodiscard]] LeafRecord leaf_record(const LeafSpec& spec, TopologyGeneration generation);
[[nodiscard]] SpineRecord spine_record(const SpineSpec& spec, TopologyGeneration generation);

}  // namespace slf::test
