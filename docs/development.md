# Development

## Layout

```text
src/         Native plugin, wire format, placement, menu and recorder
companion/   Small Windows discovery/PTT executable, built with MinGW
python/      Launcher, supervisor and recoverable installer
tests/       Native/helper tests and independently authored packet examples
nix/         Reproducible package and isolated Wine integration checks
packaging/   systemd user-service template
docs/        User and implementation documentation
```

Tests are not loaded into X-Plane. They are built/run before package installation.
There are no committed binary packet captures, vendor assemblies, generated
executables, SDK copies or paid-app repair tools.

## Reproducible build and checks

```sh
nix build
nix flake check --print-build-logs
nix develop
```

The package compiles the native plugin and Windows companion, runs four native/
JSON checks and Python helper tests, then runs the PTT checks in a disposable
Wine prefix under Xvfb. Those input tests use **F24**, not the production
Down Arrow binding. They never use your live prefix or launch BeyondATC.

For helper-only iteration from the repository root:

```sh
PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=python \
  python3 -m unittest discover -s tests -p 'test_*.py' -v
```

Wine integration cases are explicitly skipped without `BATC_TEST_SHIM`.
Use the full Nix build for those checks; do not point that variable or Wine prefix
at your production application.

Native iteration in `nix develop`:

```sh
cmake -S . -B build -DXPMP2_SOURCE_DIR="$XPMP2_SOURCE_DIR"
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

`nix develop` provides the pinned XPMP2 source and SDK headers. Outside Nix,
CMake requires C++17, Python 3, pthreads and that XPMP2 source including its SDK
content. This is a developer build path, **not a complete non-Nix installer**:
the managed lifecycle also relies on Nix GC roots, the packaged Wine companion
and systemd. Do not advertise standalone `cmake --install` as turnkey setup.

Format Nix files with `nix fmt flake.nix nix/package.nix`. Follow existing
Python/C/C++ conventions; there is no additional mandatory formatter.

## Fixtures and regressions

`tests/generate_fixtures.py` writes small synthetic little-endian packet examples
in the build directory. It does not run or extract anything from the paid app.
The independent Python encoder helps detect C++ decoder layout mistakes, but
agreement between two local implementations is not proof of vendor semantics.

Placement/feedback tests also replay selected numeric values from observed
traffic-height failures. They distinguish AGL from terrain MSL, preserve valid
airborne altitude/flags, check zero-height behavior and exercise terrain failures.
Diagnostics tests independently parse emitted JSON and cover bounded rotation,
queue overload and recording failures. Installer tests use temporary fake
simulator/app files and cover hashes, refusal, locks, interruption and recovery.
No actual application binary or authenticated state is needed.

## Runtime changes

Never replace the plugin or Wine-facing view during a flight. Build and test
first; close both applications and stop the supervisor before an explicit
upgrade. Keep recovery generations. A successful build is not a substitute for
a controlled runtime observation.

For filters, unit conversions or thresholds, replay real captured values through
the new production helper before claiming a repair. Separate observations from
inference; treat an unexpected empty trace as a finding. Do not infer the cause
of one aircraft's failure from a different aircraft's terrain warning.

Keep protocol/watchdog/PTT changes small. Do not add blanket exception suppression,
airport exclusions, authentication bypasses or support claims for untested
versions.
