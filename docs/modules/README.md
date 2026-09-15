# Technical Module Documentation

This directory explains the current architecture: what each subsystem does,
how to use it, and what must remain true when changing it.

## Documentation standard

Every technical module guide contains four named views:

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
| [Live Control and Replay](live-control-and-replay.md) | Sample-indexed control events, CW slots, sweeps, journals, and offline replay | Adding dynamic interference or debugging a live session |
| [Waveform Source and Encoders](waveform-source-and-encoders.md) | Generic byte/source/sink interfaces and WAV artifact generation | Registering a new waveform family |
| [Custom encoder tutorial](../custom-encoder-tutorial.md) | Module creation, opaque profiles and CDC command extensions | Adding a user-supplied generator |
| [RT1170 Player and Audio](rt1170-player-and-audio.md) | SD producer, impairment engine, OCRAM FIFO, codec task, and target states | Changing target timing, memory, or audio behavior |
| [Protocols, Media, and Artifacts](protocols-media-and-artifacts.md) | WFG-LIVE/1, DD008, CDC framing, MSC/FatFs ownership, and generated SD artifacts | Changing a wire command or media operation |
| [PC WAV Playback](pc-wav-playback.md) | Explicit Windows WAV output, attenuation and device selection | Listening to retained clean or mixed files |
| [Host Capture and Analysis](host-capture-and-analysis.md) | WinMM/raw WASAPI capture, bounded callback queue, WAV finalization, and analysis | Running or changing analog evidence collection |
| [Corpus Rendering and Scoring](corpus-rendering-and-scoring.md) | Reference generation, scenario expansion, scoring, summaries, and deterministic verification | Building an interference campaign |
| [Build and Dependencies](build-and-dependencies.md) | CMake targets, presets, submodules, patches and test cleanup | Adding sources, targets, or dependencies |
| [Command-line Tools](command-line-tools.md) | Operator-facing command options and copy-ready workflows | Running a tool without reading its implementation |

## Source ownership map

| Source area | Primary guide |
|---|---|
| `signal-lab/include`, `signal-lab/src`, `host/signal_lab` | Signal Lab Engine |
| `common/live_protocol.*`, `common/live_command.hpp`, `signal-lab/*live_control*`, `host/live_control.py`, `host/render/live_replay.*` | Live Control and Replay |
| `waveform-source/` | Waveform Source and Encoders |
| `rt1170/player.*`, `rt1170/platform/`, `rt1170/os/`, `rt1170/main.cpp` | RT1170 Player and Audio |
| `common/dd008/`, `common/tx_protocol.*`, `common/wav.hpp`, `rt1170/usb*`, `rt1170/tx_*` | Protocols, Media, and Artifacts |
| `host/capture/`, `host/capture-session.ps1`, `host/analyze_capture.py` | Host Capture and Analysis |
| `host/corpus.py`, `host/render_scenario.py`, `host/m110_score.py`, `corpus/`, `test-vectors/` | Corpus Rendering and Scoring |
| `CMakeLists.txt`, `CMakePresets.json`, `cmake/`, `scripts/`, `schema/`, dependency paths | Build and Dependencies |

Cross-cutting files can appear in more than one guide, but one guide owns each
contract. For example, Live Control owns event semantics while Protocols owns
CDC framing and uncertain-response behavior.

## Reading order

1. Read the root [`README.md`](../../README.md) for the system picture and quick starts.
2. Read Signal Lab Engine for the static sample pipeline.
3. Read Live Control and Replay for dynamic overlays.
4. Read Waveform Source and Encoders, then [Encoder plugins](../encoder-plugins.md), for source creation.
5. Read RT1170 Player and Audio, then Protocols, Media, and Artifacts, for hardware operation.
6. Read Host Capture and Analysis for physical evidence.
7. Read Corpus Rendering and Scoring for campaigns.
8. Read Build and Dependencies before changing dependencies or target structure.

## Update rule

When behavior changes, update the owning module guide in the same change. Keep claims bounded: host equivalence, target build success,
CDC operation, analog capture, decoder performance, and formal conformance are
different evidence levels.

## Practical demos and coding policy

Start with [How to use](../how-to-use.md) for the generated/downloaded WAV demos.
Follow [the coding standard](../coding-standard.md) for source contracts and formatting.
