# Static recompilation validation record

Base reviewed: `87b176a078c316fde3adf67a217f0c44615b0e0d`.
This record distinguishes code construction, unit evidence and actual platform
acceptance. A compiled native binary is not evidence of playable Xbox 360 games.

## Local sandbox evidence

- Linux x86-64, GCC 14.2.0 and Clang 17 available; no Apple SDK or physical iPhone.
- Portable runtime tests passed: SHA-256 empty/abc/million-a vectors; ABI and
  complete-range validation; rejection of duplicate and overlapping modules;
  instruction budgets/resume; unknown entries; changed code; module unload;
  stop requests; native host callbacks; memory-boundary faults; unsupported
  reservations. 100,000 deterministic randomized high-multiply/carry vectors.
- 1,116 selected upstream PPC fixture cases passed, from 103 fixture files.
  The selection manifest records two unsupported/assembler exclusions and 25
  absent candidate filenames. This is **not** complete PPC/VMX128 coverage.
- The actual StaticBackend adapter compiled in syntax-only mode against the
  pinned real XeniOS/dependency headers using Clang 17. Syntax checks do not
  establish linking, runtime behavior, GPU output or device acceptance.

## Required integration/platform acceptance

The repository includes an actual-runtime headless smoke target and CI jobs.
Consult the workflow conclusion and logs for the commit being used; a job added
to YAML is not evidence that it passed. The smoke program exercises the actual
Memory/Processor/ThreadState/backend, registered HLE and a native callback.

At initial submission, full-player link/runtime, physical iOS, commercial-game
playability and comparative performance are **not claimed** by this document.
Remaining ISA/FP/reservation limitations are listed in `static-recomp.md`.
