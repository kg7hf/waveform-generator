# Third-party notices

SPDX-FileCopyrightText: 2026 Paul R. Decker

Project-owned work is GPL-3.0-only;
see [COPYRIGHT](COPYRIGHT) and [LICENSE](LICENSE). This does not replace
the existing notices and terms on third-party components or adapted vendor files.

Dependencies retain their original copyright and license notices in the Git
submodules. The following table identifies the selected components and the
license files that accompany them.

| Dependency | Observed license evidence | Retained local license path when imported |
| --- | --- | --- |
| NXP MCUX SDK | Selected original-EVK board, WM8960, startup, and linker files declare BSD-3-Clause; root text is COPYING-BSD-3. | third_party/mcux-sdk/COPYING-BSD-3 |
| Arm CMSIS | Apache-2.0 in root LICENSE.txt and Core/Include/core_cm7.h. | third_party/CMSIS/LICENSE.txt |
| FreeRTOS-Kernel | MIT in root LICENSE.md and the selected ARM_CM7/r0p1 port; preserve Amazon copyright notices in source. | third_party/FreeRTOS-Kernel/LICENSE.md |
| TinyUSB | MIT in root LICENSE and selected ChipIdea HS device driver; preserve their copyright notices. | third_party/tinyusb/LICENSE |
| NXP SDMMC middleware | BSD-3-Clause. Every selected source/header declares `SPDX-License-Identifier: BSD-3-Clause`; preserve its Freescale/NXP copyright header. | third_party/sdmmc/COPYING-BSD-3 |
| FatFs middleware | ChaN FatFs License for `ff.c`, `ff.h`, `ffunicode.c`, and `diskio.h`; selected NXP `diskio.c` and SD-disk adapter files declare BSD-3-Clause. `diskio.c` also retains its ChaN skeleton attribution. | third_party/fatfs/LICENSE.txt and third_party/fatfs/SW-Content-Register.txt; BSD-3-Clause evidence remains in selected source headers. |
| pathsim (HF channel simulator) | GNU GPL v3 in root LICENSE and COPYING; `FilterTables.h` (c) 2000 Moe Wheatley AE4JY, GPL v2 or later; `AudioFile.h` GPL v3 or later and `cxxopts.h` MIT (host CLI only). See the pathsim section below. | third_party/pathsim/LICENSE and third_party/pathsim/COPYING |
| Catch2 (unit-test framework, host tests only) | Boost Software License 1.0 in root LICENSE.txt; every compiled source declares `SPDX-License-Identifier: BSL-1.0`. The CMake helpers `extras/Catch.cmake` and `extras/CatchAddTests.cmake` are BSD-3-Clause (from CMake). See the Catch2 section below. | third_party/Catch2/LICENSE.txt |

Preserve notices when changing or redistributing dependency code. Upstream
repositories may include additional components with their own terms. Local
SDMMC/FatFs adaptations are applied through `patches/` to build-directory copies.

The optional [`modem-core` encoder](examples/modem-core-encoder/README.md) uses
source from a separately supplied [public modem-core repository](https://github.com/kg7hf/modem-core).
It is not one of this repository's submodules and is absent from default builds.
The transmitter source in the currently tested checkout declares
`SPDX-License-Identifier: GPL-3.0-or-later` and that checkout includes the GPLv3
text in `COPYING`. Its copyright and license notices remain with that separate
source; this project does not claim or copy the modem implementation. A build
that includes the optional encoder must retain the notices and source for the
specific modem-core checkout used.

The FatFs content register identifies a package release; individual source
headers identify their component versions. Refer to those files when updating
the dependency.

## Adapted NXP linker scripts

`cmake/MIMXRT1064xxxxx_flexspi_nor_wfg.ld` and
`cmake/MIMXRT1176xxxxx_cm7_flexspi_nor_wfg.ld` are adapted NXP MCUXpresso SDK
linker scripts. Their Freescale/NXP notices remain in each file. Paul R.
Decker's 2026 adaptation notices cover the local memory-layout changes only;
these two files, including the adaptations, retain BSD-3-Clause terms.
A copy of the upstream license is included at
[LICENSES/BSD-3-Clause-NXP.txt](LICENSES/BSD-3-Clause-NXP.txt), so the terms
remain available even before submodules are initialized.

## pathsim

**PathSim by Moe Wheatley AE4JY, reworked by Vojtech Bubnik OK1IAK.** PathSim
began as Moe Wheatley's Windows HF path simulator (2000); Vojtech Bubnik ported
it to a portable command-line tool. Upstream: <https://github.com/bubnikv/pathsim>,
used as the submodule `third_party/pathsim`, unmodified. Git records the
revision checked out for a particular build.

| Files | Licence |
|---|---|
| pathsim as a whole (`Delay`, `GaussFIR`, `NoiseGen`, `Path`, `PathSimParams`, `PathSimProcessor`, `cmplx.h`, `main.cpp`) | GNU GPL v3 (`LICENSE`, `COPYING`) |
| `FilterTables.h` | (c) 2000 Moe Wheatley AE4JY, GNU GPL v2 or later (used here under v3) |
| `AudioFile.h` (WAV I/O, host CLI only) | (c) 2017 Adam Stark, GNU GPL v3 or later |
| `cxxopts.h` (option parsing, host CLI only) | (c) 2014-2017 Jarryd Beck, MIT (its notice is kept in the file) |

All of these are compatible with this project's GPL-3.0-only licence. Every
licence file and copyright header is kept unchanged.

Modifications (GPLv3 section 5a): the HF compliance lane's rev-6 patch
`patches/pathsim-b5fa9ae-gcc14-cli-hilbert-onset-seed-paths-truth.patch`, copied
from the original reference implementation (first recorded 2026-09-18),
with its dated notice
`patches/pathsim-b5fa9ae-gcc14-cli-hilbert-onset-seed-paths-truth.notice.md`
(copied into waveform-generator 2026-10-03). It fixes the Hilbert quadrature
taps (bug reported by Steve Pinkham N4FPV), the noise-loop sequencing, the
two-path option parsing and the fading onset, and adds `--seed` and
`--truth-out`. The patch is applied only to build-tree copies, which also
receive the notice as `MODIFIED-BY-WAVEFORM-GENERATOR.md`; the submodule stays
pristine.

The chain build (`channel-sim`, `pathsim_lib` in `pathsim_chain`) modifies
pathsim further. Its dated notice is
`patches/pathsim-wfg-embedded.notice.md` (2026-10-03), which configure copies
into the chain's build-tree copy as `MODIFIED-BY-WAVEFORM-GENERATOR-CHAIN.md`.
It covers two changes:

- `patches/pathsim-wfg-embedded.patch`, applied after rev-6 to a second
  build-tree copy. In `Path.cpp` and `NoiseGen.cpp`, the two random draws of
  each complex pair are sequenced in the lane binary's order. C++ leaves that
  order unspecified, and x86-64 and Arm compilers differ.
- The RNG shim `channel-sim/rng/pathsim_rng_shim.hpp`, force-included at
  compile time. It edits no pathsim file but changes the compiled behaviour.
  pathsim's `rand()` calls bind to `PathSim::rand()`, a UCRT-compatible
  generator, instead of the C library's. `RAND_MAX` is pinned to `0x7FFF`
  where the C library differs (newlib). `srand` is replaced by the driver's
  `seed_pathsim_rng()`.

The chain driver that calls pathsim's classes is waveform-generator's own code
(GPL-3.0-only).

Corresponding source: this repository at the commit of a build, that is the
`third_party/pathsim` submodule revision, `patches/pathsim-*`, and the build scripts,
shim and driver in `channel-sim/` (step 5a: host builds only; no firmware image
contains pathsim yet). See [docs/modules/pathsim.md](docs/modules/pathsim.md).

## Catch2

**Catch2, a C++ unit-test framework, copyright the Catch2 Authors** (the notice
in every source). Upstream: <https://github.com/catchorg/Catch2>,
used as the submodule `third_party/Catch2`, unmodified. Git records the revision
checked out for a particular build.

| Files | Licence |
|---|---|
| Catch2 as a whole (`src/catch2`, the CMake project) | Boost Software License 1.0 (`LICENSE.txt`, `SPDX-License-Identifier: BSL-1.0` in each source) |
| `extras/Catch.cmake`, `extras/CatchAddTests.cmake` (ctest registration) | BSD-3-Clause, distributed with CMake (notice in each file) |

Both are permissive and compatible with this project's GPL-3.0-only licence;
nothing is patched. Catch2 is a host-test dependency only: the host test
executables link it, and no firmware image compiles or contains it
(`cmake/wfg_catch2.cmake`). The Boost licence does not require its notice in
machine-executable object code, so the test executables carry none.
