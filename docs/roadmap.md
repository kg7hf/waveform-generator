# Roadmap

For the reader-facing inventory of complete, partial and planned features,
see [Feature status](feature-status.md). This roadmap records acceptance gaps
and priorities rather than duplicating the interface list.

## Current priorities

The project generates and renders waveform scenarios on the host and supports SD-backed playback and USB audio on RT1170 and RT1064 targets. RT1064 firmware supports USB playback. This does not qualify analog audio, host stress, simultaneous multi-PC operation, modem performance, or general firmware release readiness.

Prioritize work in this order:

1. **Repeat USB playback on multiple PCs and fixtures.** Hold firmware, WAV, receiver, negotiated format, and capture settings constant; then swap boards and PCs to separate host effects from board variation. Include representative host load, reconnects, and sustained capture. Keep exact sample comparison bounded to digital USB paths.
2. **Close the independent board checks.** Confirm RT1170 behavior and its WM8960 codec path separately. For RT1064, complete USB multi-PC and host-load checks before qualifying its WM8960 analog route. USB pass results do not transfer to analog paths.
3. **Keep host preparation and artifacts reproducible.** Use supplied WAVs by default; optional encoder plugins and scenario rendering extend the workflow. Record source, scenario, seed, renderer, output identity, and playback/capture identity separately.
4. **Measure live processing on target.** Host tests and offline ARM builds establish contracts and feasibility only. Measure CPU margin, memory/stack bounds, SD behavior, and transport continuity under the intended combined workload before making real-time claims.
5. **Keep PC virtual-audio delivery on hold.** The current Python WAV playback workflow remains available. Revisit a project-owned delivery backend only after transport feasibility and capture behavior have a concrete design; see [the proposal](design/pc-virtual-audio.md).

## Acceptance status

| Area | Current evidence | Remaining acceptance |
|---|---|---|
| RT1064 USB digital playback | Firmware support is implemented | Repeat checks across PCs and fixtures; test host load, reconnects and broader workloads |
| RT1064 analog codec | No analog qualification is claimed | Bring up and measure WM8960 route independently |
| RT1170 USB and codec | Existing supported target; see the [player and audio guide](modules/rt1170-player-and-audio.md) for current scope | Run the shared campaign and preserve separate USB/analog evidence |
| Host rendering and artifact workflows | Host tests and validation runners cover defined software contracts | Keep results tied to exact inputs and runner identities |
| Live target capacity | Offline cross-build and host simulation provide feasibility evidence | Measure runtime margins, continuity and reset behavior on each target |
| PC virtual audio | On hold | Revisit only after transport feasibility is established |

Use [testing procedures](testing.md) for current host checks and acceptance commands. Treat software tests, offline cross-builds, physical playback, analog measurements, and receiver/modem results as separate evidence classes. Update this roadmap when a defined acceptance row changes; do not retain dated execution transcripts here.
