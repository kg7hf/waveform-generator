# Upstream submodules

This directory contains six Git submodules: MCUX SDK, CMSIS, FreeRTOS-Kernel,
TinyUSB, SDMMC, and FatFs. Their upstream URLs are in `.gitmodules`; the parent
Git tree records their revisions in the usual submodule way. The existing
`dependencies.lock.json` contains selected file lists and historical metadata;
normal setup does not enforce a separate SHA lock.

After cloning, run from the project root:

```powershell
.\scripts\init-dependencies.ps1
```

The script initializes only these six dependencies and checks required files.
Nested upstream example dependencies are not needed. Normal configure/build is
offline and never updates submodules. A GitHub source ZIP does not include their
contents; use a Git clone and the setup script.

Project adaptations are explicit patches under `patches/`, applied to selected
source copies in the build directory. Keep the upstream submodules clean.
