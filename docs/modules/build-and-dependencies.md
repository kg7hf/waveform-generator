# Build and dependencies

## Scope and 50,000-foot view

CMake builds the host tools or RT1170/RT1064 EVK firmware from the
owned sources and the ordinary Git submodules that `dependencies.json` lists:
six firmware inputs, and Catch2, which only the host unit tests compile.
(`third_party/pathsim` belongs to the separate channel-sim build; see
[pathsim](pathsim.md).) Installed compilers, Python, CMake and programmers are
external tools.

```text
owned sources + submodules -> CMake preset -> executable or firmware
SDMMC/FatFs copies -> local patches -> build-tree vendor inputs
Catch2 (host builds with tests) -> Catch2 unit-test executables -> ctest
```

## How to build and test

```powershell
.\scripts\init-dependencies.ps1
python -m pip install -r host/requirements-test.txt
cmake --preset host-release
cmake --build --preset host-release --parallel
.\scripts\test.ps1
cmake --preset rt1170-player-release
cmake --build --preset rt1170-player-release --parallel
cmake --preset rt1064-player-release
cmake --build --preset rt1064-player-release --parallel
```

The `host-tests` preset uses Ninja; the other host presets use MinGW Makefiles.
All host presets use C++23. Arm presets use the installed GNU
Arm toolchain path in `CMakePresets.json`; a local user preset can override it.
The host and player presets appear first. Entries marked **Diagnostic** build
the stopped scaffold, resident tone or SD-only tools. Read-only player presets
provide write-protected USB storage for inspection.

## Optional encoder builds

The default is `WFG_ENABLE_ENCODER_PLUGINS=OFF`. To include a separately supplied
module, set that option to `ON` and `WFG_ENCODER_PLUGIN_DIR` to its directory.
Use a separate build tree for each variant. The [plugin guide](../encoder-plugins.md)
includes complete commands and a public example. The default player excludes
encoder sources and their workspace while retaining WAV playback and live effects.

## Maintainer view

`.gitmodules` owns dependency URLs and Git records the submodule revisions.
`dependencies.json` lists required local files and the inputs copied for patches.
It contains no file hashes. Update submodules with normal Git commands;
review `patches/` when updating SDMMC or FatFs. Configuration applies patches to
build-tree copies and leaves the upstream source tree alone.

Two optional fields serve Catch2. `"scope": "host-test"` marks a dependency
that only host test builds compile; an entry without it is a firmware input.
The firmware configure (`dependencies.py --prepare`, from `rt1170/CMakeLists.txt`)
checks the firmware inputs alone, so a checkout without Catch2 still configures
and builds firmware, while `init-dependencies.ps1`, a plain
`python scripts/dependencies.py` and `check-project.py` (unless given `--scope`)
check every entry.
There is no second commit or release pin in `dependencies.json`. Git records
the submodule revision. To update Catch2, check out the desired upstream
revision in `third_party/Catch2` and stage the submodule change. The host
configure reads the version declared by the checked-out sources and reports it.

The project version supplies `WFG_BUILD_ID` (for example `wfg-0.2.0`). Override
it with `-DWFG_BUILD_ID=my-build` when a distinguishing label helps. Labels allow
up to 48 letters, digits, dots, underscores and hyphens. They identify a build by
name; they are not a guarantee that two binaries contain identical code. Device
INFO, artifact metadata and replay checks use this label. Replay also checks USB
serial.

Every build also records the Git commit of the checkout and whether tracked files
differ from it. `cmake/wfg_build_info.cmake` writes `generated/wfg_build_info.hpp`
in the build tree and rewrites it only when the commit or dirty state changes.
The header holds typed constants in
`waveform_generator::build_info` (`fw_commit`, `fw_commit_hi`, `fw_commit_lo`,
`fw_dirty`), not macros.
`rt1170/build_identity.hpp` defines their types, including the `FwDirty`
enumeration, and `usb_service.cpp` checks the header at compile time: the commit
is 40 lowercase hex digits or `unknown`, and an unknown commit has an unknown
dirty state. INFO reports `fw_commit` and `fw_dirty` (0 `clean`, 1 `modified`,
2 `unknown`, 3 `clean_with_plugins`: clean plus encoder plugin sources); STATUS
reports `fw_commit_hi`/`fw_commit_lo` (the first 16 hex digits) and `fw_dirty`,
and JSON `STATUS?` also carries `build_id` and `fw_commit`. The numbers are wire
values and never change meaning. Without Git, or outside a Git checkout, the
commit is `unknown`.
Only tracked files of this checkout count. Encoder plugin sources normally live
in the ignored `private/` tree or another repository, so the commit does not
identify them: a build configured with `WFG_ENABLE_ENCODER_PLUGINS=ON` never
reports 0, and reports 3 when this checkout is clean (conservatively also for a
read-only player, which leaves plugin sources out). A clean tracked checkout does
not identify the compiler, build options, submodule working trees or final binary.
Record the build label, ELF and firmware image for comparisons that require an
exact executable identity.

`cmake/project_options.cmake` applies warnings as errors, no exceptions and no
RTTI to owned C++ targets. Library `sources.cmake` files list compiled sources.
The target linker map remains useful for SRAM/OCRAM placement and memory sizing.

## Catch2 unit tests (host only)

`cmake/wfg_catch2.cmake`, included by the root `CMakeLists.txt` for the host
target only, builds Catch2 v3 from `third_party/Catch2` when `BUILD_TESTING` is on
(CTest's option, on by default). No firmware preset reads that file, so no image
compiles or links Catch2. Catch2 is compiled like the owned tests: no exceptions,
no RTTI, and `CATCH_CONFIG_DISABLE_EXCEPTIONS` in its generated
`catch_user_config.hpp`, so Catch2 and every test unit agree. A failed `REQUIRE`
therefore reports and then ends its test case's process; `CHECK` records the
failure and continues. Catch2's own warnings stay out of the owned
warnings-as-errors: its targets are `SYSTEM` and `EXCLUDE_FROM_ALL`, and
`project_options.cmake` leaves `third_party/` alone.

A component declares a Catch2 executable with one call; the project C++ policy
reaches it like any other owned target:

```cmake
wfg_add_catch2_test(wfg_signal_lab_unit_tests
    SOURCES tests/det_math_tests.cpp
    LIBRARIES wfg_signal_lab
    COMPILE_OPTIONS ${wfg_signal_lab_unit_test_options})
```

Each `TEST_CASE` becomes its own ctest test, named `<target>: <test case name>`
and labelled `catch2`. Discovery is `PRE_TEST`: ctest asks the executable for its
cases when it runs, so a build never runs a test binary, and the compiler's
runtime directory goes first on `PATH` for discovery and for each case (Git
Bash would otherwise load an older `libstdc++-6.dll`). From the repository root:

```powershell
cmake --preset host-release
cmake --build --preset host-release --parallel
ctest --test-dir build/host-release -L catch2 --output-on-failure
```

The project-harness tests in `tests/` and the legacy signal-lab executables keep
their own `add_test` registration. Configure stops with a message when
`third_party/Catch2` is not initialised (run `scripts/init-dependencies.ps1`);
`-DBUILD_TESTING=OFF` builds the host tools without any test.

For a quick local path check:

```powershell
python scripts/check-project.py
python scripts/check-project.py --build-dir build/host-release
```

The second form checks compiled source paths in `compile_commands.json`. This
is a source-location check; it does not audit every compiler include or library.
For a module outside this checkout, also pass `--plugin-dir PATH` to explicitly
permit its compiled sources. `--scope host-test` (or `firmware`) checks only the
`dependencies.json` entries of that scope, for a checkout that initialised only
the submodules a host build reads; `WFG_DEPENDENCY_SCOPE` in the environment
sets the same default, which scopes the `wfg-project-layout` ctest too.

## Host CI

The workflow in `.github/workflows/host-ci.yml` builds and tests host code on
GitHub-hosted Linux runners. The standalone PathSim channel-simulator build has
its own hosted job and four Linux tests. The shared `host-tests` CMake workflow
preset configures, builds and runs the basic host CTest suite with Ninja.
See [GitHub-hosted CI](../development/hosted-ci.md) for workflow scope and
worker limits, [coding standard](../coding-standard.md) for the authoritative
style, complexity and static-analysis rules, and [testing](../testing.md)
for commands. Firmware cross-builds and physical acceptance are separate
checks. Do not infer board timing from a passing host job.

## Keeping test runs tidy

Python tests create temporary directories and remove their artifacts on completion,
including assertion failures. `scripts/test.ps1` runs CTest, prints failures, and
removes that preset's generated `Testing` logs only after a passing run; failure
logs remain in the build tree. Use `-Preset host-debug` for Debug,
and `-Preset pathsim-host` for channel-sim's own build (`build/main/pathsim-host`,
[pathsim](pathsim.md)), which the root project and the Linux host CI do not build.
Direct `ctest --preset host-release` remains available when you want CTest logs.
Run one test session per build directory at a time. Build products and deliberate
experiment outputs stay under ignored directories. They are not source files.

## A simple way to picture it

A preset is the recipe for a program. Submodules provide separately maintained
ingredients; patches adjust temporary copies. Rebuilding uses those ingredients
without keeping a second inventory of every byte.

## Source map

| Responsibility | Files |
|---|---|
| Targets and presets | `CMakeLists.txt`, `CMakePresets.json`, library `sources.cmake` |
| C++ policy | `cmake/project_options.cmake`, [Coding standard](../coding-standard.md) |
| Submodule setup | `.gitmodules`, `dependencies.json`, `scripts/init-dependencies.ps1` |
| Catch2 host unit tests | `cmake/wfg_catch2.cmake` (`wfg_add_catch2_test`), `third_party/Catch2` |
| Vendor adaptations | `scripts/dependencies.py`, `patches/` |
| Path checks and tests | `scripts/check-project.py`, `scripts/test.ps1`, `tests/` |
| Host CI and its checks | `.github/workflows/host-ci.yml`, `scripts/check-style.py`, `scripts/check-complexity.py`, `scripts/complexity-baseline.json` |
| Formatting | `.clang-format`, `scripts/format-source.ps1` (clang-format 19) |
| Static analysis | `.clang-tidy`, `scripts/clang-tidy.ps1`, [active gates](../coding-standard.md#active-static-analysis-gates) |

After changing an impairment, build and test the host tools and cross-build the
player. Board timing and analog behavior still need a physical bench run.
