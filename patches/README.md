# Local vendor adaptations

Upstream dependencies are Git submodules. Two need small local adaptations:

- `sdmmc-wfg.patch`: identification clock/voltage retry control and diagnostics.
- `fatfs-wfg.patch`: reuse the card initialized by the fixture rather than
  initializing it again during filesystem mounting.

RT1170 configure runs `scripts/dependencies.py`, which checks required files,
copies selected sources to `build/<preset>/vendor`, and applies these patches
there. It does not alter the submodule working trees or download anything.
Patch conflicts are ordinary configure errors to resolve when updating upstream.
`dependencies.json` lists the selected input files and patch paths. Git submodules
handle revisions in the usual way.
