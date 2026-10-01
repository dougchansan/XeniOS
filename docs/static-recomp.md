# Experimental strict static CPU backend

This fork adds a real offline PowerPC-to-C++ exporter and a prelinked CPU backend.
It does **not** establish commercial-game compatibility or a working signed iOS
release. See `static-recomp-validation.md` for the exact evidence and gaps.

## Execution model

The generated code is compiled with the host toolchain before installation. It
uses a pointer-based view of XeniOS's **existing** PPC context and routes memory,
MMIO and native imports through the existing runtime. It does not instantiate a
second emulated memory buffer or silently switch to a JIT/interpreter.

`--cpu=static` selects the backend in a normal desktop build. The stronger
`-DXENIA_STATIC_ONLY=ON` build excludes both host JIT backend targets, removes the
frontend translator pool, and rejects executable-memory requests through Xenia's
memory allocation/mapping/protection APIs. The decoder in the Python exporter
runs on the build host, not inside emitted instruction functions. Other emulator
facilities may retain instruction metadata for diagnostics; this is a CPU
execution guarantee, not a claim that every decoder in XeniOS has been deleted.

Every generated instruction checks its current guest word against the expected
word before executing. Modules also require exact SHA-256 verification of every
captured executable range when loaded. Changes to code, title updates or import
fixups are not treated as compatible because a filename or title ID matches.
All aligned instructions in a generated chunk have dispatch entries, including
indirect and mid-block targets. Native chunks return to a bounded dispatcher;
guest branches do not grow the host call stack. Native HLE callbacks may re-enter
guest execution through the normal Processor interface.

## Build and run the independent tests

Python 3.10+, CMake 3.20+, a C++20 compiler, and a Clang build with the PowerPC
assembler target are needed for the fixture suite.

```sh
python3 tools/static_recomp/tests/prepare_fixtures.py --out build-static-tests/fixtures
cmake -S tools/static_recomp -B build-static-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build-static-tests --parallel 4
ctest --test-dir build-static-tests --output-on-failure
```

`selection.json` in the fixture directory records selected files, excluded
fixtures, missing filenames, and case counts. It is not a whole-ISA coverage
percentage. Some historical upstream memory annotations contain commas/brackets
but were parsed as successive two-character hex pairs; the fixture adapter
intentionally matches `ppc_testing_main.cc` rather than changing its expected
values. Separate runtime tests use normal byte arrays.

For sanitizers, configure with `-DAOT_SANITIZE=ON` and Clang or GCC. To build the
real-runtime headless smoke test, first install the ordinary XeniOS dependencies
and pinned submodules from `docs/building.md`, then:

```sh
cmake -S . -B build-static -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DXENIA_STATIC_ONLY=ON -DXENIA_STATIC_SMOKE_TEST=ON -DXENIA_ENABLE_LTO=OFF
cmake --build build-static --target xenia-static-smoke xenia-app --parallel 4
ctest --test-dir build-static -R xenia-static-smoke --output-on-failure
```

The real smoke test uses a self-authored PPC program and the actual XeniOS
Memory, Processor, ThreadState and StaticBackend. It checks a guest store, a
loader-style native HLE import, a data-backed callback, invalidation after a code
write, and refusal of executable allocations. It does not create a GPU window or
claim to play a game.

## Capture -> export -> link -> build

1. Keep original game files, updates, saves, captured code and generated source
   **outside this public repository**. The capture/output directories receive a
   deny-all `.gitignore`, but that is not permission to publish their contents.
2. Launch the matching game with the ordinary XeniOS loader and the static CPU
   selected, setting `static_export_path` to a fresh private directory. For a
   desktop command-line invocation (binary path varies with generator/platform):

   ```sh
   XeniOS --cpu=static --static_export_path=/absolute/private/capture /absolute/private/game/default.xex
   ```

   The normal loader applies its selected title update and import fixups. The
   static backend writes `image.json` and code-section `.bin` files after those
   operations. Export-only mode suppresses guest entry points and stops the
   launch before gameplay. It is a capture operation, not a successful boot.
   A BIOS ZIP is not input. The current capture adapter supports XEX modules;
   it explicitly rejects other module types. It cannot discover a dynamic DLL
   loaded only after gameplay starts without a separate capture workflow.
3. Generate a module:

   ```sh
   python3 tools/static_recomp/export.py /absolute/private/capture/default.xex/image.json \
     --out /absolute/private/generated/default
   ```

   Unsupported words reject output by default. Code-section padding may also
   decode as unsupported. For a diagnostic build only, `--allow-unsupported`
   emits fatal guards at those addresses and records each one in `coverage.json`.
   This switch does **not** enable a fallback engine or make a game compatible.
4. Create a private source catalog:

   ```sh
   python3 tools/static_recomp/link.py /absolute/private/generated/default \
     --out /absolute/private/catalog
   ```

   The linker validates generated-source hashes and module ABI, rejects duplicate
   identities, and copies the validated source into a new catalog. It never
   loads downloaded native code in the installed app.
5. Reconfigure the player with both `XENIA_STATIC_ONLY=ON` and
   `XENIA_STATIC_MODULES_DIR=/absolute/private/catalog`. Build using the upstream
   platform instructions. iOS requires the Apple SDK, a device-compatible
   toolchain, appropriate signing and installation. The existing UIKit, Metal,
   audio and kernel implementations are retained. In the strict build the UI
   reports **Static CPU (no JIT)** and does not poll or hand off to StikDebug.
   No signed iOS binary is supplied by this change.
6. Import the exact corresponding game data. A missing module, changed code,
   unavailable native import or unsupported operation stops with the guest PC and
   reason. To add/change a precompiled title, rebuild and sign the app again.

All output destinations are create-only. Reusing a directory is rejected instead
of silently mixing modules from different builds.

## Current limitations and next engineering work

The integer/load-store/branch and selected standard VMX operations have a fixture
suite. This is not a complete Xenon ISA implementation. **VMX128 extended forms,
complete scalar/vector floating-point status/precision, and the shared
reservation monitor remain gaps.** Reservation instructions are rejected by the
exporter; it would be incorrect to add an isolated AOT-only monitor that ignored
HLE/GPU/other-thread writes. Several FP states fail explicitly rather than
silently weakening semantics. Unknown SPRs, exceptions and unsupported control
instructions also stop.

The initial memory adapter favors correctness checks over speed, including a
live instruction comparison on every instruction. No performance advantage over
the existing A64 JIT has been demonstrated. No commercial title has passed the
strict backend in this change. Real-game validation must include a known-good
upstream reference, exact module/update identity, interactive gameplay, audio,
scene changes and saves; physical iOS validation additionally needs a cold launch
without a debugger/JIT helper and lifecycle/memory/thermal checks.

The implementation is newly authored under the repository's BSD license. It does
not transplant GPL Suyu/RecompCore code or bundle XenonRecomp, game code, firmware,
keys, SDK binaries, or generated commercial-game sources.
