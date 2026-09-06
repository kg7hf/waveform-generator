# Phase 4: live impairment control and replay

The RT1170 player accepts sample-indexed impairment changes while it plays a
retained WAV. The pipeline is:

```text
SD WAV -> scenario engine -> live controls -> output FIFO -> SAI/eDMA -> WM8960
```

The live controller knows only 48 kHz mono PCM16 samples. M110 encoding is a
separate file-generation utility described in [Phase 5](phase5-artifact-generator.md).
PC-created WAVs and board-created WAVs enter the same playback path.

## Engineering protocol

`WFG-LIVE/1` is a distinct engineering protocol. The frozen `WFG/1` qualification
contract in `phase1-implementation.md` remains a separate contract; its immutable
ARM configuration, complete SHA validation, finite DMA completion proof and
qualification counters are not claimed by this implementation.

A request is `<sequence> <command> LF`, with optional CR immediately before LF.
The sequence is a canonical positive uint32 decimal that strictly increases
within the connection. Even a rejected command consumes its valid sequence.
Requests have a maximum of 192 wire bytes including LF/CRLF. An oversized record
is discarded through LF. Invalid requests execute no playback command.

Responses are JSON objects with `seq`, `protocol`, `ok`, numeric `state`,
`run_id`, and exactly one of `info`, `status`, `counters`, `result`, or `error`.
The response buffer is 4096 bytes, including LF. Only one request is outstanding;
partial writes are retained, and endpoint backpressure bounds queued input.
There is no automatic retry after an uncertain mutation.

The legacy `STATUS`, `MEDIA HOST`, `MEDIA LOCAL`, and `PLAY` lines remain available
for checkpoint tools. DD-008 binary upload uses a separate connection mode;
close and reopen the CDC port to switch between DD-008 and WFG-LIVE/1.

| Command | Behavior |
| --- | --- |
| `INFO?` | Board/USB serial, source manifest, feature list and encoder IDs. |
| `STATUS?`, `COUNTERS?` | Engineering player, FIFO, live output and file-generation snapshots. |
| `MEDIA HOST`, `MEDIA LOCAL` | Existing exclusive MSC/FatFs ownership handshake. Host ownership is refused during playback, upload or generation. |
| `LOAD:TEST.WAV` | Select `/WG/TEST.WAV` and optional `/WG/TEST.SCN`; clear the live schedule and explicit reference override. Allowed after playback has stopped. |
| `SEED:42` | Set the independent live-controller seed before queuing controls. |
| `REFERENCE:0.25` | Override the raw source RMS before queuing controls; must be in `(0,1]`. |
| `PLAY` or `PLAY:<32 lowercase hex digits>` | Start the selected file once, including open/validation/prefill. Acceptance is asynchronous; poll status for setup failure. |
| `STOP` | Request stopped playback and cleanup, or abort an active file-generation job. Poll for completion of cleanup; retained files are not deleted. |
| `GENERATE:600:long:PAYLOAD.BIN:TEST.WAV` | Create an M110 WAV from a payload already on the SD card. Poll file-generation status before LOAD. |
| `ENCODE M110B 600:long PAYLOAD.BIN TEST.WAV` | Generic encoder utility: encoder ID and opaque profile are passed to its adapter. |

Source/output names use uppercase 8.3 names, with letters, digits or underscore
in the stem. Paths and traversal are rejected. The currently registered encoder
is M110B. The encoder catalog is the extension point for future implementations;
M110D/JT8 support is not claimed merely because the interface permits adapters.

Playback states retain the checkpoint numeric mapping: 0 stopped, 1 waiting for
command, 2 mounting, 3 opening, 4 buffering, 5 playing, 6 draining, 7 done,
8 aborted and 255 fault. A completed/aborted/faulted run requires LOAD before
another PLAY. Files and audio are closed before reuse or host ownership is allowed.
Run IDs identify attempts in RAM; the auto-generated ID is only a boot-local
counter. A new host-generated UUID is recommended for each run. Neither IDs nor
the engineering counters establish hardware-qualified completion.

## Live commands

| Command | Meaning |
| --- | --- |
| `CW ON`, `CW OFF` | Enable/disable the live carrier. |
| `CW FREQ 1800`, `CW CI 3` | Carrier frequency in Hz and source-to-carrier RMS ratio in dB. |
| `STATIC ON`, `STATIC OFF` | Enable/disable seeded static crashes. |
| `STATIC RATE 1`, `STATIC PEAK 20` | Crash arrivals per second and peak dB relative to source RMS. |
| `FADE NOW 18 250` | One raised-cosine fade, depth 18 dB, total duration 250 ms. |
| `SWEEP CW FREQ 300 3400 16 500` | Sixteen equally spaced frequency values, dwelling 500 ms at each step. CW must be enabled separately. |
| `SWEEP CW CI 20 -3 16 1000` | Sixteen equally spaced C/I values at one-second intervals. |
| `SWEEP FADE 6 30 5 1000 250` | Five fades with depths 6, 12, 18, 24 and 30 dB, one second apart, each lasting 250 ms. |

Prefix a live command with `AT:<output_frame> ` to specify its exact application
position. Otherwise, the player task queues it at the next unproduced output
frame and acknowledges that value as `result.apply_frame`. The acknowledgement
also reports the number of events accepted. A sweep is accepted atomically.
For a nonuniform C/I sequence such as +20,+10,+6,+3,0,-3, schedule individual
`AT:` commands at the desired positions.

Positions refer to the PCM stream **after** scenario sample slips and before
the output FIFO. The FIFO may already contain roughly 0.5–0.8 seconds of earlier
audio, so command acknowledgement is not an analog timestamp. Late commands,
exhausted queues, or a finished producer are rejected. No file I/O, formatting
or control processing occurs in the audio callback.

The live signal order is source multiplied by live fade, then live CW and static
added, followed by PCM16 saturation. The fixed scenario is processed first.
The independent live seed does not alter scenario RNG streams. Without live
controls, PCM samples pass through unchanged and are still hashed.

The effective live reference is the explicit raw REFERENCE times scenario
source gain, or the scenario's scaled `reference_rms`. With neither, firmware
measures the first `min(N,48000)` clean source frames and rewinds before playback.
A zero reference permits clean playback but rejects live impairments; provide
REFERENCE when the WAV starts with silence. `live_reference_rms` records the
effective scaled value. The seed and reference must be retained for replay.

The bounded controller retains 128 accepted events and has 64 pending slots.
A sweep has 2–16 values. CW phase runs while disabled and survives updates.
Static uses 12 concurrent 1700 Hz tone-ring crashes with 20 ms exponential decay;
OFF ends active tails. A new live fade replaces the previous fade. Rejected
controls never silently evict earlier events.

## Host operation and retained evidence

The physical serial tools need the explicitly installed dependency in
`host/requirements-control.txt`. They require a selected port and expected USB
serial, then verify fixture identity before mutation. Tests do not open hardware.
Use the existing media handoff procedure before allowing FatFs access.

```powershell
python host/live_control.py --port <actual-port> --expected-serial <actual-serial> --record artifacts/live.jsonl interactive
```

An example interactive session is:

```text
MEDIA LOCAL
LOAD:TEST.WAV
SEED:42
CW FREQ 1800
CW CI 3
CW ON
PLAY:0123456789abcdef0123456789abcdef
STATUS?
SWEEP CW FREQ 300 3400 16 500
STATUS?
STOP
STATUS?
```

Retain a final STATUS after cleanup; the final `live_frame`, effective reference,
seed, control acknowledgements, source WAV/SCN and identities are needed to
reproduce the generated PCM. Retrieve the retained source artifacts through MSC
only after local file ownership is released. A filename alone is not source identity.

```powershell
python host/live_control.py export artifacts/live.jsonl artifacts/replay.json
build/host-release/host/render/signal_lab_render.exe --source TEST.WAV --live-events artifacts/replay.json --output artifacts/replayed.wav --sidecar artifacts/replayed.json
```

Add `--scenario TEST.SCN` when the board used that scenario. The host renderer
replays the same acknowledged frame positions, including sample slips before
the live stage. Its event reader refills the bounded pending queue; it does not
require all 128 captured events to fit simultaneously. `live_frames` can retain
a stopped producer prefix. These are generated frames, not proof of samples
that reached the analog connector before STOP.

## Validation boundary

Portable tests check control levels, phases, sweeps, random-stream isolation,
queue errors and PCM identity across block sizes. Renderer tests cover exported
plans, sample slips, exact uint64 values and invalid artifact rejection. A host
harness compiles the actual player/CDC owner code against deterministic I/O
stubs to exercise lifecycle, backpressure and disconnect handling.

The RT1170 image cross-builds with static storage for the source encoder, writer,
controller and FIFO. Firmware timing, stack headroom on the board, live USB under
audio load, on-card write behavior and host/target live digest parity require a
new hardware run. The earlier Phase 3 hardware evidence does not validate these
new Phase 4/5 paths.
