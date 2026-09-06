# Third-party notices and import status

This is an inventory of inspected donor license evidence through the revised
Phase 1.2 storage slice.
The authoritative materialization state and exact local file hashes belong in
[dependencies.lock.json](dependencies.lock.json). A planned or unresolved entry
must not be represented as already imported. Keep complete license texts and
per-file notices with the selected source snapshots.

| Dependency | Observed license evidence | Retained local license path when imported |
| --- | --- | --- |
| NXP MCUX SDK | Selected original-EVK board, WM8960, startup, and linker files declare BSD-3-Clause; root text is COPYING-BSD-3. | third_party/mcux-sdk/COPYING-BSD-3 |
| Arm CMSIS | Apache-2.0 in root LICENSE.txt and Core/Include/core_cm7.h. | third_party/CMSIS/LICENSE.txt |
| FreeRTOS-Kernel | MIT in root LICENSE.md and the selected ARM_CM7/r0p1 port; preserve Amazon copyright notices in source. | third_party/FreeRTOS-Kernel/LICENSE.md |
| TinyUSB | MIT in root LICENSE and selected ChipIdea HS device driver; preserve their copyright notices. | third_party/tinyusb/LICENSE |
| NXP SDMMC middleware | BSD-3-Clause. Every selected source/header declares `SPDX-License-Identifier: BSD-3-Clause`; preserve its Freescale/NXP copyright header. | third_party/sdmmc/COPYING-BSD-3 |
| FatFs middleware | ChaN FatFs License for `ff.c`, `ff.h`, `ffunicode.c`, and `diskio.h`; selected NXP `diskio.c` and SD-disk adapter files declare BSD-3-Clause. `diskio.c` also retains its ChaN skeleton attribution. | third_party/fatfs/LICENSE.txt and third_party/fatfs/SW-Content-Register.txt; BSD-3-Clause evidence remains in selected source headers. |
| M110 WinMM capture donor | First-party donor files do not carry a repository-level license in the inspected snapshot; no license is assigned by inference. | The recorder keeps only localized capture identity and WinMM buffer logic in `host/capture/winmm_audio.hpp` and `host/capture/winmm_audio.cpp`; provenance and exact local hashes are in [docs/source-imports.json](docs/source-imports.json). |

These observations apply to inspected files. Upstream repositories can contain
other components with different notices; a root license is not permission to
omit the notices of any selected file. Vendor snapshots remain unmodified;
fixture adaptations live in local first-party components and retain provenance.
Do not copy unrelated middleware to satisfy an unexplained include.

The retained FatFs `SW-Content-Register.txt` reports release 0.14b, while the
selected `ff.c` and `ff.h` headers identify R0.15 (`ff.c` adds patch3). Treat
the exact upstream commit and per-file hashes as the imported-byte identity;
the content register is license/component evidence, not selected-core version
evidence.

First-party M110 licensing is unspecified in the inspected donor tree. See
[LICENSE](LICENSE) and [docs/source-imports.json](docs/source-imports.json). No
license is assigned by inference from a dependency. The historical donor paths
and commits in these inventories must never become automatic build-time fetch,
import, or parent-checkout lookup instructions.

PathSim remains a separately supplied waveform-preparation tool/artifact source.
No PathSim source is imported into firmware or shared utility code. HFSimulator
integration uses local serial control and artifact boundaries; this inventory
does not claim an HFSimulator source import.
