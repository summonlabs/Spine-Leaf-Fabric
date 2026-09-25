# Contributing to Spine-Leaf Fabric

Spine-Leaf Fabric is developed by Summon Software Labs. Contributions are accepted
under the terms of the Apache License, Version 2.0.

## Contribution terms

By submitting a contribution (patch, pull request, or any other form) you agree that:

1. You are the author of the contribution, or you have the right to submit it.
2. You license your contribution under the **Apache License, Version 2.0**, the same
   license that covers this project, per section 5 ("Submission of Contributions") of
   that license. No separate license agreement is required.
3. **No Contributor License Agreement (CLA) is required.** There is no copyright
   assignment, and no sign-off bot is mandatory.
4. You retain copyright on your contribution.

Every source file in this repository carries the Apache-2.0 header. New files must
carry the same header.

## Ground rules for changes

* Keep the tiered-fabric semantics explicit. If a behavior is not modelled, it must be
  reported as `unsupported`, `unknown`, or `indeterminate` — never silently treated as
  success.
* Never fabricate hardware, protocol, or multi-host evidence. Distinguish REAL,
  SYNTHETIC, and UNSUPPORTED evidence in code, tests, and documentation.
* Structural validity and operational eligibility are separate concerns. Do not merge
  them into a single boolean.
* Missing, stale, torn, or replayed evidence must never be turned into a positive result.
* New behavior requires tests: unit tests plus, where the behavior is combinatorial, a
  seeded property or differential test against an independent reference model.
* Builds must stay warning-clean in Release and Debug (`/W4 /WX` on MSVC).

## Building and testing

```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

See `README.md` and `docs/` for architecture, semantics, and verification notes.

## Reporting defects

Open an issue with a minimal reproduction: the fabric specification, the command, the
observed outcome, and the expected outcome. Include the exact tool versions. Do not
include secrets or production topology data.
