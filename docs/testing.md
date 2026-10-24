# Testing on demand

Run commands from the repository root in PowerShell. Use a separate build or
run directory for concurrent work. Generated WAVs, captures, journals, analysis
and reports stay under ignored `build/` or `artifacts/`; they are not maintained
documentation. Use a new run directory instead of overwriting a previous run.

## Choose the check

| Change or question | Check | What passing establishes |
|---|---|---|
| Source or host-tool behavior | Host build and CTest | Covered software contracts |
| Board-specific source, shared player or DSP | Both firmware cross-builds | Compilation, linking and build-time memory/stack gates |
| Stochastic DSP or bandwidth calibration | Statistical campaign | The specified detector/statistical criteria on those inputs |
| USB playback, CW or PC scheduling | Physical capture and comparison | Delivery on that board, firmware, PC, endpoint and workload |
| Decoder behavior | External decoder/reference campaign | Results for the supplied decoder and vectors |

None of these checks alone establishes the others. A short smoke run does not
replace the long statistical campaign, and USB transport results do not validate
an analog codec path.

## Test layers and current scope

The project uses the same CMake/CTest tests and scripts for local checks and CI.
GitHub Actions selects an execution environment; it does not define different
acceptance rules. Use the smallest layer that answers the question, and report
which layer actually ran:

| Layer | Current waveform-generator selection | Status |
|---|---|---|
| Fast developer check | Focused CTest expression or one script test in a local build | Available on demand |
| Pull-request validation | Host build, CTest, policy gates, PathSim unit tests and Windows capture cross-build | Workflow implemented; public hosted execution needs available Actions budget |
| Main-branch regression | The same four device-free CI jobs on each push to `main` | Implemented; broader automatic selection is planned |
| Distributed battery | Selected statistical campaigns, long renders and multi-PC/board runs | Individual scripts exist; no unified on-demand distributed selector |
| Hardware smoke | Board WAV/CW playback, USB capture and exact sample comparison | Maintainer-run, board and endpoint specific |
| Live endurance | Sustained, uninterrupted playback/capture and health counters | Explicit on-demand campaign, not a routine CI gate |
| Release qualification | Review exact source, submodules, builds, tools, firmware, test selections and physical evidence | Manual release gate; publication is separate |

The current CTest catalog can select one case or a name expression, but it does
not yet assign every test the full timing/platform/channel/claim labels used by
the wider test strategy. Until that work is done, state those dimensions in the
run report: `offline` or `live`; `host` or `board`; `generated` or
`live-channel`; and `regression` or `conformance`. A Windows cross-build is a
compile/link result, not native Windows execution. A host simulation is not a
board result. Mark missing prerequisites, unavailable hardware and tests not
run separately from product failures.

Capture the exact source and submodule revisions, toolchain, build options,
selected tests, inputs and seeds with a result. For hardware, also record the
firmware, board, endpoint, format, duration and continuity counters. Keep the
default result concise and retain larger local diagnostics only when needed to
investigate a failure. CI does not upload captures, WAVs or build bundles.
Golden data packs and an automatic distributed selector are future work; the
default public host suite uses its checked-in and generated inputs without
private data, machines or credentials.

## Host tests

Install CMake 3.27 or newer, Ninja, a C++23 compiler and Python 3.10 or newer.
Initialize only the host dependencies, install the Python test packages, and
run the same configure/build/CTest workflow that Linux CI runs:

```powershell
git submodule update --init --depth 1 third_party/tinyusb third_party/Catch2
python -m pip install -r host/requirements-test.txt -r host/requirements-control.txt
cmake --workflow --preset host-tests
```

The shared preset uses Ninja and writes to `build/host-tests`; it disables
optional encoder plugins. CTest keeps its diagnostic logs in that build tree.
For the existing Windows `host-release` preset, `scripts/test.ps1` removes
successful-run CTest logs and retains them after a failure. Do not run two test
sessions in the same build directory.

The command above is the **basic host suite**. A complete CI-equivalent run
also needs clang-format 19, clang-tidy 19, clang-query 19, PowerShell 7 and
lizard 1.24.0, then the style, complexity, layout and static-analysis commands
in the [CI workflow](../.github/workflows/host-ci.yml). CMake conditionally omits some
CTest policy cases when their tools are missing; a local CTest pass alone does
not establish that every CI gate ran. Board firmware and hardware checks remain
separate.

For a focused change, list tests before selecting a regular expression:

```powershell
ctest --test-dir build/host-tests -N
ctest --test-dir build/host-tests -R 'prepared|retain|finite' --output-on-failure
```

Keep the style, complexity and static-analysis policy in the
[coding standard](coding-standard.md), including its tool prerequisites.
The [CI guide](development/hosted-ci.md) describes the GitHub-hosted checks.

## Firmware cross-builds

```powershell
cmake --preset rt1170-player-release -DWFG_ENABLE_ENCODER_PLUGINS=OFF
cmake --build --preset rt1170-player-release --parallel
cmake --preset rt1064-player-release -DWFG_ENABLE_ENCODER_PLUGINS=OFF
cmake --build --preset rt1064-player-release --parallel
```

These commands build without flashing. Also build the relevant read-only or
plugin variant when changing its conditional paths. Refer to the
[RT1170 pipeline](modules/rt1170-player-and-audio.md) and
[RT1064 port](modules/rt1064-board.md) for memory, startup and hardware details.

## Demo smoke checks

After building host-release, use fresh output directories:

```powershell
.\scripts\demo-waveform.ps1 -OutputDirectory build/demo-check
python host/stage_wav.py inspect build/demo-check/clean.wav
python host/stage_wav.py inspect build/demo-check/cw.wav
python host/stage_wav.py inspect build/demo-check/field.wav
.\scripts\demo-download.ps1 -OutputDirectory build/download-check
```

The first script creates a source and renders CW and mixed effects. The second
downloads its documented sample, converts it explicitly, and renders the same
mixtures. Neither plays audio unless `-Play` and an exact output device are
supplied. File-format inspection is not an acoustic playback check. See
[how to use](how-to-use.md) for the device-selection examples.

## Physical USB playback and CW

Use the [Windows bench runbook](windows-usb-bench.md) for both boards. It owns
discovery, profile creation, serial-bound card staging and capture commands.
The [manual capture comparison](usb-sound-card-bringup.md#7-play-and-check-bit-exact)
explains exact PCM24-to-float comparison and the required per-play counters.

For each run:

1. Select the actual board serial and associated physical recording endpoint.
   Record firmware identity, source/scenario and intended duration.
2. Stage and read back the WAV, dismount/eject the medium, and transfer ownership
   to firmware only after the host operation succeeds.
3. Open all capture clients, allow the USB stream to settle, and inspect `USB?`
   for actual stereo 48 kHz, 24-bit negotiation. A float32 capture format does
   not establish the USB bit depth.
4. Run a short clean play before adding effects. For live CW, record all commands
   and acknowledged sample positions in one journal; use the effective reference
   RMS reported for that source and keep the source/scenario unchanged.
5. Wait for terminal playback status before stopping capture. Check both board
   and capture diagnostics for underruns, missed intervals and incomplete output.
6. Compare the entire active capture against the expected PCM24 samples, aligned
   to the start of playback. Check frame count, every sample, and surrounding
   silence. A correlation peak or matching board-side CRC alone is insufficient.

For a live-CW run, generate the expected mixture from its journal. The following
uses a clean source with no static scenario, and assumes `build/usb-check/`
contains that run's `play.jsonl` and exact `source.wav`:

```powershell
python host/live_control.py export build/usb-check/play.jsonl build/usb-check/play.replay.json
build/host-release/host/render/signal_lab_render.exe `
  --source build/usb-check/source.wav --live-events build/usb-check/play.replay.json `
  --output build/usb-check/expected.wav --sidecar build/usb-check/expected.json
```

Compare the reconstructed output using the repository's streaming comparator:

```powershell
python host/make_playlist.py compare --expected build/usb-check/expected.wav `
  --capture build/usb-check/capture.wav --stream-bits 24
```

This checks each captured channel against the mono expected stream and checks
silence after it. Its default alignment uses the first nonzero sample. An
all-silent expected stream requires `--capture-start`; when supplying an
explicit start, check silence before that position separately using the manual
capture check. Do not use `--sent-frames` to turn an unintentionally truncated
run into a successful complete-play test.

If the board also used a static scenario, provide that identical scenario with
`--scenario`. See [live replay](modules/live-control-and-replay.md) for identity,
seed, applied-frame and CUT behavior. Compare with `expected.wav`, not the clean
source. Python's statistical reference renderer is not a bit-exact substitute
for the C++ engine's stochastic output.

Extend the short test to the desired duration using a source of that length.
Report the channel actually captured; a left-channel comparison does not test
the right channel. Repeat host-load, second-PC and analog cases separately.
`usb_bench.py` launches operations but does not automate this entire acceptance
sequence or issue a zero-dropped-samples verdict. Use the streaming comparator above for long captures; the inline manual
comparison loads complete files into memory.

## Prepared waveform comparisons

Follow [prepared artifacts](prepared-artifacts.md) to verify a board WAV/JSN
pair and compare it with an offline C++ retained render. Use identical source
bytes, scenario, effective reference RMS and seed. Compare generation first;
then play the same verified file through USB to test delivery separately.
PathSim is a statistical reference, not a sample-identical Watterson renderer.

Ordinary PREPARE followed by PLAY is sequential. Experimental PREPAREPLAY has
separate accounting, storage-visibility and rate/lead requirements; see
[finite playback](modules/finite-playback.md). Host simulation does not establish
SD latency or contended board throughput. A finalized artifact and successful
playback are separate outcomes and both must be checked.

## External reference comparisons

PathSim's optional parity gates require a separately supplied reference-data
tree. Its executables, decoder tools and datasets are not included in the public
checkout. Use the [PathSim guide](modules/pathsim.md) for the separate build and
gate commands. Their `--lane-root` must name the actual reference tree.

The [Watterson method](design/watterson-validation.md) documents the channel
criteria, external prerequisites and decoder comparisons. Pass the current host
build explicitly; the runner's default points to an older local build layout:

```powershell
python signal-lab/validation/run_watterson_validation.py `
  --build build/host-release --pathsim-build build/main/pathsim-host `
  --lane-root 'C:/reference-data' --work build/watterson-check
```

Replace `C:/reference-data` with the supplied dataset. `--quick` reduces the
campaign to a smoke check; incomplete coverage is not acceptance. Decoder rows
require their decode/scoring stage, not merely a successful render.

## Wideband noise campaign

The [wideband method](design/wideband-validation.md) owns detector settings,
positive controls and statistical interpretation:

```powershell
python signal-lab/validation/run_wideband_validation.py `
  --build build/host-release --work build/wideband-check
```

The default streams three five-hour renders at 6, 9 and 12 kHz noise bandwidth,
using about 7.8 GB for the WAVs. `--duration-seconds 32` is a smoke check whose
detector may be inconclusive. Resume only with the runner's identity checks;
do not mix artifacts from different configurations. The runner records the
renderer binary hash in its run identity.
