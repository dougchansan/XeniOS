# Static recompilation validation record

Base reviewed: `87b176a078c316fde3adf67a217f0c44615b0e0d`.
Implementation branch: `codex/static-recomp`; draft PR #1.
This record distinguishes source construction, unit evidence and actual platform
acceptance. A compiled native binary is not evidence of playable Xbox 360 games.

## Verified local evidence

- Linux x86-64, GCC 14.2.0 and Clang 17; no local Apple SDK or physical iPhone.
- Four portable CTest suites pass after the iOS build-driver addition: runtime,
  exporter, build-driver contracts and selected PPC fixtures.
- Runtime tests cover SHA-256 empty/abc/million-a vectors; ABI and complete-range
  validation; duplicate and overlapping module rejection; instruction budgets
  and resume; unknown entries; changed code; unload; stop requests; native host
  callbacks; memory-boundary faults; and explicit unsupported reservations.
  Arithmetic helpers pass 100,000 deterministic randomized high-multiply/carry
  checks.
- **1,116 selected upstream PPC cases pass, from 103 fixture files.** The selection
  manifest records two unsupported/assembler exclusions and 25 absent candidate
  filenames. This is not complete PPC/VMX128 coverage or a game test.
- The exporter suite contains six tests. The iOS build-driver suite contains
  four contract tests and does not invoke an Apple SDK, sign or install an app.
- A separate Clang 17 AddressSanitizer/UndefinedBehaviorSanitizer configuration
  passes the runtime and exporter suites (2/2). The 1,116-case fixture suite was
  not run under these sanitizers.
- The real StaticBackend adapter and selected integration units pass syntax-only
  compilation against pinned real XeniOS/dependency headers. This is not a link
  or runtime result.

## Observed CI evidence

At commit `5b8755606841cfc9cbb87ec64412da39f4a2859c`, run
https://github.com/dougchansan/XeniOS/actions/runs/36835668285:

- Ubuntu 24.04 portable runtime/exporter/selected-PPC-fixture job: passed.
- ARM64 macOS 26 portable runtime/exporter job: passed. The PPC fixture selection
  was not run on this runner.
- Full strict XeniOS CMake configuration: passed after adding the required public
  runtime-data fetch. Full application compilation was still in progress when
  this record was written; no link/smoke success is inferred.

The earlier strict configuration failed because the bundled public game-patch
repository was missing; `./xenia-build.py fetchdata` fixes that setup step.
The inherited upstream lint job also reports existing formatting differences in
`a64_backend.cc` and `a64_seq_memory.cc`; those files were not changed by the
static backend implementation. The overall PR is not claimed fully green.

Commit `ec74b6208c1bac3589c6c329c0cff39e40868cbf` adds ARM64 feature-detection
isolation and an unsigned iOS build driver. The device-SDK job is
https://github.com/dougchansan/XeniOS/actions/runs/36837137037.
Adding or starting this job is not evidence that an iOS app built successfully.
Consult its actual conclusion and `static-ios-build-evidence` logs. Its empty
catalog deliberately cannot execute a game.

## Remaining acceptance gates

The actual-runtime headless smoke target uses the real Memory, Processor,
ThreadState and StaticBackend. Its registered host import and native callback
are self-authored test functions, not validation of the complete Xbox kernel.
It does not create a GPU window or run a commercial game.

Full-player link/runtime, physical iPhone execution, signing/installation,
commercial-game playability and comparative performance remain unverified in
this record. Extended VMX128, shared reservations, broader floating-point
behavior and guest exceptions/nonlocal flow still require implementation and
validation; these are engineering gaps, not merely missing device access.
See `static-recomp.md` and `static-recomp-ios.md` for commands and limitations.
