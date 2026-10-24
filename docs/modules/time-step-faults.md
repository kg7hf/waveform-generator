# Time-step faults: sample slips, CUT and transport gaps

This note separates three events that can look like a discontinuity in a
capture. A scenario `sample_slip` changes the engine's sample sequence. The
player's `CUT` schedule edits the engine's output after live controls. A USB
missed interval records a transport-service gap. They occur at different
boundaries and have different frame coordinates and counters.

## Where each event acts

```text
WAV/card frames
    -> Signal Lab scenario (Watterson first when present; sample_slip last)
    -> live controls
    -> CUT (DROP / ZERO)
    -> output FIFO -> USB capture or codec
```

The scenario pipeline starts with its source (the card on the RT1170). Fade
ordering among scenario stages is observable: fade before AWGN/CW attenuates
the desired signal before those later components are added. A `sample_slip`
must be the final scenario stage because it changes stream length and shifts
the relationship between later input-frame windows and output samples. It is
a discrete-time model; `sample_clock` / ppm clock drift is still deferred.

The player runs `LiveController::process()` on the engine output, then offers
that block to the CUT stage. Consequently CUT also acts after the scenario's
Watterson, fade, noise, CW, impulse and sample-slip stages and after live
controls. CUT does not add a Signal Lab stage. Both live `CUT` commands and a
playlist item's `cut=` option use that same player stage and frame convention.
See the [live protocol's CUT injector](../live-control-protocol.md#cut-injector)
for the command grammar and bounds and the [RT1170 player guide](rt1170-player-and-audio.md#cut-injector)
for the player implementation and counters.

## The two content faults

`sample_slip` is a Signal Lab impairment with `delete` or `duplicate` events.
Its event times are expressed in seconds on the scenario input timeline and
converted to 48 kHz input-frame positions. A delete consumes input samples
without emitting them; a duplicate emits retained delivered-output history.
Therefore deletes shorten the engine output and duplicates lengthen it. Slip
events and limits are described in the [Signal Lab guide](signal-lab-engine.md#time-step-faults).

`CUT` uses `CUT:<offset>:<N>:<drop|zero>,...` for a live play. Its `offset`
counts frames in the engine output, before CUT, from that play's first output
frame. Thus, when the scenario contains slips, CUT positions refer to the
already slipped output stream, not directly to card/WAV positions. A clean
play's engine output is the card, so its offsets are card-frame positions.
Playlist `cut=` carries the same entries without the `CUT:` prefix and applies
them to that item. On a duration-looped playlist item, offsets count the whole
continuous engine-output stream from the item start; they do not restart at a
card-loop seam.

- `DROP` discards N engine-output frames at the offset. Those frames have
  already passed through the engine and live controls, so their state and
  timelines continue; only the stream sent downstream becomes N frames
  shorter. A DROP that reaches end-of-output is clipped there.
- `ZERO` inserts N exact zero frames before the engine-output frame at the
  offset. The zeros bypass the engine and live controls; after them, output
  resumes at the same next engine frame. The sent stream becomes N frames
  longer.

Entries are limited to 16, each length is 1 through 4800 frames, and entries
must be sorted, non-overlapping, and within the 14,400-frame maximum DROP
balance. The schedule latches to the next accepted `PLAY`; playlist items
carry their own optional schedule. See the protocol reference for all
validation and latching rules.

## Combined prediction

Render the scenario first, including its final sample-slip stage, then apply
CUT to that rendered output. For example, the scenario can contain a fade and
a delete event while the player applies a later output DROP:

```json
{
  "schema": "signal-lab.scenario/1",
  "test_id": "SLIP-AND-CUT",
  "seed": 7,
  "source": {"path": "clean.wav"},
  "reference": "auto",
  "source_gain_db": 0,
  "clip_policy": "saturate",
  "impairments": [
    {"type": "fade", "depth_db": 3, "duration_ms": 250,
     "starts_seconds": [0.5]},
    {"type": "sample_slip", "events": [
      {"at_seconds": 1, "kind": "delete", "length_samples": 48}
    ]}
  ]
}
```

For a live play, set `CUT:96000:480:drop` before the accepted `PLAY`. That
offset is on the final engine-output axis (after the delete), not a claim that
it maps to card frame 96000. To make a host prediction from the same scenario
and source, use `host/cut_schedule.py predict` with `--scenario`, `--render`,
and the same `--cut` value. The predictor renders the scenario and then
applies CUT, matching the firmware order. A playlist can combine `scn=` and
`cut=` on one item; `host/make_playlist.py` uses that same order when its
prediction inputs are available.

```powershell
python host/cut_schedule.py predict --card clean.wav --scenario case.scn `
  --render build\host-release\host\render\signal_lab_render.exe `
  --cut CUT:96000:480:drop --output-wav expected.wav
```

The engine digest describes the engine's whole output before CUT, including
slip effects. USB CRCs describe the frames actually sent, including ZERO
frames and excluding DROP frames. For a play stopped part-way, CUT reporting
and `cut_schedule.py predict --sent-frames` describe the confirmed sent
prefix. On codec output, USB play counters are not populated; the player guide
documents the codec-side counters.

## Content cuts are not USB stalls

CUT is a deterministic content edit. It does not pause the USB service clock
or create a missed USB interval: a DROP shortens the stream and ZERO lengthens
it while the route continues sending at its normal cadence. CUT accounting is
reported by `uac_play_cut_*` and `uac_play_sent_frames` on USB plays.

An actual pause or delayed service is measured separately by
`uac_play_missed_intervals` for intervals that delayed a play and
`uac_missed_intervals` for the stream diagnostic. A resulting capture gap is
not equivalent to a scheduled CUT, and a content prediction cannot establish
that USB or codec timing recovered correctly. Host player tests can inject
service-time gaps for software behavior; physical pacing and analog output
still require separate target and bench evidence.
