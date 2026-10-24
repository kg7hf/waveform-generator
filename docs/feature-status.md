# Feature status

This is the reader-facing inventory of what the current source provides. It
separates **implementation** from **acceptance**: a feature can be callable and
covered by host tests while still needing measurements on a particular board,
PC, analog connector or workload. See [Testing](testing.md) for those evidence
levels and [Roadmap](roadmap.md) for the order of remaining work. The default
build needs no waveform encoder plugin.

## Fully implemented interfaces

“Fully implemented” means the interface and described behavior exist in the
default build. It does not mean every hardware configuration has been qualified.

| Feature | Available behavior | Start here |
|---|---|---|
| Supplied WAV input and calibration tone | Validate mono 48 kHz packed PCM24; generate a repeatable ten-second tone | [How to use](how-to-use.md#demo-1-generate-a-waveform-add-cw-and-a-mixed-channel) |
| Offline C++ effects render | Apply ordered scenarios and emit a PCM24 WAV plus report; CW, AWGN, impulse, fade, sample slip, Watterson, frequency shift and second source are parsed and processed | [Signal Lab](modules/signal-lab-engine.md) and [renderer switches](modules/command-line-tools.md#signal_lab_renderexe) |
| PC playback of a completed WAV | Python/PortAudio streams a selected WAV to one exact Windows WASAPI output; mono is copied to left and right | [PC playback](modules/pc-wav-playback.md) |
| RT1170/RT1064 USB WAV player | SD-backed PCM24 file playback through a USB Audio Class 2 capture endpoint, with CDC control and mass-storage card handoff | [Windows USB bench](windows-usb-bench.md) |
| Live board controls | Four CW slots, seeded static crashes, immediate fades, stepped sweeps, sample-indexed scheduling, journaling and offline replay | [WFG-LIVE/1](live-control-protocol.md#live-commands) |
| Fixed fault injection and test batteries | Scenario sample slips; post-engine CUT drop/zero schedules; on-card PLAYLIST items with gaps, routes and result blocks | [Time-step faults](modules/time-step-faults.md) and [PLAYLIST](live-control-protocol.md#playlist) |
| Host capture and analysis tools | WinMM or WASAPI-raw input capture, reports, expected-stream comparison and per-item playlist capture splitting | [Capture](modules/host-capture-and-analysis.md) and [CLI reference](modules/command-line-tools.md) |
| Optional encoder interface | Build-time static plugin registry, generic `ENCODE` command and public `TONE` example; disabled in the normal build | [Encoder plugins](encoder-plugins.md) |
| Source and binary licensing | GPL-3.0-only project code, Paul R. Decker SPDX notices, retained third-party notices and submodule licenses | [Copyright](../COPYRIGHT) and [third-party notices](../THIRD_PARTY_NOTICES.md) |

The Python reference renderer supports fewer effects than the C++ renderer;
it is a separate implementation, not the board-bit-exact oracle. For exact
scope and JSON fields, use the linked subsystem guides. “Fully implemented”
in this table means a usable software interface, not a claim of zero dropped
samples, analog fidelity, modem conformance or real-time margin on every setup.

## Partially implemented or experimental

| Area | What exists now | What is still open |
|---|---|---|
| RT1064 WM8960 analog output | Codec route and firmware build are present | Independent analog bring-up and measurement; USB results do not qualify the connector |
| Physical validation across PCs | Short deployed-firmware USB WAV plus scheduled CW captures have passed on both boards | Sustained host-load, reconnect, multi-PC and broader-effects campaigns; report each fixture and endpoint separately |
| Full on-target effects capacity | C++ engine, board builds, memory/stack gates and host tests exist | Measure deadline, SD and transport margins under combined long-running workloads on each target |
| Board-side `PREPARE` | Writable images can render and verify a new impaired card WAV before a separate play | Broader physical media/failure campaigns and automatic discovery/reuse after restart |
| `PREPAREPLAY` | Experimental finite render/play coordinator, command and host simulation exist | Measured rate/lead on the board and combined storage/playback acceptance; do not treat host simulation as throughput proof |
| Public modem-core encoder adapter | Optional adapter uses the separately supplied public transmitter; host parity tests and board cross-builds exist | Playback and timing validation of an adapter-enabled board image |
| Hosted public CI | Workflow selects GitHub-hosted Ubuntu for device-free checks on every push, PR and dispatch | No hosted run has passed yet; the last private-repository attempts were refused by the Actions budget |
| Unified test selection and result labels | Local CTest selection, four device-free CI jobs and separate board/campaign scripts exist | Full timing/platform/channel/claim labels, named broader suites, distributed selection and public golden-data packs are planned; see [testing](testing.md#test-layers-and-current-scope) |

These rows are usable only within their stated limits. In particular, a successful
`PREPARE` admission is not a completed artifact, and a `PLAY` acknowledgment
is not a completed or continuous capture. Check terminal status and counters.

## Future or on hold

| Idea | Current position |
|---|---|
| Native Windows C++ live audio output and graphical effects studio | Planned; today render a WAV first, then use the Python playback helper |
| Windows virtual sound-card driver | On hold; the RT1170/RT1064 USB audio devices are the multi-PC delivery method |
| Runtime-loaded plugins or arbitrary plugin-defined CDC verbs | Not provided; plugins are linked at build time and use the bounded `ENCODE` profile. Extending CDC requires coordinated host and firmware parser work |
| Automatic board-side prepared-artifact archive/reuse selection | Not provided; retrieve and verify artifacts with the explicit host tools |
| Sample-clock drift/ppm fault model and general radio filter stage | Not provided; sample slips, CUTs, frequency shift and current channel stages have distinct implemented meanings |
| RT1064 eMMC storage support | Not provided; use the removable SD card |

Use the [complete how-to](how-to-use.md) for end-to-end examples, the
[CLI reference](modules/command-line-tools.md) for switches, and the
[protocol reference](live-control-protocol.md) for CDC commands and replies.
