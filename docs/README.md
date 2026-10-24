# Documentation map

Start with the [project overview](../README.md), then follow one route:

| Goal | Guide |
|---|---|
| Make the first WAV with interference | [Quick start](quickstart.md) |
| Use the generated and downloaded WAV demos | [How to use](how-to-use.md) |
| Operate either board on Windows | [Windows USB bench](windows-usb-bench.md) |
| Debug RT1170 USB bring-up | [USB sound card bring-up](usb-sound-card-bringup.md) |
| Add a waveform producer | [Custom encoder tutorial](custom-encoder-tutorial.md) |
| Look up plugin contracts | [Encoder plugins](encoder-plugins.md) |
| Look up CDC commands and responses | [Live control protocol](live-control-protocol.md) |
| Generate clean encoder artifacts | [Artifact generation](artifact-generation.md) |
| Verify or reuse board-prepared artifacts | [Prepared artifacts](prepared-artifacts.md) |
| Understand or change implementation | [Module index](modules/README.md) |
| Run software or hardware checks | [Testing](testing.md) |
| Follow coding rules | [Coding standard](coding-standard.md) |
| Prepare a contribution | [Contributing](development/contributing.md) |
| Understand public CI checks | [GitHub-hosted CI](development/hosted-ci.md) |
| See remaining work | [Roadmap](roadmap.md) |

## Design references

- [Retained waveform lifecycle](design/retained-waveforms.md)
- [Watterson channel design](design/watterson-stage.md)
- [Watterson validation method](design/watterson-validation.md)
- [Wideband validation method](design/wideband-validation.md)
- [Frequency-truth export format](design/frequency-truth-export.md)
- [PC virtual-audio proposal, on hold](design/pc-virtual-audio.md)
- [USB OUT recorder proposal](design/uac2-out-recorder.md)
- [HF simulator survey and design background](design/hf-channel-simulator-survey.md)

## Documentation ownership

Each contract has one authoritative home. The protocol reference owns wire
grammar and response schemas; module guides own implementation, lifetime and
failure behavior; runbooks own complete operating sequences. Tutorials link
to reference material instead of copying its tables. The README describes the
project and routes readers to these guides.

Preserve the high-level view, maintainer explanation and useful worked examples.
For a small feature, a concise guide can link to its parent module's shared
views rather than repeat the entire pipeline. Update the owning guide with the
implementation. Keep current limitations explicit, including the distinction
between host tests, firmware builds and physical board tests.

Run results, timing transcripts, captures and generated reports belong in ignored
`build/` or `artifacts/` directories. Keep reusable recipes, acceptance criteria,
known implementation limits and design rationale in the documentation. A past
passing report is not a substitute for rerunning checks on changed inputs.
