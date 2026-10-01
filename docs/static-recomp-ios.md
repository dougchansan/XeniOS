# Strict iOS build and acceptance

This is an experimental build workflow, not a compatibility or release claim.
The static emitter is incomplete; see static-recomp.md. No game content or
Apple signing credentials belong in this repository or its CI artifacts.

## Build on a Mac

Install the upstream build prerequisites, the iPhoneOS SDK and Metal toolchain;
run `./xb setup` to fetch pinned dependencies and the public runtime databases.
After creating a private source catalog with export.py and link.py:

```sh
python3 tools/static_recomp/build_ios.py \
  --catalog /absolute/private/catalog \
  --build-dir /absolute/private/xenios-ios-build --jobs 3
```

The build and catalog directories must be outside the public source checkout.
Build output is create-only. The driver compiles the upstream host shader tool,
configures the device-only Xcode build with XENIA_STATIC_ONLY=ON, builds without
installation signing, and checks the resulting binary for the static backend
and unexpected x64/a64 CPU backend symbols. Symbol checks are not an exhaustive
security audit or evidence of device execution.

For an explicit compile/link probe with no title modules:

```sh
python3 tools/static_recomp/build_ios.py \
  --empty-catalog --build-dir /absolute/private/xenios-ios-probe
```

An empty catalog cannot run a game. The unsigned app is expected at
`bin/iOS/Release/XeniOS.app` inside the build directory. Each step writes a log.
`static-build-result.json` separately records build completion, signing,
device testing and gameplay verification; the last three remain false here.
Provision, sign and install using the developer's own local Apple workflow.
This driver does not access certificates, install on devices or upload output.

## Physical-device acceptance still required

Use an exact game/update/module identity with a known-good upstream baseline.
Confirm a cold launch without a debugger or JIT helper, actual controllable
rendered gameplay and audio, scene transitions, save/load, pause/title stop and
relaunch. Measure memory, thermal behavior and frame times rather than inferring
performance from portable tests. Unsupported instructions and code identity
changes must stop with a diagnostic, never silently enter another CPU engine.

The four build-driver unit tests only check command construction, output-path
boundaries and early refusal on non-macOS hosts. They do not run an Apple SDK.
