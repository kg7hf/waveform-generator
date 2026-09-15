# Build and dependencies

## Scope and 50,000-foot view

CMake builds the Windows tools or the original RT1170 EVK firmware from the
owned sources and six ordinary Git submodules. Installed compilers, Python,
CMake and programmers are external tools.

```text
owned sources + submodules -> CMake preset -> executable or firmware
SDMMC/FatFs copies -> local patches -> build-tree vendor inputs
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
```

Host presets use MinGW Makefiles and C++23. Arm presets use the installed GNU
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
It contains no hashes or revision locks. Update submodules with normal Git commands;
review `patches/` when updating SDMMC or FatFs. Configuration applies patches to
build-tree copies and leaves the upstream source tree alone.

The project version supplies `WFG_BUILD_ID` (for example `wfg-0.2.0`). Override
it with `-DWFG_BUILD_ID=my-build` when a distinguishing label helps. Labels allow
letters, digits, dots, underscores and hyphens. They identify a build by name;
they are not a guarantee that two binaries contain identical code. Device INFO,
artifact metadata and replay checks use this label. Replay also checks USB serial.

`cmake/project_options.cmake` applies warnings as errors, no exceptions and no
RTTI to owned C++ targets. Library `sources.cmake` files list compiled sources.
The target linker map remains useful for SRAM/OCRAM placement and memory sizing.

For a quick local path check:

```powershell
python scripts/check-project.py
python scripts/check-project.py --build-dir build/host-release
```

The second form checks compiled source paths in `compile_commands.json`. This
is a source-location check; it does not audit every compiler include or library.
For a module outside this checkout, also pass `--plugin-dir PATH` to explicitly
permit its compiled sources.

## Keeping test runs tidy

Python tests create temporary directories and remove their artifacts on completion,
including assertion failures. `scripts/test.ps1` runs CTest, prints failures, and
removes that preset's generated `Testing` logs. Use `-Preset host-debug` for Debug.
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
| Vendor adaptations | `scripts/dependencies.py`, `patches/` |
| Path checks and tests | `scripts/check-project.py`, `scripts/test.ps1`, `tests/` |
| Formatting | `.clang-format`, `scripts/format-source.ps1`, `scripts/cgm_options.txt` |

After changing an impairment, build and test the host tools and cross-build the
player. Board timing and analog behavior still need a physical bench run.
