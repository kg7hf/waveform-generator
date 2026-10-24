# Local vendor adaptations

Upstream dependencies are Git submodules. Three carry local patches here: two
firmware inputs, which both board configurations patch, and pathsim, which the
`channel-sim` build patches.

- `sdmmc-wfg.patch`: identification clock/voltage retry control and diagnostics,
  and a bounded write failure: after a failed write command `SD_Write` accepts
  only an ACMD22 count of 1 to the blocks it sent, so a card that writes none
  fails `SD_WriteBlocks` instead of being sent the same blocks without end
  ([MSC transfers](../docs/modules/protocols-media-and-artifacts.md#msc-transfers-and-throughput)).
- `fatfs-wfg.patch`: reuse the card initialized by the fixture rather than
  initializing it again during filesystem mounting.
- pathsim: the HF lane's rev-6 patch
  (`pathsim-b5fa9ae-gcc14-cli-hilbert-onset-seed-paths-truth.patch`) and
  waveform-generator's draw-order patch for the chain build
  (`pathsim-wfg-embedded.patch`), each with its dated modification notice
  (`*.notice.md`). See the pathsim sections below.

Firmware configure runs `scripts/dependencies.py`, which checks required files,
copies selected sources to `build/<preset>/vendor`, and applies the two firmware
patches there. It does not alter the submodule working trees or download anything.
Patch conflicts are ordinary configure errors to resolve when updating upstream.
`dependencies.json` lists the selected input files and patch paths. Git submodules
handle revisions in the usual way.

## pathsim (rev-6, lane patch)

`pathsim-b5fa9ae-gcc14-cli-hilbert-onset-seed-paths-truth.patch` originated
against bubnikv/pathsim at `b5fa9ae`. Git records the current submodule
revision; the patch must apply to its committed sources:

- The lane built its historical reference `pathsim.exe` from `b5fa9ae` plus this patch.
- The file mixes CRLF and LF line endings because several upstream files are stored with CRLF. Copy it as bytes.
- A verbatim copy cannot carry a header, so its dated modification notice (what, why, GPLv3 section 5a) is
  `pathsim-b5fa9ae-gcc14-cli-hilbert-onset-seed-paths-truth.notice.md`.

Changes:

- Adds `--seed` (one `srand` selects a complete, reproducible fading and noise realization; default 1) and
  `--truth-out` (the M110PTr1 per-sample channel-truth export; passive, the WAV output is unchanged).
- Fixes the two-path composition (`main.cpp`: `--spread`/`--delay2`/`--spread2` address path slots 0/1/2; stock
  built a third, static path), the fading onset (`warm_up_fading`, 4 s, plus a whole-file active-RMS seed of the
  level tracker), the Hilbert quadrature taps (`Delay.cpp`; stock fading was non-circular), the unsequenced noise
  write in `NoiseGen.cpp` that destroys the signal under GCC 14/C++17, and GCC 14 build breaks (missing includes).
- The scriptable flags themselves (`--snr`, `--spread`, `--delay2`, `--spread2`, `--offset2`, `--direct`) are
  stock; `--seed` and `--truth-out` are the reliable "patch applied" indicators.

This patch is not listed in `dependencies.json`: the root build does not use pathsim yet. The standalone
`channel-sim` build applies it at configure to a build-tree copy exported from the checked-out commit and fails if it does not apply cleanly
(`channel-sim/cmake/pathsim_vendor.cmake`, [pathsim guide](../docs/modules/pathsim.md)).

When updating the patch or submodule, check patch applicability and rerun gate 0
(`channel-sim/gates/gate0_reference_parity.py`) with an explicitly supplied external reference-data tree.

### pathsim chain build: RNG shim (2026-10-03) and `pathsim-wfg-embedded.patch` (2026-10-03)

The chain build (`channel-sim`, `pathsim_lib`) changes pathsim in two ways. Both are modifications under GPLv3
section 5a, and `pathsim-wfg-embedded.notice.md` is their dated notice. Configure copies it into the chain's
build-tree copy as `MODIFIED-BY-WAVEFORM-GENERATOR-CHAIN.md`.

1. **The RNG shim** (no source edit). `channel-sim/rng/pathsim_rng_shim.hpp` is force-included into pathsim's library
   sources. It declares `PathSim::rand()`, a UCRT-compatible generator, ahead of pathsim's code, so pathsim's
   unqualified `rand()` calls bind to it by name lookup. Where the C library's `RAND_MAX` differs (newlib), it pins
   `RAND_MAX` to `0x7FFF`. The driver's `seed_pathsim_rng()` replaces `srand`. The build checks that pathsim's
   objects reference `PathSim::rand()` and not the C runtime's `rand`/`srand`
   (`channel-sim/cmake/check_pathsim_rng.cmake`).
2. **`pathsim-wfg-embedded.patch`**, applied after rev-6 to a second build-tree copy (`pathsim-chain-src`).
   `pathsim_ref` keeps rev-6 alone. It sequences the two draws of `v.set(rsample(), rsample())` in `Path.cpp` and
   `NoiseGen.cpp` in the lane binary's order: the first draw goes into the imaginary part. C++ leaves that order
   unspecified. x86-64 GCC (the lane's `pathsim.exe`) and Arm GCC (the firmware) evaluate the arguments in opposite
   orders, so without the patch the RT1170 would swap I and Q in every pair: a different realization for the same
   seed. The shim alone cannot fix this. The patch starts with its own dated header, and configure requires it
   to apply cleanly. Gate 1 shows that the x86-64 output is unchanged. Gate 1b
   (`channel-sim/gates/check_draw_order.py`) shows the order in GCC's GIMPLE for both compilers.

Further chain-build changes go into the same patch and notice, with a new date, never into the submodule. Examples:
a pathsim revision that calls `::rand()`, or one that adds another unsequenced draw pair (configure refuses such a
pair). After any change to either patch, rerun gates 1 to 3 as well (`channel-sim/gates/run_gates.py`).
