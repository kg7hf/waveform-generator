# Phase 2 and 3 report: portable impairment engine and RT1170 real-time integration

Date: 2026-09-06. Follows the [Phase 1 report](phase1-test-waveform-report.md).
Phase 2 delivers the portable C++23 impairment library, its host WAV-rendering
backend and unit tests; Phase 3 integrates the same engine into the RT1170
player so a clean WAV on the microSD is impaired in real time on its way to the
codec. All results are engineering evidence, not formal conformance.

## 1. What was built

```
signal-lab/
├── include/signal_lab/
│   ├── sample_stream.hpp   SampleSource / SampleSink / run_pipeline
│   ├── impairment.hpp      Impairment interface, StageContext, placement constructors, sizes
│   ├── scenario.hpp        Scenario model (same JSON as Phase 1) + parse_scenario
│   ├── mixer.hpp           Engine: PCM16 in -> gain -> stages -> saturate -> PCM16 out, digests
│   ├── json.hpp            fixed-capacity JSON reader (no allocation)
│   └── det_math.hpp        deterministic exp/log/sin/cos, PCG32, Irwin-Hall Gaussian, FNV-1a digest
├── src/  det_math.cpp json.cpp scenario.cpp band_fir.cpp awgn.cpp cw.cpp impulse.cpp fade.cpp
│         sample_slip.cpp mixer.cpp sample_stream.cpp
├── sources.cmake           one source list shared by the host library and the firmware
├── CMakeLists.txt          wfg_signal_lab static library + wfg_signal_lab_tests (ctest wfg-signal-lab-cpp)
└── tests/signal_lab_tests.cpp
host/render/signal_lab_render.cpp   host WAV renderer (writes WAV, sidecar JSON, target PLAY.SCN)
host/capture-session.ps1            controlled waveform_capture session (READY / stop file / FINAL)
rt1170/player.cpp                   engine between the SD read and the ring push; PLAY.SCN loader
rt1170/status_contract.hpp, usb_checkpoint.cpp   fourteen engine_* STATUS fields
```

The engine is waveform-agnostic: it consumes and produces PCM16 mono 48 kHz
blocks and never learns what produced them. Stages are the Phase 1 models
(band-limited AWGN, CW, static crashes with tone or noise ringing, fades,
sample deletion/duplication), placement-constructed into one static 32 KiB
arena, with no heap, no exceptions, no RTTI and no C-library transcendental
calls.

### 1.1 Determinism design

The requirement (prompt section 26) is that the same WAV, scenario and seed
produce the same sample sequence on the host and on the RT1170. The engine
reaches bit identity, not just "practical" reproducibility, by removing every
platform-dependent numeric path:

- `det::exp/log/sin/cos` are fixed-order polynomial evaluations on
  IEEE-754 doubles with exact-constant argument reduction; `libm` results
  differ in their last bits between newlib, MinGW and glibc, so none are used.
- The random source is PCG32 keyed by `(seed, family, stage)` through
  SplitMix64; Gaussian samples are the Irwin-Hall sum of twelve 24-bit
  uniforms (the 513-tap FIR that follows makes the result Gaussian to beyond
  4 sigma).
- Every sum runs in a fixed order and both toolchains compile the library
  with `-ffp-contract=off -fno-fast-math` (sources.cmake), so no fused
  multiply-adds appear on Cortex-M7 that x86-64 would not perform.
- Levels, phases and schedules are keyed by the absolute input frame index,
  so results do not depend on the block size (unit-tested for 2048 versus
  1000 versus 100-frame blocks).
- Quantization is round-half-even with saturation, the Phase 1 convention.

The engine keeps an FNV-1a 64-bit digest of the emitted PCM16 stream and of the
consumed source stream. The host renderer prints both; the firmware reports
both in STATUS. Equal digests prove sample-exact equality without moving the
audio off the board.

### 1.2 Streaming targets and the reference level

Every level in a scenario is relative to the clean source's reference RMS. The
host can measure that from the file; a streaming player cannot look ahead. The
Phase 2 scenario grammar therefore adds one field, `reference_rms`. The host
renderer writes a copy of the scenario with the measured value injected
(`--target-scenario PLAY.SCN`), and the firmware rejects a scenario that lacks
it (`engine_error=4`) rather than guessing.

### 1.3 Firmware integration

`rt1170/player.cpp`: after the WAV header is validated, the player opens
`2:/WG/PLAY.SCN`. If the file is absent the player is a clean pass-through, as
before. If present, it is parsed (8 KiB limit), the engine is configured for the
file's frame count, and every 4 KiB SD chunk (2048 frames) is processed in the
priority-4 player task between `read_payload_chunk()` and `push_samples()`. A
rejected scenario is reported (`engine_state=2`, `engine_error`) and playback
continues clean so a bad document never bricks the card. The priority-7 audio
hook is untouched. The player task stack grew from 1536 to 4096 words: the
first on-target attempt crashed because a FatFs `FIL` and a 4 KiB kernel
scratch array lived on the 6 KiB stack; both are static now.

New STATUS fields: `engine_state` (0 none, 1 active, 2 rejected),
`engine_error` (1 open, 2 read, 3 parse, 4 missing reference_rms,
5 configure, 6 too large), `engine_stages`, `engine_frames_in`,
`engine_frames_out`, `engine_clipped`, `engine_digest_hi/lo`,
`engine_source_digest_hi/lo`, `engine_max_block_cycles`,
`engine_events_applied`, `engine_events_dropped`, `engine_arena_bytes`.
The worst-case STATUS line is now 1878 bytes (`cdc_response_capacity` 2560;
`tests/contract_tests.cpp` pins the new value).

Firmware footprint after the section 5 revision: DTCM (`m_data`) 177 872 of
262 144 bytes (68 %), OCRAM2 (`m_data2`) 128 KiB for the output FIFO; flash
text 112 KB. (The first integration kept the FIFO in DTCM at 87 %.)

## 2. Host validation

| Check | Result |
|---|---|
| `wfg_signal_lab_tests` (deterministic math vs `std::` to 2e-14, JSON exactness vs `strtod`, scenario parsing, block-size independence, digests, levels, slips) | PASS |
| `ctest --preset host-release` | 8/8 (contract-cpp, capture-core, signal-lab-cpp, contract-json, stage-wav, analyze-capture, signal-lab (Python), source-boundary) |
| C++ versus Phase 1 Python engine, deterministic models (CW +3 dB, 18 dB fade, 48-sample deletions), 5.8 M samples | max difference 1 LSB, identical length |
| C++ versus Python, stochastic models (corner noise, moderate crashes, MIX-005) | levels equal within 0.1 dB RMS; different realizations by design (different RNG) |
| Host render of MIX-003 decoded by `m110_app_decode --engine siso` | exact, 0 bit errors, EOM found |

## 3. Target validation (MIMXRT1170-EVK, WM8960, Realtek microphone capture)

Firmware: `rt1170-player-release`, ELF SHA-256 in each run's `firmware.sha256`;
flashed with LinkServer 26.6.137 over the on-board CMSIS-DAP probe (no
elevation was needed with TEMP redirected into the build tree). Source on the
card: `m110_600L_reference_perfect.wav` (2 min, 5 808 000 frames) staged with
`stage_wav.py`, plus `PLAY.SCN` from the host renderer.

| Run (artifacts/runs/…) | Scenario | Host digest | Target digest | Frames out | Clipped | Events | Result |
|---|---|---|---|---|---|---|---|
| 20260906T162043Z…mix003 | MIX-003: fade, AWGN, CW | 4d8958d7f45d9d20 | 4d8958d7f45d9d20 | 5 808 000 = 5 808 000 | 0 = 0 | 14 = 14 | bit-exact |
| 20260906T163911Z…mix005 | MIX-005: fade, AWGN, CW, crashes, slips | d2146c161d5d44a8 | d2146c161d5d44a8 | 5 807 975 = 5 807 975 | 1152 = 1152 | 136 = 136, 0 dropped | bit-exact |
| 20260906T164711Z…analog, 20260906T165248Z…analog | MIX-003 replayed three more times | 4d8958d7f45d9d20 | 4d8958d7f45d9d20 (x3) | 5 808 000 | 0 | 14 | reproducible |

Source digests (`b8168de6c8edeb95`) matched on every run, which also proves the
SD read path delivered the file byte-exactly. Every run finished with
`player_state=7`, `pcm_drained=1`, `underruns=0`, `sd_read_errors=0`,
`audio_tx_errors=0` and `ring_min_pre_eof_frames=16000` of 32 768.

Engine cost on the 996 MHz CM7: `engine_max_block_cycles` 14.3–15.4 M per
2048-frame block, i.e. 34–36 % of the 42.7 ms block period at the worst block
(the 513-tap band-limiting FIR dominates). Real time holds with margin
(the ring never dipped below its pre-EOF minimum), but this is the first
optimisation target for Phase 4 (CMSIS-DSP FIR, half-band kernel).

### 3.1 Analog end-to-end

With the WM8960 output cabled into the Realtek microphone input and captured
by `waveform_capture --backend wasapi-raw --channel left` (run
20260906T165248Z, playback 5):

| Capture | Engine | Result |
|---|---|---|
| raw capture, signal RMS −36 dBFS | siso | 33 420 errors, EOM not found |
| raw capture | adaptive | loss of lock after 344 bytes |
| capture +12 dB digital gain (RMS −24 dBFS) | siso | **exact: 0 bit errors, EOM found** |
| capture +12 dB | adaptive | loss of lock (same as the digital corpus for MIX-003) |

So the impaired message survives the whole chain (RT1170 engine → DAC → cable →
ADC → PC decoder) once the absolute level is brought back into the turbo
engine's working range. The raw-level failure is the absolute-level sensitivity
found in Phase 1 (section 6.2 of that report), now reproduced on real hardware
at −36 dBFS. The first two capture attempts were silent (a cable fault, replaced
by the owner) and the recorder helper needed two fixes (a blocking read and an
event-handler thread issue); the final helper is `host/capture-session.ps1`.
The recorder ended that capture with `CAPTURE_ERROR` after playback (a
discontinuity after the audio stopped); the WAV was finalized and complete.

## 4. Procedure (reproducible)

```powershell
# host
cmake --preset host-release; cmake --build --preset host-release --parallel; ctest --preset host-release
build\host-release\host\render\signal_lab_render.exe --scenario case.json --source clean.wav --output impaired.wav --sidecar impaired.json --target-scenario PLAY.SCN
# firmware
cmake --preset rt1170-player-release; cmake --build --preset rt1170-player-release --parallel
& 'C:\NXP\LinkServer_26.6.137\LinkServer.exe' flash --probe <serial> 'MIMXRT1176xxxxx:MIMXRT1170-EVK' load -e build\rt1170-player-release\waveform_generator.elf   # TEMP/TMP redirected into the build tree
& 'C:\NXP\LinkServer_26.6.137\LinkServer.exe' probe <serial> wiretimedreset 100
# card (fixture in host mode after boot; G: mounted)
python host/stage_wav.py stage clean.wav --card-root G:\ --level-percent 70 --force ; Copy-Item PLAY.SCN G:\WG\PLAY.SCN
Write-VolumeCache -DriveLetter G ; .\host\media-control.ps1 -Port COM8 -Action Local -HostVolumeDismounted
# capture + play
.\host\capture-session.ps1 -Output run\capture.wav -DeviceName "Microphone (Realtek(R) Audio)" -StopFile run\stop.flag -JsonlPath run\capture.jsonl
.\host\media-control.ps1 -Port COM8 -Action Play ; poll -Action Status until player_state=7 ; New-Item run\stop.flag
# verify: engine_digest_hi/lo == host digest; decode the capture with m110_app_decode
```
One PLAY per boot: reset the fixture (wiretimedreset) between runs.

## 5. Phase 3 revision: producer-paced output FIFO (owner direction, same day)

The owner's revised topology puts the only large buffer *after* the impairment
engine and treats everything upstream as a producer that merely has to keep
that FIFO ahead of the DAC:

```
SD / WAV --4 KiB working buffer--> impairment engine --> OUTPUT FIFO (OCRAM2, 1365 ms)
                                                              |  hard real-time boundary
                                                              v
                                                   128-frame audio hook -> SAI/eDMA -> WM8960
```

Implementation (`rt1170/player.cpp`, `cmake/MIMXRT1176xxxxx_cm7_flexspi_nor_wfg.ld`):

- The output FIFO is 65 536 frames (128 KiB) in OCRAM2 through a generator-local
  copy of the vendor linker script that adds a NOLOAD `.ocram` section in
  `m_data2` (import ledger m110-026). DTCM use fell from 87 % to 68 %.
- Watermarks: high 38 400 frames (800 ms, stop producing and start the codec),
  wake 24 000 (500 ms, resume), critical 4 800 (100 ms, counted as a near-miss).
  A whole engine block always fits above the high watermark (static_assert), so
  a block is pushed in one go and is never deadline-bound; only the audio hook
  draining the FIFO has a deadline.
- The SD side keeps one 4 KiB working buffer (one engine block). A ping-pong
  buffer would only help with an asynchronous SD read; FatFs `f_read` is
  synchronous here, so it was not added.
- New STATUS fields: `ring_capacity_frames`, `ring_high_watermark_frames`,
  `ring_wake_watermark_frames`, `ring_critical_frames`, `ring_avg_frames`,
  `ring_critical_events`, `producer_rate_sps`, `producer_worst_block_cycles`,
  `producer_sleeps` (worst-case STATUS line 2172 bytes). With the existing
  `ring_min_pre_eof_frames`, `max_sd_read_cycles`, `engine_max_block_cycles` and
  `underruns` these are exactly the safety metrics the revision asked for.

Measured on the board (run 20260906T171550Z, MIX-003, digest `4d8958d7f45d9d20`
= host, sixth match):

| Metric | Value |
|---|---|
| Output FIFO capacity / high / wake / critical | 1365 / 800 / 500 / 100 ms |
| FIFO average depth | 655 ms |
| FIFO minimum depth before EOF | 488 ms (the producer wakes at 500 ms and refills within one block) |
| Critical-level events / underruns | 0 / 0 |
| Producer rate while active | 201.5 ksps = 4.2× real time; 282 watermark sleeps in 121 s |
| Worst block (SD read + engine + push) | 12.4 ms of a 42.7 ms block period |
| Worst SD read / worst engine block | 3.7 ms / 9.4 ms (22 % of the block period) |

Analog capture on the same run, after the owner raised the PC microphone gain
by 50 %: playback RMS −28.7 dBFS, peak −6.0 dBFS. The raw capture decodes on
the turbo engine with the EOM found and 96 bit errors of 66 904; +6 dB
(−22.7 dBFS) and +12 dB copies decode exactly. The turbo engine's absolute-level
cliff for this impaired signal therefore lies between −28.7 and −22.7 dBFS RMS;
the adaptive engine loses lock on MIX-003 at every level, as in the digital
corpus.

## 6. Caveats and Phase 4 recommendations

- **CPU**: the engine's worst block costs 22 % of the block period (34–36 %
  was measured on the first integration, which included cold caches at the
  first blocks); with the producer at 4.2× real time there is room for more
  stages, but the 513-tap band FIR remains the first thing to optimise
  (CMSIS-DSP `arm_fir_f32`, or a 257-tap kernel with its hash in the scenario)
  before Watterson fading joins the chain.
- **Arena**: 32 KiB holds five stages (MIX-005 used 21 KiB); the sample-slip
  stage caps runs at 1024 samples, the Python engine allowed 48 000.
- **Events**: `max_events` is 32 per stage (explicit fade starts, slip events);
  periodic and Poisson schedules are unbounded.
- **Level**: the analog path delivers about −36 dBFS RMS at codec level 70 %;
  the PC turbo engine needs roughly −24 dBFS or better. Either raise the codec
  level / PC input gain in the capture contract, or fix the receiver's
  absolute-level dependence (the real defect).
- **Protocol**: Phase 4 should replace the one-shot P1.2 `PLAY` with the
  WFG/1 commands the docs already define, add `LOAD <scenario>`, `CW ON/OFF`,
  `FADE NOW`, sweep commands, and re-arm without a reset. The engine already
  supports open-ended windows (`total_frames = 0`) for live sources.
- **Digest scope**: the digest covers the PCM handed to the ring; the codec
  path after it is the P1.2 transport, unchanged.
- Nothing here is committed; the nested repository still has one commit.
