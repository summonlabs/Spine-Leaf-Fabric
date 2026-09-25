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

// Downstream consumer: builds a tiny fabric, validates it, indexes it, asks for
// an eligible spine set, and round-trips the canonical identity. It fails with a
// non-zero exit code if any of that does not hold, so "it linked" is not enough.

#include <cstdio>
#include <string>

#include "slf/builder.hpp"
#include "slf/eligibility.hpp"
#include "slf/spec.hpp"
#include "slf/topology.hpp"
#include "slf/version.hpp"

int main() {
  using namespace slf;
  const std::string spec =
      "fabric id=1 name=\"consumer\" generation=1 epoch=1 state=operational\n"
      "leaf id=1 name=\"leaf-1\" role=tor domain=1 state=active access_mbps=400000 "
      "role_incarnation=1 generation=1\n"
      "leaf id=2 name=\"leaf-2\" role=tor domain=2 state=active access_mbps=400000 "
      "role_incarnation=2 generation=1\n"
      "spine id=1 name=\"spine-1\" role=fabric domain=1 state=active fabric_mbps=3200000 "
      "role_incarnation=3 generation=1\n"
      "port id=1 owner=leaf:1 speed_mbps=100000 admin=enabled generation=1\n"
      "port id=2 owner=spine:1 speed_mbps=100000 admin=enabled generation=1\n"
      "port id=3 owner=leaf:2 speed_mbps=100000 admin=enabled generation=1\n"
      "port id=4 owner=spine:1 speed_mbps=100000 admin=enabled generation=1\n"
      "link id=1 a=leaf:1 pa=1 b=spine:1 pb=2 class=leaf_spine admin=enabled generation=1\n"
      "link id=2 a=leaf:2 pa=3 b=spine:1 pb=4 class=leaf_spine admin=enabled generation=1\n";
  const auto fabric = parse_spec(spec);
  if (!fabric.ok()) {
    std::printf("parse failed: %s\n", fabric.status().to_string().c_str());
    return 1;
  }
  const auto index = TopologyIndex::build(fabric.value());
  if (!index.ok()) {
    std::printf("index failed: %s\n", index.status().to_string().c_str());
    return 1;
  }
  EligibilityEngine engine(index.value());
  const auto constraints = PathConstraints::defaults_for(fabric.value().options);
  const auto assessment = engine.assess_leaf_pair(LeafId{1}, LeafId{2}, constraints);
  if (!assessment.ok() || assessment.value().verdict != EligibilityVerdict::Eligible) {
    std::printf("assessment was not eligible\n");
    return 1;
  }
  const auto reloaded =
      deserialize_fabric(std::span<const std::byte>(serialize_fabric(fabric.value())), BuilderLimits{});
  if (!reloaded.ok() || reloaded.value().topology_digest != fabric.value().topology_digest) {
    std::printf("canonical identity did not survive a round trip\n");
    return 1;
  }
  std::printf("consumer ok: %s version=%.*s spines=%zu digest=%.16s\n", "spine-leaf-fabric",
              static_cast<int>(kVersion.size()), kVersion.data(),
              assessment.value().eligible_spines.size(),
              fabric.value().topology_digest.hex().c_str());
  return 0;
}
