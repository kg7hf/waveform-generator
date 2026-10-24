# Technical Module Documentation

This directory explains the current architecture: what each subsystem does,
how to use it, and what must remain true when changing it.

## Documentation standard

Major subsystem guides contain four named views. Small feature guides may
link to their parent subsystem for shared context rather than repeat it:

| View | Audience | Required content |
|---|---|---|
| 50,000-foot view | Project leads and new engineers | Inputs, outputs, purpose, and boundaries |
| How to use it | Operators and integrators | API or command sequence, prerequisites, and concrete usage |
| Scientist and maintainer view | DSP engineers and code owners | Equations, data structures, bounded resources, invariants, failure modes, and diagnostics |
| 5th-grader view | Anyone building intuition | Plain-language analogy without changing the technical claim |

Each guide also has a toy worked example. Toy values are illustrative and are
not conformance vectors unless a guide explicitly identifies a retained vector
and its hash.

## Module index

| Guide | Owns the explanation of | Read next when |
|---|---|---|
| [Signal Lab Engine](signal-lab-engine.md) | Static scenarios, ordered stages, reference levels, PCM conversion, clipping, and block independence | Adding or changing an impairment |
| [Finite Prepared Streams](finite-playback.md) | Prepared lead, committed storage, bounded playback queues and failure ownership | Building the finite overlap scheduler |
| [Live Control and Replay](live-control-and-replay.md) | Sample-indexed control events, CW slots, sweeps, journals, and offline replay | Adding dynamic interference or debugging a live session |
| [Waveform Source and Encoders](waveform-source-and-encoders.md) | Generic byte/source/sink interfaces and WAV artifact generation | Registering a new waveform family |
| [Custom encoder tutorial](../custom-encoder-tutorial.md) | Module creation, opaque profiles and CDC command extensions | Adding a user-supplied generator |
| [RT1170 Player and Audio](rt1170-player-and-audio.md) | SD producer, impairment engine, OCRAM FIFO, codec task, the USB route (sound-card stream, per-play statistics, miss attribution), and target states | Changing target timing, memory, or audio behavior |
| [Protocols, Media, and Artifacts](protocols-media-and-artifacts.md) | DD008, CDC framing, the composite USB device and its UAC2 sound-card function, MSC/FatFs ownership, and generated SD artifacts | Changing a wire command, a USB descriptor or a media operation |
| [PC WAV Playback](pc-wav-playback.md) | Explicit Windows WAV output, attenuation and device selection | Listening to retained clean or mixed files |
| [Host Capture and Analysis](host-capture-and-analysis.md) | WinMM/raw WASAPI capture (analog inputs and the board's USB sound card), bounded callback queue, WAV finalization, and analysis | Running or changing evidence collection |
| [Stereo Artifacts](stereo-artifacts.md) | Checked composition of independent mono renders and exact stereo file verification | Preparing independent left/right channel files |
| [pathsim Channel Simulator](pathsim.md) | The PathSim submodule (rev-6 patch, plus the chain's draw-order patch), the `channel-sim` chain driver (RNG shim, 8 kHz driver, the lane's 48 kHz resampler chain, `pathsim_chain`) and gates 0 to 3 against the lane's binaries and outputs | Changing the PathSim revision or patches, the chain driver or the channel-sim build |
| [Build and Dependencies](build-and-dependencies.md) | CMake targets, presets, submodules, patches and test cleanup | Adding sources, targets, or dependencies |
| [Command-line Tools](command-line-tools.md) | Operator-facing command options; complete workflows live in the runbooks | Running a tool without reading its implementation |


### Board and DSP references

| Guide | Owns |
|---|---|
| [RT1064 board](rt1064-board.md) | RT1064 startup, pins, clocks, memory and port-specific limits |
| [AWGN](awgn.md) | Noise density, scaling and Gaussian/FIR implementation |
| [Watterson channel](signal-lab-watterson.md) | Multipath API, schema, bounded state and implementation walkthrough |
| [Frequency shift](frequency-shift.md) | Global offset and Doppler sweep |
| [Wideband noise](wideband-noise.md) | Selectable noise bandwidth and static-path fixtures |
| [Pulsed CW](pulsed-cw.md) | Periodic gating and ramps |
| [Swept CW](swept-cw.md) | Triangular carrier sweep |
| [Markov CW](markov-cw.md) | Random ON/OFF dwell model |
| [FSK CW](fsk-cw.md) | Symbol timing and mark/space convention |
| [Faded CW](faded-cw.md) | Rayleigh gain applied to the interferer |
| [Second source](second-source.md) | WAV overlay, independent fading and gating |
| [Random stream identity](random-stream-identity.md) | Stable stage seeds and random-family independence |
| [Time-step faults](time-step-faults.md) | Sample slips, transport CUTs and unintended gaps |

## Source ownership map

| Source area | Primary guide |
|---|---|
| `signal-lab/include`, `signal-lab/src`, `host/signal_lab` | Signal Lab Engine |
| `common/live_protocol.*`, `common/live_command.hpp`, `signal-lab/*live_control*`, `host/live_control.py`, `host/render/live_replay.*` | Live Control and Replay |
| `waveform-source/` | Waveform Source and Encoders |
| `rt1064/`, `cmake/rt1064_config.cmake` | RT1064 board |
| `rt1170/player.*`, `rt1170/platform/`, `rt1170/os/`, `rt1170/main.cpp` | RT1170 Player and Audio |
| `common/dd008/`, `common/tx_protocol.*`, `common/wav.hpp`, `rt1170/usb*`, `rt1170/tx_*` | Protocols, Media, and Artifacts |
| `host/capture/`, `host/capture-session.ps1`, `host/analyze_capture.py` | Host Capture and Analysis |
| `host/render_scenario.py`, `host/signal_lab/` | Signal Lab Engine |
| `CMakeLists.txt`, `CMakePresets.json`, `cmake/`, `scripts/`, `schema/`, dependency paths | Build and Dependencies |

Cross-cutting files can appear in more than one guide, but one guide owns each
contract. For example, Live Control owns event semantics while Protocols owns
CDC framing and transport failure behavior. The [protocol reference](../live-control-protocol.md)
owns wire grammar and response schemas; [testing](../testing.md) owns acceptance
procedures. See the [documentation map](../README.md#documentation-ownership).

## Reading order

1. Read the root [`README.md`](../../README.md) for the system picture and quick starts.
2. Read Signal Lab Engine for the static sample pipeline.
3. Read Live Control and Replay for dynamic overlays.
4. Read Waveform Source and Encoders, then [Encoder plugins](../encoder-plugins.md), for source creation.
5. Read the shared RT1170 Player and Audio guide, the RT1064 port when applicable,
   and Protocols, Media, and Artifacts for firmware implementation. Use the
   [Windows bench](../windows-usb-bench.md) for hardware operation.
6. Read Host Capture and Analysis for physical evidence.
7. Read Build and Dependencies before changing dependencies or target structure.

## Update rule

When behavior changes, update the owning module guide in the same change. Keep claims bounded: host equivalence, target build success,
CDC operation, analog capture, decoder performance, and formal conformance are
different evidence levels.

## Practical demos and coding policy

Start with [How to use](../how-to-use.md) for the generated/downloaded WAV demos.
The [USB sound card bring-up](../usb-sound-card-bringup.md) runbook takes a player
image from flashing to a verified bit-exact capture.
Follow [the coding standard](../coding-standard.md) for source contracts and formatting.
