# Third-party notices

Project-owned work is GPL-3.0-only; see [LICENSE](LICENSE). This does not replace
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

Preserve notices when changing or redistributing dependency code. Upstream
repositories may include additional components with their own terms. Local
SDMMC/FatFs adaptations are applied through `patches/` to build-directory copies.

The FatFs content register identifies a package release; individual source
headers identify their component versions. Refer to those files when updating
the dependency.
