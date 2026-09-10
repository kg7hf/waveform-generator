# Build Configurations, Contracts, and Provenance

## 1. Scope

This guide covers CMake target selection, checked-in presets, offline dependency
materialization, source manifests, source-import and dependency ledgers, schemas,
and standalone-boundary audits.

## 2. 50,000-foot view

The project is a standalone nested repository. A build may use installed compilers
and tools, but all project source and materialized library inputs must resolve
inside this repository or through a declared toolchain exception.

Configure creates a source manifest from actual first-party bytes. Firmware and
host artifacts can therefore report source identity without depending on a Git
checkout or commit name.

## 3. How to use it

Host Release workflow:

```powershell
cmake --preset host-release
cmake --build --preset host-release --parallel
ctest --preset host-release --output-on-failure
```

Writable RT1170 player workflow:

```powershell
cmake --preset rt1170-player-release
cmake --build --preset rt1170-player-release --parallel
```

Read-only RT1170 workflow:

```powershell
cmake --preset rt1170-player-readonly-release
cmake --build --preset rt1170-player-readonly-release --parallel
```

Boundary audit after a configured target build:

```powershell
python scripts/verify-standalone.py --root . `
  --build-dir build/rt1170-player-release `
  --toolchain-root $env:USERPROFILE\.mcuxpressotools\arm-gnu-toolchain-14.2.rel1-mingw-w64-x86_64-arm-none-eabi `
  --check-provenance --require-build-evidence
```

Do not edit `third_party/` casually. Update dependencies through the explicit
materialization workflow, pinned commit, license notices, and per-file lock.

## 4. Scientist and maintainer view

### Target selection

`WFG_TARGET=host` builds portable libraries, executables, and tests. `WFG_TARGET=rt1170`
builds one selected firmware image. `WFG_FIRMWARE_IMAGE` is `scaffold`, `tone`,
`player`, or `sdcard`. `WFG_MSC_READ_ONLY` changes both USB write behavior and
local artifact-writer inclusion. `WFG_SD_POWER_CONTROL_TEST` selects the SD-only
diagnostic behavior.

Debug and Release are separate evidence points. Embedded Release uses the target
float and optimization contract. Host equivalence does not replace Arm build or
target execution.

### Identity construction

Configure recursively hashes generator-owned files while excluding build output,
Git metadata, local environments, artifacts, and materialized third-party trees.
Third-party bytes are represented by `dependencies.lock.json`; imported first-party
adaptations are represented by `docs/source-imports.json`.

The build ID combines project version, first-party source-manifest SHA-256,
dependency-lock SHA-256, and source-import-ledger SHA-256. Adding documentation
changes first-party source identity because documentation is part of the standalone
project record.

### Provenance maintenance

When a materialized imported file changes, update its local hash and byte size in
the source-import ledger and describe the adaptation. Do not change the donor hash
unless the donor bytes changed. A provenance mismatch is a useful failure, not a
test to disable.

### Failure boundaries

Separate product failures from missing toolchains, inaccessible temporary folders,
network attempts, unmaterialized dependencies, stale CMake caches, and source-ledger
drift. Retain configure, build, test, map, manifest, and audit outputs when making
release or hardware claims.

## 5. 5th-grader view

A build is a recipe. The preset chooses which cake to bake. The lock files list
the exact flour and sugar. The source manifest fingerprints the instructions. If
someone changes one sentence or ingredient, the fingerprint changes, so another
person can tell they did not bake from exactly the same recipe.

The boundary checker makes sure the cook did not secretly borrow an ingredient
from an unlabeled cupboard outside the project.

## 6. Toy worked example

Imagine a project with only three files:

```text
main.cpp  -> hash AAA
engine.cpp -> hash BBB
README.md -> hash CCC
```

The sorted manifest records all three names, sizes, and hashes, then receives its
own hash `MMM`. Firmware identity might be summarized as:

```text
wfg-0.2.0,source=MMM,deps=DDD,imports=III
```

Changing only `README.md` produces a new source manifest hash. Changing an imported
`engine.cpp` also requires its local hash/size entry in `source-imports.json` to be
updated. Neither action changes the recorded donor hash unless the upstream donor
itself changed.

## 7. Code map and change checklist

| Responsibility | Files |
|---|---|
| Root target selection and build ID | `CMakeLists.txt` |
| Presets | `CMakePresets.json` |
| Arm toolchain/linker/target flags | `cmake/` |
| Target composition | `rt1170/CMakeLists.txt` |
| Dependency identity | `dependencies.lock.json`, `THIRD_PARTY_NOTICES.md` |
| Imported-source identity | `docs/source-imports.json` |
| Contract validation | `schema/`, `scripts/verify-standalone.py`, `tests/` |

Source or target additions need source-list updates, presets if behavior differs,
host and Arm warnings-as-errors, map review, provenance checks, extraction/offline
consideration, and documentation ownership in this module index.
