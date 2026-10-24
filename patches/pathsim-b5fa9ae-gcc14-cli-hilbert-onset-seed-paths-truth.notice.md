# Modification notice: pathsim rev-6 patch

This is the dated header (GPLv3 section 5a) for
`pathsim-b5fa9ae-gcc14-cli-hilbert-onset-seed-paths-truth.patch`. The patch is a
byte-preserved reference patch. Its mixed line endings and checked digest
are part of the build input, so this separate file carries its modification notice.
`channel-sim` configure copies it into the patched build-tree copy of pathsim as
`MODIFIED-BY-WAVEFORM-GENERATOR.md`, so the modified files carry the notice.

| Item | Value |
|---|---|
| Work modified | pathsim, <https://github.com/bubnikv/pathsim>, commit `b5fa9aef68a0c2cdd2895f149e9baf5f3bccef51` |
| Authors | PathSim by Moe Wheatley AE4JY, reworked by Vojtech Bubnik OK1IAK |
| Licence | GNU GPL v3 (`LICENSE`, `COPYING`); `FilterTables.h` (c) 2000 Moe Wheatley AE4JY, GPL v2 or later |
| Patch sha256 | `c8cf692a187d33f7103ba98957e2c480d450d28b6d1f59972423b9f400cbd585` |

## Dates

- 2026-09-18: the reference patch is first recorded.
- 2026-09-25: the lane's `pathsim.exe` (sha256 `77461211...`, "rev-6") is built from `b5fa9ae` plus this patch.
- 2026-10-03: the patch is copied verbatim into waveform-generator `patches/`, with this notice.

## What changes, and why

| File | Change | Why |
|---|---|---|
| `Delay.cpp` | The Hilbert filter takes the quadrature part from the Q taps; adds `<cstring>` | Stock code applied the I taps to both parts, so the path input was not analytic and the Rayleigh fade was non-circular (envelope mean/rms about 0.78 instead of 0.886). Reported by Steve Pinkham N4FPV (cf. bubnikv/pathsim#3). `<cstring>` fixes the GCC 14 build. |
| `NoiseGen.cpp`, `NoiseGen.h` | `pInOut[i] = siggain * pInOut[i] + acc; ++i;`; optional `noise_out` capture | The stock `pInOut[i] = siggain * pInOut[i ++] + acc;` is sequenced right-to-left under C++17 (GCC 14 default), so every store lands one sample ahead and the input is destroyed. The capture feeds `--truth-out` and does not alter the output. |
| `Path.cpp`, `Path.h` | Optional per-sample complex path-gain capture; adds `<cstring>` | Feeds `--truth-out`; the output arithmetic is unchanged. |
| `PathSimProcessor.cpp`, `PathSimProcessor.h` | `seed_signal_rms`, `warm_up_fading`, `enable_truth_capture` (M110PTr1 export); adds `<cstdint>`, `<cstdio>`, `<string>`, `<vector>` | File mode: the level tracker starts from the whole-file active RMS, and the fading generators are pre-rolled, so the commanded SNR and steady-state fading hold from the first sample. The truth export is passive. |
| `main.cpp` | `--seed` (`srand`, default 1) and `--truth-out`; `--spread`/`--delay2`/`--spread2` (and the `3` options) address path slots 0, 1 and 2; the input/output check tests only the parsed `input_file`/`output_file` (stock also tested the parser's left-over `argc`); active-RMS seed (samples with \|v\| > 1e-4) and a 4 s fading warm-up before the block loop | Reproducible realizations; stock parsing built a third, static path for `--spread S --delay2 D --spread2 S`; file-mode SNR and fading onset as above. |

Build and update instructions: [Local vendor adaptations](README.md).
