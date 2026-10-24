# Upstream submodules

This directory contains Git submodules. Six are firmware inputs: MCUX SDK, CMSIS,
FreeRTOS-Kernel, TinyUSB, SDMMC, and FatFs. Catch2 (pinned at release v3.16.1) is
the unit-test framework of the host tests only; no firmware image compiles it.
pathsim is the HF channel simulator of the separate channel-sim build
([docs/modules/pathsim.md](../docs/modules/pathsim.md)). Their upstream URLs are in
`.gitmodules`; the parent Git tree records their revisions in the usual submodule
way. `dependencies.json` lists the files each build needs and, for Catch2, the
pinned release and commit; it holds no file hashes.

After cloning, run from the project root:

```powershell
.\scripts\init-dependencies.ps1
```

The script initializes the dependencies in `dependencies.json` (the six firmware
inputs and Catch2) and pathsim, checks required files and checks that Catch2 is at
its pinned commit (channel-sim's configure checks pathsim's pin). Nested upstream
example dependencies are not needed. Normal configure/build is offline and never
updates submodules. A GitHub source ZIP does not include their contents; use a Git
clone and the setup script.

Project adaptations are explicit patches under `patches/`, applied to selected
source copies in the build directory. Keep the upstream submodules clean.
