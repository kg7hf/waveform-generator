# Live Control and Deterministic Replay

## 1. Scope

This guide covers `WFG-LIVE/1`, the portable `LiveController`, the RT1170 player
mailbox adapter, host session journals, replay-plan export, and offline replay.
It explains dynamic changes made after the static scenario engine.

## 2. 50,000-foot view

Live control lets an operator change interference while audio is running without
making timing depend on USB latency. The board acknowledges an exact output-frame
position. That frame, command order, seed, and effective reference RMS are enough
to reconstruct the same overlay offline.

```text
static-engine PCM -> live fade -> four CW oscillators -> live crashes -> saturate
```

`WFG-LIVE/1` carries bounded commands, responses and frame-indexed live controls.

## 3. How to use it

Always select the current serial endpoint and verify the board-reported serial:

```powershell
python host/live_control.py `
  --port COM42 `
  --expected-serial REPLACE_WITH_OBSERVED_SERIAL `
  --record run.jsonl `
  command "INFO?" "STATUS?"
```

Configure two CW sources before playback:

```powershell
python host/live_control.py --port COM42 --expected-serial SERIAL `
  --record play.jsonl command `
  "CW 0 FREQ 900" "CW 0 CI 12" "CW 0 ON" `
  "CW 1 FREQ 1300" "CW 1 CI 12" "CW 1 ON" "PLAY"
```

Legacy `CW ON`, `CW OFF`, `CW FREQ`, and `CW CI` commands address slot 0.
Indexed commands address slots 0 through 3. Sweeps change frequency or C/I in
bounded steps but do not implicitly enable a slot.

Export and replay an acknowledged session:

```powershell
python host/live_control.py export play.jsonl play.replay.json
python host/live_control.py --port COM42 --expected-serial SERIAL `
  --record replay.jsonl replay play.replay.json
```

Never overwrite an old journal. A timeout or malformed response is uncertain;
retain the journal, reconnect, and inspect `INFO?` and `STATUS?`. Do not resend
the uncertain mutation automatically.

## 4. Scientist and maintainer view

### Coordinate system

Events use absolute final-output frames. At 48 kHz, frame 48,000 is one second
and frame 96,000 is two seconds. Upstream sample duplication or deletion does
not move an already acknowledged live event because live control follows the
static engine.

Equal-frame events execute in enqueue order. The capture is immutable until
reset; no old record is silently discarded to make room.

### Bounded state

| Resource | Capacity |
|---|---|
| Pending controls | 64 |
| Captured controls | 128 |
| Expanded sweep values | 16 |
| Concurrent static envelopes | 12 |
| Independent CW oscillators | 4 |
| Wire request | 192 bytes |

Each oscillator has independent enable, phase, phase step, frequency, and C/I.
Oscillator phases free-run while disabled so a parameter change does not imply a
phase reset. All enabled slots are summed before the single final quantizer.

Live static uses seeded exponential/Poisson arrivals and a bounded active-crash
pool. `STATIC RATE` is an average event rate, not a promise that crashes occur
inside a particular interval. `FADE NOW` is a scalar time envelope, not a
frequency-selective multipath channel.

### Replay identity

A useful replay record binds:

- Protocol and firmware identity.
- Source and scenario artifacts retained by the caller.
- Effective positive `live_reference_rms`.
- Live seed.
- Accepted commands and acknowledged apply frames.
- Final live frame count and digest when available.

USB arrival time, acknowledgment time, DAC time, and analog audibility time are
different clocks. The protocol claims only the output-frame schedule.

### Failure and debugging signals

Inspect `live_frame`, digest, clipping, accepted/applied controls, queue free,
capture free, static starts/drops, and the terminal run state. A `BUSY`, late
event, full queue, full capture, invalid slot, invalid value, or untrustworthy
JSON response is a failed control operation, not an invitation to guess state.

## 5. 5th-grader view

Imagine a DJ playing a recording. You ask for whistle number 1 to start at the
one-second mark. The DJ writes "sample 48,000" in a notebook instead of writing
"when the USB message arrived." Later, another DJ can use the notebook and start
the same whistle at the same sample.

There are four whistle machines. Each has its own pitch and loudness knob. They
all feed one speaker, so turning on several can make the combined sound too loud.

## 6. Toy worked example

Suppose the current output frame is 40,000 and the queue receives:

```text
frame 48,000: CW slot 1 frequency = 1300 Hz
frame 48,000: CW slot 1 C/I = 12 dB
frame 48,000: CW slot 1 enabled = true
frame 96,000: CW slot 1 enabled = false
```

The first three controls share a frame and execute in that order. The first
sample containing slot 1 is frame 48,000. The slot runs for exactly 48,000 output
frames, or one second. If a static scenario duplicated 100 source samples before
that point, the live start still occurs at output frame 48,000.

## 7. Code map and change checklist

| Responsibility | Files |
|---|---|
| Text grammar | `common/live_protocol.hpp`, `common/live_protocol.cpp` |
| Player-mailbox translation | `common/live_command.hpp` |
| Portable event engine | `signal-lab/include/signal_lab/live_control.hpp`, `signal-lab/src/live_control.cpp` |
| Host validation and journals | `host/live_control.py` |
| Offline replay reader | `host/render/live_replay.hpp`, `host/render/live_replay.cpp` |
| RT1170 response/state bridge | `rt1170/player.cpp`, `rt1170/usb_service.cpp` |

When adding a control, update grammar, host validation, target translation,
portable state, replay representation, capacity calculations, response docs, and
tests. Preserve legacy forms unless the protocol version changes.

## 8. Control walkthrough and invariants

`parse_live_command` validates text into a typed command. The target mailbox
transfers it to the player owner; the producer accepts or rejects it against
current load/run state. The portable controller applies accepted controls in
stable order at their output-frame boundary. Its float processing entry rejects
non-finite input before advancing the timeline. Acknowledgments retain the
resolved frame and effective reference used by the controller.

`host/live_control.py` records requests and responses. Export accepts usable
acknowledged controls, and `host/render/live_replay.cpp` validates the replay
record before processing. A timeout is not an acknowledged event. Never invent
an acknowledgment or replay a mutation merely because the host timestamp exists.

Queue and event-history capacities are independent limits. Keep refusal explicit
when either fills; do not evict old controls silently. Test same-frame ordering,
late commands, full queues, LOAD resets and block boundaries using
`wfg-live-control`, `wfg-live-protocol`, `wfg-live-host` and `wfg-player-live-owner`.
