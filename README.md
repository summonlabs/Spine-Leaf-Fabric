# Spine-Leaf Fabric

An explicit tiered-fabric representation and governance runtime for leaf/spine
topologies, written in C++20 with no third-party dependencies.

Spine-Leaf Fabric models **what tiered fabric exists now**, **which tiered paths
remain eligible**, and **what authority survives topology or tier failure**. It
owns tiered semantics — membership, tier roles, adjacency, capacity, failure
domains, path eligibility, generation binding, and authority — and deliberately
does **not** own generic topology discovery, arbitrary route computation, ECMP
policy, QoS, switch/ASIC programming, or cluster/site governance.

## What is implemented

* **Tiered model.** `Fabric` for exactly one topology generation: leaf and spine
  records with roles, role incarnations, failure domains, and administrative
  states; ports and links with administrative state and effective speed;
  retired links retained as history; observed link evidence with observer,
  epoch, and sequence.
* **Structural validation.** A builder refuses duplicate identities, dangling
  edges, self loops, illegal same-tier adjacency, contradictory tier/role
  combinations, contradictory capacity declarations, cross-generation
  references, out-of-bound counts, and invalid UTF-8 before anything is frozen.
  Structural validity is separate from operational eligibility.
* **Canonical identity.** Records are sorted and encoded canonically; the
  topology digest and evidence digest are SHA-256 over that encoding, so
  insertion order never changes identity and any material field change moves it.
* **Adjacency index.** A compressed adjacency array per link class, rebuilt per
  generation. The index owns the fabric it describes.
* **Reachability classes.** leaf↔spine direct, leaf↔leaf through one or many
  spines, same-tier peer, peer-assisted, spine↔spine through a leaf, and
  unreachable — computed structurally, then refined operationally.
* **Eligibility.** Per-spine verdicts, leaf-pair assessments, eligible spine
  sets with bounded diversity enumeration, exact rational oversubscription,
  checked capacity aggregation, failure-domain diversity and domain-loss
  tolerance, and a fabric health classification. Every answer carries the
  generation, epoch, digest, and controller incarnation it was computed at.
* **Evidence policy.** `config_only` decides from durable configuration;
  `require_fresh` requires evidence observed by the *current* incarnation at the
  current generation and epoch. Missing, historical, conflicting, or
  "observer says unknown" evidence yields `indeterminate`, never `eligible`.
* **Governance.** Leases, authority tokens, and path authorities bound to
  (controller incarnation, epoch, generation, topology digest, lease). Restarts
  mint a fresh incarnation and bump the epoch, so previously issued authority is
  reported as `fenced` or `stale` rather than honoured.
* **Exactly-once commands.** Each governed mutation carries a request id; the
  committed result is journaled, and a retry after a crash replays the recorded
  result (`deduplicated`) instead of applying the mutation twice.
* **Versioned persistence.** A chained journal (per-record SHA-256 and chain
  hash), atomic snapshots, bounded segments/records/bytes, conservative
  recovery, and explicit recovery statuses (`clean`, `crash_without_trailer`,
  `truncated_tail`, `chain_broken`, `corrupt`, `incompatible_version`).
  Recovered dynamic evidence is historical and marked as requiring
  re-observation.
* **Framed transport.** Length-prefixed, digest-verified frames over loopback
  TCP with bounded payloads, bounded connections, per-connection deadlines, and
  typed errors for hostile framing.
* **CLI and library.** `slfctl` for offline inspection and for running a
  controller; a small C++ API for embedding.

## Build

```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

MSVC builds use `/W4 /WX`; GCC/Clang builds use `-Wall -Wextra -Wpedantic
-Wconversion -Wsign-conversion -Werror`. `-DSLF_ENABLE_ASAN=ON` adds
AddressSanitizer.

## Use

```
slfctl validate --spec fabric.slf        # structure only, with issue codes
slfctl inspect  --spec fabric.slf        # records, digests, adjacency
slfctl paths    --spec fabric.slf --from leaf:1 --to leaf:2 --diversity
slfctl spines   --spec fabric.slf --leaf leaf:1 --evidence
slfctl health   --spec fabric.slf --evidence
slfctl serve    --spec fabric.slf --state ./state --port 9000
slfctl client   status --port 9000
```

Exit codes: 0 ok, 2 usage, 3 invalid, 4 refused/fenced, 5 indeterminate, 6 I/O,
7 cancelled, 8 integrity, 9 not found/conflict.

As a library:

```cpp
#include "slf/builder.hpp"
#include "slf/eligibility.hpp"
#include "slf/topology.hpp"

auto index = slf::TopologyIndex::build(fabric);            // shares ownership
slf::EligibilityEngine engine(*index);
auto assessment = engine.assess_leaf_pair(slf::LeafId{1}, slf::LeafId{2}, constraints);
```

An installed package exports `SummonLabs::slf`:

```cmake
find_package(spine_leaf_fabric CONFIG REQUIRED)
target_link_libraries(app PRIVATE SummonLabs::slf)
```

## Verification

`tests/` holds the suite that is run by CTest: unit tests, seeded property
tests, a differential test against an independent brute-force oracle
(`tests/oracle.cpp`), adversarial framing/specification/recovery tests,
concurrency tests, and a multiprocess test that starts `slfctl` as a child
process, talks to it over loopback TCP, hard-kills it with `TerminateProcess` at
several lifecycle boundaries, restarts it, and proves incarnation/epoch fencing
and exactly-once commit replay. `benchmarks/slfbench.cpp` reports completed-work
timings; `examples/` holds two runnable programs.

## Boundaries and limitations

* Modelled structure only. No switch, ASIC, NIC, or protocol state is read or
  programmed; there is no ECMP, QoS, BGP, LLDP, or discovery implementation.
* Evidence is supplied by an operator or an external observer through the API
  and the protocol. This runtime does not probe links itself, and no physical
  link behaviour is claimed.
* Loopback TCP is the only transport. No multi-host, RDMA, InfiniBand, or NVLink
  behaviour is claimed or tested.
* Single-writer governance. One active lease authorises mutations; distributed
  consensus between controllers is not implemented.
* Durability is proven at the process-kill boundary (flush plus `_commit` /
  `fdatasync`). Power-loss durability and torn-sector behaviour are not claimed.
* The fabric is a single tier pair (leaf, spine). Three-tier clos designs are
  out of scope.
* The tooling targets the offline/loopback case; there is no packaging, service
  management, or configuration-management integration.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
