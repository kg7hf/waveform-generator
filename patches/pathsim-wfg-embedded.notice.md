# Modification notice: pathsim chain build (waveform-generator)

This is the dated notice (GPLv3 section 5a) for the changes waveform-generator
makes to pathsim in its chain build, on top of the HF lane's rev-6 patch. The
chain build is `pathsim_lib` in `channel-sim/`, which the `pathsim_chain` host
program uses. Firmware uses the separate Signal Lab Watterson implementation. It compiles
pathsim's six library sources (`Delay`, `GaussFIR`, `NoiseGen`, `Path`,
`PathSimParams`, `PathSimProcessor`). The reference CLI `pathsim_ref` is built
from rev-6 alone and is not affected.

`channel-sim` configure copies this file into the chain's build-tree copy
(`<build>/pathsim-chain-src`) as `MODIFIED-BY-WAVEFORM-GENERATOR-CHAIN.md`. The
rev-6 notice goes into the same copy as `MODIFIED-BY-WAVEFORM-GENERATOR.md`.

| Item | Value |
|---|---|
| Work modified | pathsim, <https://github.com/bubnikv/pathsim>, commit `b5fa9aef68a0c2cdd2895f149e9baf5f3bccef51`, plus the rev-6 patch (`pathsim-b5fa9ae-gcc14-cli-hilbert-onset-seed-paths-truth.patch`) |
| Authors | PathSim by Moe Wheatley AE4JY, reworked by Vojtech Bubnik OK1IAK |
| Licence | GNU GPL v3 (`LICENSE`, `COPYING`); `FilterTables.h` (c) 2000 Moe Wheatley AE4JY, GPL v2 or later |
| Modified by | waveform-generator (Paul R. Decker), GPL-3.0-only |

## Dates

- 2026-10-03: the RNG binding (section 1) enters the chain build (commit `c5d2721`).
- 2026-10-03: `pathsim-wfg-embedded.patch` (section 2) is added. This notice now
  covers both changes.

## 1. Random numbers: pathsim's `rand()` bound to a UCRT-compatible generator

What: the header `channel-sim/rng/pathsim_rng_shim.hpp` is force-included
(`-include`) ahead of every pathsim library source in the chain build. No
pathsim file is edited for this, but the compiled behaviour changes:

- The header declares `PathSim::rand()`. pathsim calls `rand()` unqualified
  inside `namespace PathSim` (`Path.cpp`, `NoiseGen.cpp`), so name lookup finds
  that declaration instead of the C library's `rand()`. Every Rayleigh and AWGN
  draw then comes from `channel-sim/rng/pathsim_rng.cpp`, which implements the
  Microsoft C runtime's generator (`state = state * 214013 + 2531011`, output
  `(state >> 16) & 0x7FFF`).
- Where the C library's `RAND_MAX` is not `0x7FFF` (newlib, the RT1170's C
  library, defines `0x7FFFFFFF`), the header redefines `RAND_MAX` as `0x7FFF`
  for pathsim's sources. With the Microsoft runtimes, which already use
  `0x7FFF`, nothing changes.
- `srand(seed)`, which pathsim calls only in `main.cpp`, is replaced by the
  driver's `seed_pathsim_rng(seed)`. The chain does not compile `main.cpp`.

Why: the lane's reference `pathsim.exe` (sha256 `77461211...`) draws from the
Universal C Runtime. A build that used another C library's `rand()` would
produce a different fading and noise realization for the same `--seed`.

## 2. `pathsim-wfg-embedded.patch`: the order of each pair of draws

What: in `Path.cpp` (`Rayleigh::sample`) and `NoiseGen.cpp`
(`add_band_limited_noise`), `v.set(rsample(), rsample())` becomes two named
draws: the first goes into the imaginary part and the second into the real
part. Then `v.set(r_draw, i_draw)` is called.

Why: C++ leaves the order in which a call's arguments are evaluated
unspecified. GCC for x86-64, which built the lane's binary, evaluates the second
argument first, so the first draw becomes the imaginary part. GCC for Arm
evaluates the first argument first. With the same random stream, an Arm build
would therefore swap I and Q in every pair. Each Rayleigh tap would become
`j*conj(h)`, and the AWGN samples would be swapped in pairs. Writing the
x86-64 order explicitly gives every compiler the lane's realization. The
x86-64 output is byte-identical (gate 1). A GIMPLE dump of both compilers
confirms the order (`channel-sim/gates/check_draw_order.py`).

The patch file starts with its own dated header. Configure checks every
touched file's preimage and postimage blob ids. See also
[patches/README.md](README.md) and `docs/modules/pathsim.md`.
