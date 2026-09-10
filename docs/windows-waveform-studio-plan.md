# Windows Waveform Studio and Extensible Interference Platform Plan

Status: Proposed  
Planning date: 2026-09-10  
Target project: `waveform-generator`  
Primary platforms: Windows 11 host and MIMXRT1170-EVK  
Proposed phase: Phase 7 and later

## 1. Purpose

This document defines the architecture and delivery plan for a Windows 11
waveform-generation application with a studio mixing-board control surface. The
studio interface will be written in C# with Avalonia. A separate C++ engine will
generate a desired waveform, add controlled interference, and play the result
through a selected Windows sound card. The same control surface will also be
able to operate the RT1170 waveform player through that engine.

The product is intended to be both:

- An approachable live demonstration instrument.
- A deterministic engineering test fixture that records exactly what it did.

The design deliberately separates the user interface, command semantics, DSP,
audio transport, hardware transport, and evidence collection. This separation
allows new interference models, output devices, source formats, control
surfaces, and automation systems to be added without replacing the core
application.

## 2. Four views of the product

### 2.1 The 50,000-foot view

Waveform Studio is a virtual test bench. It creates a clean modem waveform,
passes that waveform through channel effects, mixes in one or more interfering
signals, and sends the result to either a Windows sound card or the RT1170
board. A studio-style console provides one strip for each effect or interferer.

The same scenario can be saved, replayed, automated, and compared across the
Windows and RT1170 execution targets.

### 2.2 The operator view

An operator selects a target, source, output device, and scenario. The operator
then uses faders and numeric controls to add fading, impulse noise, CW signals,
AM-equivalent interference, pulsed interference, or jamming. Meters show signal
levels, clipping, buffer health, and the exact output frame at which a control
change took effect.

The operator can save the complete console state, export the command history,
record the output, and replay the same run later.

### 2.3 The scientist and maintainer view

The application is a deterministic, block-scheduled signal graph controlled by
versioned typed commands. Each processing module declares stable identifiers,
parameter units, limits, update rules, state, resource requirements, and
telemetry. The Windows and RT1170 runtimes use the same DSP and command
semantics wherever their capabilities overlap.

The user interface is not part of the real-time signal path. GUI changes enter
a bounded command queue, are applied at an acknowledged frame boundary, and
are recorded with source, scenario, seed, build, and device identity.

### 2.4 The 5th-grader view

Imagine a music mixing desk. One slider controls the message we want to hear.
Other sliders add bad radio conditions, such as fading, beeps, crackles, or
another station. The computer keeps a notebook of every slider movement so it
can make exactly the same noisy signal again.

## 3. Current baseline and capability boundary

The existing project provides the foundation to reuse:

- A canonical 48-kHz mono waveform stream.
- Host waveform generation and offline artifact workflows.
- A signal impairment engine and deterministic scenario processing.
- RT1170 playback through WM8960, SAI1, and eDMA.
- Bounded live control using `WFG-LIVE/1`.
- Continuous CW support with multiple slots.
- Host capture and analysis utilities.
- Runtime counters, hashes, and provenance conventions.

The following must not be presented as already implemented:

- Direct project-owned Windows sound-card rendering.
- A Windows graphical control surface.
- Morse-keyed CW with valid WPM timing.
- A scientifically defined AM broadcast interference model.
- Dedicated ignition and lightning generators with different event models.
- Woodpecker-style burst generation.
- General spot, swept, barrage, and pulsed jammers.
- Sample-synchronized duplex playback and capture.

The GUI must expose a control only when the selected runtime advertises that
capability. A disabled placeholder may explain a planned feature, but it must
not imply that the signal is being generated.

## 4. Goals

### 4.1 Functional goals

- Run the waveform generator directly on Windows 11 through a selected sound
  card.
- Provide a studio mixing-board control surface.
- Control either a local Windows runtime or an attached RT1170 from the same
  application.
- Reuse the existing waveform, signal-lab, and live-control implementations.
- Add multiple simultaneous, independently controlled interference sources.
- Save, load, compare, and replay complete scenarios.
- Support headless operation for scripts and automated test campaigns.
- Record command application, frame accounting, seeds, counters, hashes, and
  device identity.
- Add new DSP modules without redesigning the GUI or audio backend.
- Add new execution targets and control transports without changing DSP code.

### 4.2 Architectural goals

- One authoritative DSP implementation for host and RT1170 where practical.
- Stable module and parameter identifiers.
- Versioned scenario and control schemas.
- Capability negotiation rather than platform assumptions.
- Fixed-capacity, real-time-safe execution on the RT1170.
- Optional rich host functionality without increasing embedded dependencies.
- Clear separation between convenience playback and evidence-grade playback.
- Explicit signal ordering and level-reference semantics.
- Forward-compatible extension points without committing prematurely to an
  unstable binary plugin ABI.

### 4.3 User-experience goals

- Controls that can be understood by looking at the console.
- Exact numeric entry for every scientific parameter.
- Immediate visual feedback without falsely claiming immediate DSP application.
- Consistent controls in local and RT1170 modes.
- Named snapshots for common test conditions.
- Clear warnings for resampling, clipping, unsupported modules, stale commands,
  and device changes.

## 5. Non-goals

- Generating an actual HF RF carrier through a 48-kHz sound card.
- Treating baseband audio interference as RF front-end validation.
- Replacing a calibrated RF signal generator, channel simulator, or SDR.
- Claiming modem conformance from successful waveform playback.
- Loading arbitrary third-party DSP binaries into the RT1170.
- Making Avalonia, .NET, WASAPI, or Windows types visible in the portable DSP
  libraries.
- Allowing the GUI thread to participate in real-time rendering.
- Hiding sample-rate conversion or recovery behavior from evidence records.

## 6. Guiding design principles

### 6.1 One logical instrument, multiple execution targets

The application presents a single logical console. An execution target decides
where the graph runs:

- `Local Windows`: DSP executes on the PC and writes to a host audio sink.
- `RT1170 USB`: DSP executes on the board and the PC sends live commands.
- `File renderer`: DSP executes on the PC and writes an artifact.
- `Null renderer`: DSP executes without an audio device for deterministic tests.

Future targets may include network-connected hardware, an SDR bridge, or a
receiver-in-the-loop service.

### 6.2 Commands, not GUI callbacks

The GUI never calls DSP setters. It creates typed commands. Commands use the
same validation, sequence, capability, queue, and acknowledgement rules for
local and remote execution.

### 6.3 Physical units are authoritative

The UI may use normalized slider positions, but commands carry physical values
such as hertz, decibels, milliseconds, WPM, modulation fraction, or frames.
This prevents a future visual redesign from changing experiment semantics.

### 6.4 The frame clock is authoritative

Wall-clock time is useful for display, but output frame position determines
when changes apply. Every accepted mutation reports the frame boundary at
which it became active.

### 6.5 Determinism is opt-out

Random processes use explicit seeds. A `Randomize` action generates and records
a new seed. No evidence run may depend on an unrecorded wall-clock seed.

### 6.6 Embedded limits are explicit

The RT1170 uses fixed capacities and static allocation in the real-time path.
The Windows runtime may have larger capacities, but scenarios exceeding board
limits are marked non-portable rather than silently reduced.

## 7. Proposed product components

| Component | Responsibility | Platform dependencies |
|---|---|---|
| `waveform-engine` | Headless process hosting sessions, execution targets, IPC, and evidence | C++ host |
| `wfg_runtime` | Session state, graph lifecycle, commands, frame clock, telemetry | Portable C++ |
| `signal_lab` | Portable DSP modules and deterministic generators | Portable C++ |
| `wfg_control` | Typed commands, validation, capabilities, protocol adapters | Portable C++ |
| `wfg_scenario` | Versioned scenario model, migration, validation | Portable C++ |
| `wfg_audio` | Audio sink/source abstractions and format boundaries | Portable interfaces |
| `wfg_wasapi` | Windows endpoint enumeration and event-driven rendering | Windows/WASAPI |
| `waveform-play.exe` | Headless local player and automation endpoint | Windows host |
| `Waveform.Studio` | Graphical control surface and studio IPC client | C#/.NET/Avalonia |
| `wfg_ipc` | Versioned local command, telemetry, and visualization transport | C++ and C# implementations |
| RT1170 runtime | Embedded target adapter, queues, audio hooks, telemetry | MCUXpresso/FreeRTOS |
| Evidence recorder | Commands, metadata, counters, hashes, captures | Host, bounded subset on board |

Names are provisional. Existing source boundaries should be reused during
implementation rather than duplicated merely to match this table.

## 8. System architecture

```text
+-----------------------------------+
| Waveform.Studio                   |
| C# + Avalonia + XAML              |
| MVVM, meters, scenarios, timeline |
+----------------+------------------+
                 |
        versioned WFG-IPC/1
       named pipe on Windows
                 |
+----------------v------------------+
| waveform-engine                  |
| C++ headless process             |
| authoritative validation         |
| session state and journal        |
+----------------+------------------+
                 |
        IRuntimeTarget interface
      +----------+------------------------------+
      |                                         |
+-----v----------------+              +---------v----------+
| LocalRuntimeTarget   |              | Rt1170RuntimeTarget|
| signal graph         |              | WFG-LIVE/1 over USB|
| frame scheduler      |              | remote telemetry   |
| bounded audio FIFO   |              +--------------------+
+-----+----------------+
      |
      +-------------+----------------+---------------+
      |             |                |               |
+-----v------+ +----v-------+  +-----v------+  +-----v------+
| WASAPI    | | file sink  |  | null sink  |  | future sink|
| render    | | WAV / PCM  |  | hash only  |  | platform   |
+------------+ +------------+  +------------+  +------------+
```

The C# process never owns the real-time audio callback and never loads portable
DSP state directly. It is a client of the C++ engine. This protects audio timing
from UI rendering and managed garbage collection, keeps the engine independently
scriptable, and avoids exposing C++ object ownership through a managed ABI.

## 9. Runtime target abstraction

The C++ engine and native CLI interact with an `IRuntimeTarget`, not directly
with USB, WASAPI, or the DSP graph. The C# studio interacts with an
`IRuntimeClient` that represents the same operations over `WFG-IPC/1`.

Conceptual interface:

```cpp
class IRuntimeTarget {
public:
    virtual TargetIdentity identity() const = 0;
    virtual CapabilitySet capabilities() const = 0;
    virtual SubmitResult submit(const CommandEnvelope& command) = 0;
    virtual RuntimeSnapshot snapshot() const = 0;
    virtual EventBatch poll_events() = 0;
    virtual Status arm(const SessionPlan& plan) = 0;
    virtual Status start() = 0;
    virtual Status stop() = 0;
};
```

This interface is conceptual, not a requirement to use virtual calls in an
embedded audio loop. The RT1170 implementation may use fixed function tables or
compile-time binding.

### 9.1 Studio runtime client

The Avalonia application owns only operator-facing state:

- The discovered engine connection and connection status.
- Capability and descriptor snapshots received from the engine.
- Commanded values and separately acknowledged applied values.
- Scenario editing state before it is submitted.
- UI layout, theme, panel visibility, and control mappings.
- Decimated meter, waveform, spectrum, event, and log views.

The studio does not assume that a submitted command has taken effect. It waits
for an `applied` event containing the sequence and output frame.

The client contract is represented in C# interfaces so the UI can be exercised
with a mock client without starting an audio engine:

```csharp
public interface IRuntimeClient
{
    TargetIdentity Identity { get; }
    CapabilitySet Capabilities { get; }
    RuntimeSnapshot Snapshot { get; }

    Task<SubmitResult> SubmitAsync(
        CommandEnvelope command,
        CancellationToken cancellationToken);

    IAsyncEnumerable<RuntimeEvent> ReadEventsAsync(
        CancellationToken cancellationToken);
}
```

### 9.2 Local runtime target

The local target owns:

- The selected source.
- The compiled signal graph.
- The control-command queue.
- The canonical output-frame clock.
- A bounded producer FIFO.
- The selected audio sink.
- Runtime telemetry and evidence state.

### 9.3 RT1170 runtime target

The RT1170 target owns no local DSP graph. It:

- Resolves the exact USB device identity.
- Negotiates capabilities.
- Encodes commands using `WFG-LIVE/1`.
- Does not automatically retry an uncertain mutation.
- Tracks accepted and applied sequence numbers.
- Maps board telemetry into the common runtime snapshot.
- Distinguishes transport loss from board runtime faults.

### 9.4 Future target options

| Target | Extension approach | Compatibility expectation |
|---|---|---|
| Network RT1170 | New command transport | Same target contract |
| SDR output | New sink or target | New sample-rate capabilities |
| Virtual audio cable | Existing WASAPI sink | No DSP changes |
| Receiver service | New duplex target | Adds decode telemetry |
| Offline campaign worker | New target wrapper | Same scenarios and journal |

## 10. Signal graph architecture

### 10.1 Default graph

```text
Source
  |
  v
Desired-channel processors
  - source gain
  - fading
  - timing/sample-slip effects
  |
  +---------------------------+
                              |
Interference generator bank --+--> summing bus --> master --> sink
  - CW slots                  |
  - impulse slots             |
  - AM-equivalent slots       |
  - pulsed slots              |
  - jammer slots              |
  - future adjacent signal ---+
```

By default, desired-signal fading occurs before interference is added. This
models a desired path fading independently of a local or differently propagated
interferer. Future scenarios may assign a separate channel processor chain to
an interferer, but the topology must be explicit in the scenario.

### 10.2 Graph plan and graph runtime

A scenario contains a declarative `GraphPlan`. Before playback, the target:

1. Resolves each module identifier through the module registry.
2. Checks versions and target capabilities.
3. Checks node, state, scratch, queue, and CPU budgets.
4. Validates routing and signal ordering.
5. Resolves parameter defaults and units.
6. Compiles an immutable `GraphRuntime`.
7. Allocates all required state before the target is armed.

The audio thread processes only the compiled graph. Structural changes create a
new graph off the audio thread and swap it at a safe boundary, or require a
stop/re-arm operation if the target cannot support a live swap.

### 10.3 Fixed-capacity embedded model

The portable graph contract must not require heap allocation while processing.
Each module declares:

- Persistent state bytes.
- Scratch bytes per block.
- Maximum event capacity.
- Maximum channel and port count.
- Supported sample rates and block sizes.
- Estimated CPU cost or a measured budget class.

The RT1170 graph compiler uses static arenas and rejects plans that exceed its
capacity. The Windows target can support larger plans while preserving the
same failure semantics.

### 10.4 Module categories

The registry recognizes a small set of stable roles:

- `source`: produces the desired waveform.
- `processor`: transforms an existing stream.
- `interferer`: produces an independently levelled stream.
- `mixer`: combines streams and reports headroom.
- `meter`: observes without changing the signal.
- `sink_adapter`: converts the canonical stream at a platform boundary.

New categories require a schema revision. New modules within a category do
not.

## 11. DSP module extension contract

### 11.1 Stable module descriptor

Every module publishes a descriptor resembling:

```cpp
struct ModuleDescriptor {
    ModuleId id;                 // Example: "wfg.interference.cw"
    uint16_t contract_version;
    ModuleRole role;
    const ParameterDescriptor* parameters;
    size_t parameter_count;
    ResourceRequirements resources;
    CapabilityFlags capabilities;
};
```

Module IDs and parameter IDs are serialized and must remain stable. Display
names may change without changing scenario meaning.

### 11.2 Parameter descriptor

Each parameter declares:

- Stable ID.
- Display name and short label.
- Value type: Boolean, integer, scalar, enumeration, duration, or reference.
- Physical unit.
- Minimum, maximum, default, and meaningful step.
- Linear, logarithmic, or discrete control mapping.
- Update policy.
- Smoothing or ramp policy.
- Evidence precision and serialization precision.
- Whether a change is safe while running.
- Optional dependency on another parameter.

Proposed update policies:

| Policy | Meaning |
|---|---|
| `block` | Apply atomically at the next DSP block boundary |
| `ramped` | Begin a deterministic ramp at the next block boundary |
| `event` | Schedule at an explicit output frame |
| `rearm` | Stop and recompile the graph before applying |
| `readonly` | Telemetry only |

The C# UI derives standard controls from descriptors. A module may supply a
custom Avalonia control or panel layout, but the custom panel still emits the
same typed parameters through `IRuntimeClient`.

### 11.3 Processing rules

All portable DSP modules must:

- Perform no allocation, file I/O, logging, or blocking calls in `process`.
- Use only supplied scratch and persistent state.
- Consume explicit seeds for stochastic behavior.
- Define reset, seek, loop, and end-of-source behavior.
- Define clipping and saturation behavior.
- Report event and fault counters.
- Define whether output depends on block size.
- Include a toy worked example in its technical documentation.

### 11.4 Registration options

The initial implementation should use a static registry. This is the safest
portable option and works on the RT1170.

| Option | Advantages | Costs | Decision |
|---|---|---|---|
| Static registry | Portable, deterministic, embedded-safe | Rebuild required for new modules | Initial implementation |
| Windows DLL plugin | Third-party extension without rebuild | ABI, trust, crash, and real-time risks | Defer |
| Out-of-process source | Failure isolation and language freedom | IPC latency and non-portable behavior | Future host-only option |
| VST-style plugin | Existing audio ecosystem | Semantics and licensing do not match test fixture | Not planned |

The public scenario and parameter contracts should be stable before considering
a DLL ABI. A future C ABI can wrap the same descriptors and processing model
without changing scenarios.

## 12. Command and control architecture

### 12.1 Command envelope

Every mutation is carried in an envelope containing:

- Protocol and schema version.
- Session identifier.
- Monotonic sequence number.
- Target module instance.
- Parameter or operation identifier.
- Typed value.
- Optional requested application frame.
- Optional transaction identifier.

The target returns separate states:

- `rejected`: invalid and will not be applied.
- `accepted`: validated and queued.
- `applied`: active at the reported output frame.
- `uncertain`: transport was lost before outcome was known.
- `superseded`: replaced by a newer coalesced command before application.

### 12.2 Local and remote command parity

The preferred flow is:

```text
C# typed UI request
       |
       v
WFG-IPC/1 --> C++ authoritative validator and command model
                         |
                         +--> local bounded queue --> LiveController --> graph
                         |
                         +--> WFG-LIVE/1 encoder --> USB --> RT1170 LiveController
```

The C# client may perform descriptor-driven validation for immediate operator
feedback, but the C++ engine remains authoritative. The engine normalizes the
request into the same typed command used by local and RT1170 targets. A
protocol-loopback mode should remain available for testing both serialized
paths.

### 12.3 Slider coalescing

A pointer movement can produce hundreds of GUI events. Sending all events would
fill a bounded command queue and make the control feel delayed.

The session controller therefore:

- Retains only the newest unapplied value for a coalescible parameter.
- Never coalesces transport actions, event triggers, seed changes, or explicit
  automation points.
- Records superseded values only in optional UI diagnostics, not as applied DSP
  events.
- Displays the latest commanded value separately from the applied value.

### 12.4 Atomic scene changes

A scene recall may change many parameters. Applying half of a scene for one
block would produce an unintended signal. The protocol should add a bounded
transaction mechanism:

```text
BEGIN <transaction-id>
SET <module> <parameter> <value>
SET <module> <parameter> <value>
COMMIT <transaction-id> [AT_FRAME <frame>]
```

The target validates and reserves the complete transaction before accepting it.
Targets advertise maximum commands and bytes per transaction. Existing
single-command behavior remains compatible.

### 12.5 Capability negotiation

Capabilities include:

- Target identity and firmware/runtime version.
- Protocol versions.
- Supported source and module IDs.
- Module contract versions.
- Maximum module instances by type.
- Sample rates, block sizes, and channel formats.
- Queue and transaction limits.
- Capture and telemetry support.
- Evidence features.

The RT1170 protocol should use compact flags or chunked responses rather than
attempting to fit a complete desktop descriptor catalog into one small live
message. Both sides compile the common descriptor registry and negotiate IDs
and versions.

### 12.6 Local IPC protocol

`WFG-IPC/1` is a host-only transport contract between the Avalonia studio and
the C++ engine. It does not replace `WFG-LIVE/1`; it carries the richer desktop
session model and maps target mutations to the same authoritative C++ commands.

Initial transport:

- Per-session Windows named pipe.
- Restrictive current-user pipe access.
- Random connection token passed when the engine is launched.
- Length-prefixed frames rather than newline-dependent parsing.
- UTF-8 JSON control and status payloads for inspectability.
- Explicit maximum frame size and bounded decoding.
- Correlation IDs for every request and response.
- Independent monotonic runtime event sequence.

Message families:

```text
HELLO / HELLO_ACK
CAPABILITIES
OPEN_SESSION / CLOSE_SESSION
LOAD_SCENARIO / VALIDATION_RESULT
ARM / START / STOP / DRAIN
SUBMIT_COMMAND / COMMAND_ACCEPTED / COMMAND_APPLIED
BEGIN_TRANSACTION / COMMIT_TRANSACTION
SNAPSHOT / EVENT / FAULT / FINAL
SUBSCRIBE_METERS / SUBSCRIBE_VISUALIZATION
```

Low-rate commands, status, counters, and meters can use JSON. High-rate waveform
or spectrum data may later use a bounded binary frame type or shared-memory ring
without changing command semantics. Visualization data is lossy and may be
dropped; control, faults, and final evidence events are not.

Future Unix hosts use a Unix-domain socket implementation behind the same C#
and C++ transport interfaces.

### 12.7 Engine lifecycle and disconnect policy

The studio normally launches `waveform-engine` with a unique pipe name and
connection token. It may also attach to an explicitly selected existing engine.

The session records one disconnect policy:

- `stop`: stop or safely drain when the studio disconnects; default for live
  operator sessions.
- `continue`: keep running headlessly; intended for evidence and campaign runs.
- `mute`: keep processing but mute the physical sink; optional diagnostic mode.

The UI must show which policy is armed. Reconnection obtains a complete snapshot
before controls are enabled.

## 13. Audio stream and scheduling contract

### 13.1 Canonical stream

The compatibility profile retains:

- 48,000 frames per second.
- One desired audio channel.
- Normalized float between source, graph, static impairments, and live controls.
- Signed packed PCM24 for generated WAV artifacts and the RT1170 file boundary.
- Signed 24-bit samples left-aligned in the RT1170 codec's 32-bit I2S slots.
- Windows evidence mode requires a 24-bit-capable endpoint path: PCM24,
  24-valid-bits-in-32, or IEEE float. PCM16 fallback is convenience-only and
  must be labeled as such in session evidence.
- The existing engine block contract.
- A 2,048-frame live-control epoch unless a target explicitly advertises
  another compatible mode.

Internal module arithmetic may use a wider integer or floating-point
accumulator. Conversion and clipping occur only at defined boundaries.

### 13.2 Frame accounting

The runtime tracks at least:

- Source frames read or generated.
- Desired-path frames processed.
- Output frames produced.
- Output frames submitted to the sink.
- Output frames confirmed consumed where the API provides that fact.
- Captured input frames.
- Dropped, duplicated, padded, or discarded frames.

Sample-slip modules make source and output frame positions diverge. Commands
therefore use the canonical output-frame clock unless explicitly scoped to a
source position.

### 13.3 Producer and render separation

The local Windows target uses:

```text
DSP producer thread --> bounded SPSC audio FIFO --> WASAPI render thread
```

The producer works in canonical DSP blocks. The render thread consumes whatever
frame count the endpoint requests, including partial DSP blocks. The FIFO
stores frame-position metadata so acknowledgements and faults remain traceable.

### 13.4 Underrun policy

Evidence mode:

- Latch the first underrun frame.
- Increment exact underrun and inserted-silence counts.
- Mark the run invalid.
- Stop or drain according to the selected fail-closed policy.

Demonstration mode:

- Insert silence rather than stale audio.
- Continue if configured.
- Keep the fault indication visible until acknowledged.
- Record the run as non-evidence-grade.

Stale-buffer replay is never an acceptable recovery policy.

### 13.5 Parameter smoothing

Smoothing belongs in the portable module, not in the UI. A ramp is defined by:

- Start output frame.
- Start value.
- End value.
- Duration in frames.
- Curve type.

This makes a slider move replayable and gives Windows and RT1170 the same
transition behavior.

## 14. Windows audio architecture

### 14.1 WASAPI decision

Use native event-driven WASAPI rather than a general multimedia wrapper.

| Option | Advantages | Costs | Decision |
|---|---|---|---|
| WASAPI | Stable endpoint IDs, exclusive mode, detailed timing and fault data | Windows-specific implementation | Selected |
| PortAudio | Cross-platform and simple API | Additional abstraction and weaker Windows evidence detail | Possible future sink |
| SDL audio | Convenient with SDL UI stacks | Less control over evidence semantics | Not selected |
| JUCE | Strong audio application framework | Licensing and framework ownership | Not selected |
| Windows waveOut | Simple legacy API | Inferior endpoint and timing control | Capture compatibility only if needed |

### 14.2 Endpoint identity

Users select a friendly endpoint name, but scenarios and evidence store the
stable WASAPI endpoint ID plus:

- Friendly name at run time.
- Data flow and role.
- Device state.
- Negotiated format.
- Driver-reported periods and buffer size.
- Shared or exclusive mode.

A changed or missing endpoint requires explicit operator selection. The runtime
must not silently fall back to the Windows default device during an evidence
run.

### 14.3 Evidence mode

Evidence mode requests:

- WASAPI exclusive rendering.
- A compatible 48-kHz format.
- Explicit channel mapping.
- No hidden application-level resampling.
- Fixed and recorded buffer configuration.
- Fail-closed behavior for endpoint changes and underruns.

If the endpoint supports only stereo, mono is duplicated or routed according to
an explicit channel-routing setting at the sink boundary. The internal graph
remains mono.

### 14.4 Convenience mode

Convenience mode may use WASAPI shared mode. The UI must display a persistent
`SHARED` indication and record the Windows mix format. If Windows can resample
the stream, the run is not sample-exact evidence.

### 14.5 Future audio sinks

The `IAudioSink` boundary permits:

- WASAPI output.
- WAV or raw PCM output.
- Null/hash output.
- Network stream output.
- SDR or high-rate DAC output.
- Virtual loopback test sink.

Sink adapters do not contain interference logic.

## 15. Threading model

### 15.1 Host threads

| Process/thread | Responsibility | May block? |
|---|---|---|
| Studio UI | Avalonia painting and operator input | Yes, separate process |
| Studio IPC client | Requests, events, reconnection, view-model updates | Yes, asynchronous |
| Engine IPC server | Bounded decode, validation dispatch, subscriptions | Bounded waits only |
| Engine session/control | Coalescing, target commands, scenario operations | Bounded waits only |
| Engine DSP producer | Command application and graph processing | No unbounded operations |
| Engine WASAPI render | Endpoint event and FIFO drain | No allocation or UI work |
| Engine telemetry | Snapshot aggregation and IPC publication | Yes, bounded |
| Engine capture writer | Buffered disk output | Yes, isolated by bounded FIFO |

### 15.2 Queue rules

- Use bounded SPSC queues for one-producer/one-consumer real-time paths.
- Use bounded MPSC only where multiple command producers are genuinely needed.
- Allocate queue storage before arming.
- Record high-water marks and overflow attempts.
- Do not lock the audio callback.
- Do not publish a partially updated runtime snapshot.
- UI refresh should read immutable or double-buffered snapshots.

### 15.3 Priority and affinity

The Windows render and producer threads should use appropriate Multimedia Class
Scheduler Service characteristics. Priority changes must be recorded and must
fall back safely if unavailable. CPU affinity should remain an optional
diagnostic setting, not a default requirement.

## 16. Studio control-surface design

### 16.1 Visual language

The application should look like a purpose-built broadcast and laboratory
console:

- Dark graphite console surface.
- Warm neutral meter faces.
- Amber for enabled or armed controls.
- Cyan or teal for active signal flow.
- Red only for clipping, invalid state, or faults.
- `Bahnschrift` for panel labels.
- `Cascadia Mono` for measurements, sequence numbers, and frame positions.
- Restrained motion limited to meters, transport state, and applied-value
  transitions.

The design should not imitate a generic web dashboard or rely on decorative
skeuomorphism.

### 16.2 Main regions

```text
+-----------------------------------------------------------------------+
| target | endpoint | mode | sample rate | connection | READY/FAULT     |
+-----------------------------------------------------------------------+
| source and transport | scene controls | run identity | elapsed/frames |
+-----------------------------------------------------------------------+
| desired | fading | ignition | lightning | CW1 | CW2 | CW3 | CW4 | ... |
| channel strips with faders, exact values, enable, bypass, mute, solo   |
+-----------------------------------------------------------------------+
| scope | spectrum | event timeline | command and fault log             |
+-----------------------------------------------------------------------+
| RMS | peak | headroom | clip | FIFO | underrun | seed | MASTER        |
+-----------------------------------------------------------------------+
```

### 16.3 Control rules

- Every slider or dial has an exact editable value and unit.
- Double-click resets to the documented default.
- Shift-drag gives fine adjustment.
- A control shows commanded and applied values when they differ.
- Controls are disabled with a reason when unsupported.
- Level controls state their reference convention in a tooltip and details
  panel.
- Dangerous output-level changes may require the output to be armed.
- Bypass preserves settings; reset restores defaults.
- Mute suppresses one contribution without changing its settings.
- Solo is a monitoring convenience and is recorded if it affects output.

### 16.4 Generated and custom panels

Standard module panels are generated from parameter descriptors. This makes a
new module immediately operable without writing new GUI code. A module may add
a custom Avalonia control for specialized visualization or workflows, but:

- The descriptor remains authoritative.
- The custom panel cannot bypass typed commands.
- A generic fallback panel always remains available.
- Scenario files never contain Avalonia control state.

### 16.5 Layout persistence

Console layout and scientific scenario state are separate:

- User profile stores window geometry, panel order, collapsed strips, theme,
  meter refresh rate, and preferred endpoint.
- Scenario stores signal graph, parameters, source, seeds, automation, and
  evidence settings.

Loading another operator's layout must not change the generated waveform.

## 17. Selected C# user-interface architecture

### 17.1 Framework decision

Use C# and Avalonia with XAML for the studio application. Use MVVM to separate
operator presentation, editable scenario state, target state, and runtime
events. The C++ engine remains usable without .NET.

| Option | Strengths | Weaknesses | Decision |
|---|---|---|---|
| Avalonia | C#/XAML, desktop-first, Windows/Linux/macOS, custom-drawn console | Separate .NET toolchain and packaging | Selected |
| Uno Platform | C#/XAML with broad desktop, web, and mobile reach | Broader platform model than currently required | Retained alternative |
| .NET MAUI | Microsoft .NET UI stack and mobile support | No first-party Linux desktop target | Not selected |
| Qt 6 Widgets | Mature C++ controls and direct native integration | Keeps UI in C++ and adds Qt deployment decisions | Principal fallback |
| Win32/Direct2D | Native and dependency-light | High implementation and accessibility cost | Strict-dependency fallback |
| Dear ImGui | Fast engineering UI development | Less polished operator and accessibility behavior | Diagnostic UI only |

### 17.2 C# project responsibilities

The studio solution contains:

- Avalonia views and control templates.
- MVVM view models and immutable display snapshots.
- Generated or shared C# protocol data contracts.
- Named-pipe and future Unix-socket clients.
- Scenario editing and migration presentation.
- Studio layout and user preferences.
- Decimated visualization rendering.
- Mock runtime clients for UI development and testing.

It does not contain:

- DSP implementations.
- WASAPI callbacks.
- RT1170 USB protocol ownership.
- Authoritative command validation.
- Evidence hashes or final frame accounting.

### 17.3 Dependency direction

```text
Waveform.Studio.csproj --> Avalonia packages
Waveform.Studio.csproj --> WFG-IPC C# contracts

waveform-engine CMake target --> portable C++ runtime
waveform-engine CMake target --> platform audio and USB transports

portable C++ runtime -X-> .NET or Avalonia
RT1170 firmware      -X-> .NET or Avalonia
```

No C++/CLI bridge is required. If an in-process integration is considered in
the future, it must expose a small stable `extern "C"` API rather than C++ class
layouts. It remains an alternative, not the initial architecture.

### 17.4 Packaging

The Windows package contains:

- A self-contained or explicitly runtime-dependent `Waveform.Studio.exe`.
- `waveform-engine.exe` and its native dependencies.
- Version-matched IPC contract metadata.
- Default themes and safe example scenarios.
- License notices and build provenance.

Studio and engine versions are negotiated at connection time. An incompatible
pair fails before a session is armed.

## 18. Interference module roadmap

### 18.1 Desired-signal fading

Baseline model:

```text
y[n] = g[n] * x[n]
```

where `g[n]` is generated from an explicitly selected model.

Planned models:

| Model | Parameters | Purpose |
|---|---|---|
| Manual | gain or depth | Demonstration and threshold search |
| Periodic | depth, period, shape, phase | Repeatable cyclic fades |
| Outage | start, duration, depth, recovery | Recovery and reacquisition |
| Bounded random | depth range, dwell range, seed | Repeatable field-like variation |
| Rayleigh/Rician | Doppler and K-factor parameters | Later channel-research option |

The first release should prioritize manual, periodic, and scheduled outage
models. Statistical models require documented assumptions and validation.

### 18.2 Ignition noise

Ignition noise is a burst of related pulses, not independent generic clicks.
Parameters may include:

- Burst rate or RPM-equivalent rate.
- Pulses per burst.
- Intra-burst spacing.
- Pulse width, attack, and decay.
- Ringing frequency and damping.
- Polarity behavior.
- Peak level and seed.

### 18.3 Lightning static

Lightning crashes use a different envelope and arrival process:

- Arrival distribution or bounded interval.
- Attack time.
- Multi-stage decay.
- Optional ringing or colored tail.
- Peak-level distribution.
- Burst clustering.
- Seed.

The UI may offer simple presets, but the scenario records the expanded physical
parameters rather than only a preset name.

### 18.4 CW and keyed CW

Continuous CW uses:

```text
i[n] = A * sin(2*pi*f*n/Fs + phase)
```

The keyed extension multiplies the carrier by a deterministic envelope. It
must define:

- Text or symbol sequence.
- WPM and timing standard.
- Farnsworth spacing if used.
- Dot/dash weighting.
- Rise/fall envelope.
- Repeat delay.
- Start phase and start frame.
- Behavior on live frequency and speed changes.

Continuous sine generation must not be described as Morse until the keyer is
implemented.

### 18.5 AM-equivalent interference

The sound-card runtime operates in audio baseband. It cannot emit a 1-MHz HF
carrier. The planned module represents an audio-band equivalent such as a
heterodyne or modulated interfering component:

```text
i[n] = A * (1 + m*u[n]) * cos(2*pi*f_audio*n/Fs + phase)
```

where `u[n]` may be a tone, noise process, or audio program source and `m` is
the modulation fraction.

Parameters include:

- Audio-band carrier or beat frequency.
- Modulation depth.
- Modulation source and source gain.
- Interference level.
- Carrier leakage.
- Optional over-modulation policy.

Documentation and UI labels must identify this as an audio-equivalent model,
not an RF AM transmitter.

### 18.6 Woodpecker-style pulsed interference

The base model is a gated tone, chirp, or noise source:

- Pulse repetition frequency.
- Pulse width.
- Pulses per burst.
- Burst repetition period.
- Frequency or bandwidth.
- Sweep span and direction.
- Timing and frequency jitter.
- Level and seed.

This is also an audio-equivalent model. It can reproduce the cadence and
baseband obstruction relevant to a receiver audio path, not the RF propagation
or front-end overload of a historical transmitter.

### 18.7 Intentional jammer family

The jammer framework should share schedulers, oscillators, noise sources, and
envelopes rather than duplicate implementations.

Initial modes:

- Spot tone.
- Swept tone.
- Repeating chirp.
- Spot noise.
- Barrage noise.
- Pulsed noise.

Common parameters include center frequency, bandwidth, level, sweep rate, duty
cycle, burst length, and seed.

### 18.8 Adjacent valid waveform interference

A future interferer may be another registered waveform source. This enables
tests with an adjacent or overlapping valid signal rather than only synthetic
noise. The architecture should permit a source instance to feed an interferer
strip with independent level, timing offset, frequency translation, and
channel effects.

## 19. Level and clipping semantics

### 19.1 Level references

Every level must declare one of these references:

- `dBFS`: relative to full-scale digital amplitude.
- `clean_rms`: relative to a fixed clean desired-signal RMS measurement.
- `C/I`: desired RMS divided by interferer RMS.
- `absolute_linear`: explicit linear gain for diagnostics.

For C/I:

```text
C/I dB = 20 * log10(desired_rms / interferer_rms)
```

Therefore, `C/I = +12 dB` means the desired signal is 12 dB stronger than the
interferer. The UI must not use an ambiguous label such as only `Level: 12`.

The clean reference window and computed RMS are recorded. A fixed reference is
preferred over a rolling reference because a deep fade must not cause an
interferer level to change unintentionally.

### 19.2 Mixing and headroom

The summing bus uses a wider accumulator. The master stage applies an explicit
headroom policy before the single conversion to the selected 24-bit-capable
endpoint format.

Proposed policies:

| Policy | Behavior | Use |
|---|---|---|
| `fail_on_clip` | Latch fault and invalidate run | Default evidence mode |
| `hard_clip` | Saturate and count samples | Deliberate clipping tests |
| `soft_limit` | Apply documented nonlinear curve | Demonstrations only |
| `auto_headroom` | Precompute a fixed master reduction | Convenience, recorded |

There is no silent limiter. Any nonlinear policy is part of the scenario and
evidence record.

## 20. Scenario model

### 20.1 Scenario contents

A scenario contains:

- Schema version and scenario identifier.
- Human-readable title and notes.
- Source descriptor and source identity.
- Canonical stream format.
- Graph topology and ordered module instances.
- Parameter values and seeds.
- Level-reference definitions.
- Automation tracks and scheduled events.
- Loop and end-of-source behavior.
- Sink requirements, not an implicit machine-specific fallback.
- Evidence policy.
- Optional expected capabilities.

Machine-specific UI layout does not belong in the scenario.

### 20.2 Illustrative scenario

```json
{
  "schema": "wfg-scenario/2",
  "id": "two-cw-deep-fade",
  "stream": {
    "sample_rate_hz": 48000,
    "channels": 1,
    "sample_format": "pcm16"
  },
  "source": {
    "kind": "wav",
    "path": "input.wav"
  },
  "graph": {
    "desired": [
      {
        "instance": "fade-main",
        "module": "wfg.channel.fade.periodic",
        "parameters": {
          "depth_db": 24.0,
          "period_seconds": 8.0,
          "shape": "raised-cosine"
        }
      }
    ],
    "interferers": [
      {
        "instance": "cw-1",
        "module": "wfg.interference.cw",
        "parameters": {
          "frequency_hz": 900.0,
          "ci_db": 12.0,
          "phase_degrees": 0.0
        }
      },
      {
        "instance": "cw-2",
        "module": "wfg.interference.cw",
        "parameters": {
          "frequency_hz": 1300.0,
          "ci_db": 12.0,
          "phase_degrees": 90.0
        }
      }
    ]
  },
  "master": {
    "clip_policy": "fail_on_clip",
    "headroom_db": 6.0
  },
  "random": {
    "seed": 1701
  }
}
```

This is an architectural example. The final schema must reuse existing field
names where compatible and pass formal schema review before becoming stable.

### 20.3 Schema evolution

- Major schema changes require an explicit migration.
- Minor additions use optional fields with documented defaults.
- Stable module and parameter IDs are never silently reassigned.
- Unknown modules are preserved by editors where possible but rejected for
  execution unless the target supports them.
- Migration produces a report of every changed or defaulted value.
- Saving a migrated scenario never overwrites the original without an explicit
  operator action.

## 21. Automation architecture

Automation should be expressed in output frames or exactly convertible time
units.

Proposed track types:

- Parameter point and ramp.
- Module enable/bypass event.
- Impulse or burst trigger.
- Scene recall.
- Source seek or restart.
- Marker and annotation.
- Measurement window.

Automation is compiled into bounded events before an evidence run. Live UI
changes use the same event representation after validation.

Future control adapters may include:

- MIDI control surfaces.
- OSC.
- A local named pipe.
- TCP on an authenticated laboratory network.
- Python campaign orchestration.
- Physical knobs attached to a microcontroller.

Adapters translate into typed commands. They do not control DSP directly.

## 22. Headless Windows player

`waveform-play.exe` provides the same runtime without Avalonia.

Proposed options:

| Option | Meaning |
|---|---|
| `--list-devices` | Enumerate stable render and capture endpoint identities |
| `--device-id ID` | Select the exact output endpoint |
| `--exclusive` | Require evidence-oriented exclusive mode |
| `--shared` | Use Windows shared-mode convenience playback |
| `--source PATH` | Select WAV or another registered source |
| `--scenario PATH` | Load a versioned scenario |
| `--loop` | Loop according to documented reset semantics |
| `--control-stdin` | Accept live protocol commands on standard input |
| `--control-pipe NAME` | Host a local named-pipe control endpoint |
| `--jsonl-status PATH` | Write READY, STATUS, event, fault, and FINAL records |
| `--capture-device-id ID` | Select an optional input endpoint |
| `--capture-out PATH` | Write captured audio and sidecar metadata |
| `--render-out PATH` | Render to a file sink instead of a sound card |
| `--null` | Run with counters and hashes only |
| `--evidence` | Enable fail-closed evidence policy |

Examples:

```powershell
waveform-play.exe --list-devices

waveform-play.exe `
  --device-id "{stable-wasapi-endpoint-id}" `
  --exclusive `
  --source input.wav `
  --scenario two-cw-deep-fade.json `
  --jsonl-status run.jsonl
```

The GUI links the runtime library directly. It must not shell out to the player
for normal operation.

## 23. Studio application command line

Proposed startup options:

```powershell
waveform-studio.exe `
  --backend windows `
  --device-id "{stable-wasapi-endpoint-id}" `
  --scenario two-cw-deep-fade.json

waveform-studio.exe `
  --backend rt1170 `
  --usb-serial FFF70008FFF60009
```

The application may open without an armed output so scenarios can be edited
safely while hardware is unavailable.

## 24. Telemetry and evidence

### 24.1 Runtime telemetry

The common snapshot should include:

- Runtime state: idle, ready, armed, running, draining, done, or fault.
- Target, build, and source identity.
- Current output frame and source frame.
- Accepted and applied command sequence.
- FIFO fill and high-water/low-water marks.
- Underrun, overrun, stale-buffer, and queue-overflow counts.
- Peak, RMS, headroom, and clipping counts.
- Active graph and seed identity.
- Sink format and endpoint identity.
- First fault type and frame.
- Stream digest state when available.

### 24.2 Journal

The command journal records:

- Original operator or automation request.
- Normalized typed value.
- Validation result.
- Accepted sequence.
- Requested frame, if any.
- Applied frame.
- Rejection, supersession, or uncertain outcome.
- Origin: UI, CLI, automation, MIDI, USB, or API.

### 24.3 Evidence levels

| Level | Evidence | What it establishes |
|---|---|---|
| E0 | Build and unit results | Component behavior in the tested build |
| E1 | Null/file render with hashes | Deterministic digital graph behavior |
| E2 | Windows WASAPI run | Operation through the named PC endpoint |
| E3 | RT1170 run | Operation on the named board and firmware |
| E4 | Physical analog capture | Behavior across the documented analog patch |
| E5 | Receiver decode result | Receiver outcome for the captured or live run |
| E6 | Formal procedure | Only the claims covered by that procedure |

No level automatically implies a higher level. In particular, a sound-card
playback, analog tone, or short decode does not establish conformance.

## 25. Worked toy stream example

Consider four desired samples:

```text
clean desired:       [1000,  500, -500, -1000]
fade gain:              0.5
after fade:          [ 500,  250, -250,  -500]
CW contribution:     [   0,  100,    0,  -100]
mixed:               [ 500,  350, -250,  -600]
master gain:             0.8
output:              [ 400,  280, -200,  -480]
```

The graph did not fade the CW because the CW entered after the desired-signal
fade. If the CW had its own fading processor, that would appear as another
explicit chain in the scenario.

For a real 2,048-frame block, the same sequence is:

1. Read or generate the desired block.
2. Apply commands scheduled for this output-frame boundary.
3. Process the desired-channel chain.
4. Generate every enabled interferer block.
5. Apply each interferer's fixed level reference and channel chain.
6. Sum into a wider accumulator.
7. Apply the master headroom and clipping policy.
8. Quantize once to signed PCM24 or convert to an explicitly negotiated
   24-bit-capable Windows endpoint format.
9. Update meters, hashes, counters, and frame positions.
10. Publish an immutable telemetry snapshot outside the audio callback.

## 26. Security and operational safety

- Start with output muted or disarmed after a new endpoint is selected.
- Store and display exact device and board identity.
- Require explicit confirmation before switching from file/null output to a
  physical output at a high level.
- Clamp parameters in the common validator, not only in the GUI.
- Do not retry an uncertain remote mutation automatically.
- Bound every queue, message, string, transaction, and event list.
- Treat scenario titles, notes, source paths, and remote text as data.
- Do not load arbitrary code from a scenario.
- Keep convenience-mode warnings visible in captures and evidence summaries.
- Record channel routing to avoid accidental left/right wiring assumptions.

## 27. Build configurations

Proposed configurations are additive to existing host and RT1170 presets:

| Configuration | Contents | Avalonia required |
|---|---|---|
| Host core | Portable libraries and existing tools | No |
| Windows playback | Core, WASAPI backend, `waveform-play.exe` | No |
| Windows studio | Playback runtime and `waveform-studio.exe` | Yes |
| RT1170 writable | Portable supported graph and board runtime | No |
| RT1170 read-only | Supported graph with media restrictions | No |
| Test/null | Portable runtime with null and file sinks | No |

Dependency options:

- Pin the .NET SDK and Avalonia package versions through the project's
  documented dependency mechanism.
- Keep .NET and Avalonia outside portable C++ libraries and RT1170 firmware.
- Use Windows SDK WASAPI headers without introducing a separate audio SDK.
- Keep optional visualization dependencies out of headless builds.

## 28. Proposed source layout

The exact placement should be reconciled with existing files during the first
implementation read pass. The intended boundaries are:

```text
common/
  control model, capabilities, command envelopes, protocol adapters

signal-lab/
  portable DSP nodes, descriptors, registry, graph compilation

host/runtime/
  local runtime target, session controller, journal, telemetry

host/audio/
  portable sink/source interfaces

host/audio/wasapi/
  endpoint catalog, render client, capture client, format adapter

host/playback/
  waveform-play command-line application

host/studio/
  C# Avalonia application, control surface, meters, target adapters

rt1170/
  target adapter, capacity table, compact capabilities, board telemetry

docs/modules/
  one technical document for each new module
```

The implementation must prefer extending existing abstractions over creating
parallel versions under these provisional names.

## 29. Documentation plan

The overall README will gain:

- Windows 11 purpose and limitations.
- Build configurations.
- Complete command-line option summaries.
- Windows local and RT1170 remote examples.
- Sound-card mode and device-selection guidance.
- Baseband-versus-RF boundary.
- Evidence-level explanation.
- Links to every new technical module.

New module documents:

- `windows-audio-renderer.md`
- `windows-headless-player.md`
- `studio-control-surface.md`
- `backend-and-live-control-routing.md`
- `signal-graph-and-module-registry.md`
- `scenario-automation-and-evidence.md`
- `interference-fading.md`
- `interference-impulse-noise.md`
- `interference-keyed-cw.md`
- `interference-am-equivalent.md`
- `interference-pulsed-and-jamming.md`

Each document must include:

- Scope and ownership boundary.
- 50,000-foot explanation.
- Operator how-to.
- Scientist and maintainer explanation.
- 5th-grader explanation.
- Data flow and code map.
- Units, equations, ordering, and invariants.
- Toy worked example.
- Failure modes and debugging guidance.
- Capability and evidence boundaries.
- Change checklist.

## 30. Delivery phases

### Phase 7A: Contract consolidation

Deliverables:

- Final runtime-target interface.
- Stable module and parameter descriptor model.
- Capability model.
- Scenario schema proposal and migration rules.
- Command acknowledgement and transaction design.
- Audio sink abstraction.
- Null and file sink reference implementations.
- Updated architecture and module documentation.

Exit criteria:

- Existing host and RT1170 behavior can be represented without GUI or WASAPI
  assumptions.
- A scenario can be validated against a target capability set before arming.
- A command's accepted and applied states are independently represented.

### Phase 7B: Headless Windows sound-card runtime

Deliverables:

- Stable WASAPI endpoint enumeration.
- Event-driven render backend.
- Exclusive evidence mode and shared convenience mode.
- Bounded producer FIFO and frame accounting.
- `waveform-play.exe`.
- JSONL status and evidence output.
- Existing live controls through stdin or named pipe.

Exit criteria:

- A selected waveform plays through an explicitly named endpoint.
- No endpoint fallback or resampling occurs silently.
- Underruns, clipping, queue faults, and frame counts are retained.
- File/null replay and live command logs can establish deterministic digital
  behavior separately from sound-card behavior.

### Phase 7C: Studio console MVP

Deliverables:

- Optional C#/.NET Avalonia application.
- Windows and RT1170 target selection.
- Source and transport controls.
- Endpoint and USB identity selection.
- Desired, fading, static, continuous-CW, and master strips for capabilities
  that already exist.
- Meter, FIFO, frame, command, and fault displays.
- Scenario save/load and command-journal export.

Exit criteria:

- The same supported parameter has the same unit and meaning in local and board
  modes.
- Commanded and applied state are visibly distinct.
- Unsupported controls cannot be activated.
- GUI activity does not enter the real-time callback.

### Phase 7D: Interference expansion

Implement in small independently reviewable modules:

1. Morse-keyed CW.
2. Scheduled outage and refined deep fading.
3. Ignition burst generator.
4. Lightning crash generator.
5. AM audio-equivalent interferer.
6. Woodpecker-style pulsed interferer.
7. Spot, swept, chirp, noise, and pulsed jammer family.
8. Adjacent valid-waveform source.

Each module requires:

- Portable engine implementation where feasible.
- Descriptor and capability entry.
- Scenario schema coverage.
- Generic and, if justified, custom UI.
- Deterministic file/null evidence.
- RT1170 resource-budget decision.
- Four-level technical documentation and toy example.

### Phase 7E: Automation and scene system

Deliverables:

- Atomic scene recall.
- A/B scenes.
- Frame-based parameter automation.
- Trigger and marker tracks.
- Compiled bounded event schedules.
- Command-log import and replay.
- Campaign-oriented headless interfaces.

### Phase 7F: Duplex capture and bench evidence

Deliverables:

- Optional WASAPI input capture.
- Separate render and capture clocks.
- Drift and discontinuity reporting.
- Recorded physical wiring description.
- Windows output to physical capture workflow.
- RT1170 output to Realtek capture workflow.
- Optional receiver result association without conflating it with generator
  correctness.

### Phase 7G: Extended control surfaces and targets

Candidate deliverables:

- MIDI fader and knob mapping.
- OSC or authenticated network control.
- Network-connected RT1170 transport.
- SDR/high-rate output adapter.
- Out-of-process experimental source provider.
- Multi-channel and higher-sample-rate graph profiles.

These additions use existing typed commands, descriptors, scenarios, and target
capabilities rather than bypassing them.

## 31. Acceptance strategy

### 31.1 Contract acceptance

- Stable IDs are documented and duplicate IDs are rejected.
- Scenario validation fails closed on unknown required modules.
- Parameter units and ranges are identical across generic UI, CLI, and live
  protocol adapters.
- Transactions are either fully applied or fully rejected.

### 31.2 Digital runtime acceptance

- A scenario and command journal replay to the same result under the same
  declared deterministic contract.
- Multiple block sizes are tested for modules claiming block-size independence.
- Source and output frame accounting remains valid across sample-slip events.
- Clipping policies produce explicit counts and status.
- Unsupported capability requests fail before playback.

### 31.3 Windows audio acceptance

- Endpoint selection uses the exact stable endpoint ID.
- Exclusive mode either obtains the required format or fails clearly.
- Shared mode is permanently identified as non-sample-exact.
- FIFO underrun never replays stale samples.
- Endpoint invalidation produces a latched fault and first-fault frame.
- Long-run counters and final drain state are retained.

### 31.4 GUI acceptance

- Every standard parameter can be edited numerically.
- Rapid slider movement cannot overflow the real-time queue.
- Commanded and applied values are distinguishable.
- Capability changes update controls without changing scenario data silently.
- Layout changes do not change signal output.
- The application remains operable with meters and spectrum panels disabled.

### 31.5 Cross-target acceptance

- Common scenarios are accepted or rejected according to advertised
  capabilities.
- Common commands retain the same unit, range, and ordering semantics.
- Exact pre-sink stream equality is required only where the portable math
  contract guarantees it.
- Otherwise comparison uses a documented numeric tolerance and is not called
  bit-identical.
- Windows and RT1170 analog captures are treated as separate physical evidence.

## 32. Risks and mitigations

| Risk | Consequence | Mitigation |
|---|---|---|
| GUI logic leaks into DSP | Unrepeatable or platform-specific output | Typed command boundary and generic panels |
| Slider event flood | Queue delay or overflow | Coalescing and bounded acknowledgements |
| Hidden Windows resampling | Invalid sample-exact claims | Exclusive evidence mode and recorded mix format |
| RT1170 resource exhaustion | Unsupported scenarios fail at runtime | Pre-arm graph budgeting and capabilities |
| Floating-point platform drift | Cross-target hashes differ | Portable math contract or tolerance-labelled comparison |
| Scenario schema churn | Old tests become unusable | Stable IDs, versioning, and explicit migrations |
| Premature DLL plugin ABI | Permanent compatibility burden | Static registry first, C ABI only after contracts stabilize |
| Ambiguous interference levels | Scientifically invalid runs | Explicit dBFS, clean RMS, and C/I references |
| RF claims from audio model | Misleading validation conclusions | Persistent baseband scope labels and documentation |
| UI meters disturb audio | Dropouts during visualization | Decimated observer FIFO and disableable panels |
| Automatic mutation retry | Duplicate or uncertain state change | Sequence tracking and no blind retry |
| Device default changes | Playback through wrong hardware | Stable endpoint identity and fail-closed evidence mode |

## 33. Extension examples

### 33.1 Add a new chirped interferer

The maintainer adds:

1. A portable module with stable ID `wfg.interference.chirp`.
2. Parameter descriptors for start frequency, end frequency, duration, level,
   envelope, repeat interval, and seed.
3. A static registry entry.
4. A capability bit and RT1170 resource declaration.
5. Scenario-schema examples.
6. A technical module document.

The generic GUI creates a usable strip automatically. A custom spectrogram
preview can be added later without changing command or scenario semantics.

### 33.2 Add a MIDI mixing surface

The maintainer adds a MIDI input adapter that maps hardware control IDs to
typed module parameters. The adapter emits the same commands as the GUI. No DSP,
WASAPI, scenario, or RT1170 code changes are required.

### 33.3 Add an SDR output

The maintainer adds a new sink with sample-rate and channel capabilities. A new
graph profile may introduce complex I/Q samples, but the existing 48-kHz audio
profile remains unchanged. Scenarios declare the required profile explicitly.

### 33.4 Add a network-controlled board

The maintainer adds a transport under the existing RT1170 runtime target. Device
identity, sequence handling, uncertain outcomes, and capabilities remain part
of the same contract.

## 34. Decision record

| ID | Decision | Rationale |
|---|---|---|
| D-001 | Use one logical console with local and RT1170 targets | Prevent divergent operator semantics |
| D-002 | Keep DSP portable and UI-free | Reuse and deterministic testing |
| D-003 | Use typed commands and frame acknowledgements | Repeatable live control |
| D-004 | Use native WASAPI for Windows output | Endpoint identity and evidence control |
| D-005 | Use C# and Avalonia for the studio UI | Cross-platform XAML UI without coupling DSP to .NET |
| D-013 | Use normalized float internally and one PCM24 destination quantizer | Preserve 24-bit source precision across static and live processing |
| D-006 | Start with a static module registry | Embedded safety and no premature ABI |
| D-007 | Use capability negotiation | Safe feature growth across unequal targets |
| D-008 | Separate evidence and convenience audio modes | Prevent hidden-resampling claims |
| D-009 | Treat AM and woodpecker models as audio-equivalent | Respect the 48-kHz baseband boundary |
| D-010 | Keep layout state outside scenarios | Visual changes cannot alter experiments |
| D-011 | Use fixed clean RMS by default for C/I | Fades do not move interference level |
| D-012 | Put deterministic ramps in DSP modules | Cross-target control parity |

## 35. Open decisions before implementation

These decisions should be resolved during Phase 7A without changing the overall
architecture:

- .NET SDK and Avalonia package pinning and offline-cache policy.
- Exact C++ language level supported by all host and RT1170 toolchains.
- Canonical internal accumulator representation.
- Exact local command serialization test boundary.
- Transaction size and protocol encoding within RT1170 message limits.
- Default WASAPI FIFO depth and fail-closed stop/drain policy.
- Whether exclusive stereo duplication is mandatory when mono is unavailable.
- First-release fading models.
- First-release ignition and lightning pulse shapes.
- Whether capture belongs in the first Windows release or Phase 7F.
- Exact spectrum implementation and maximum observer CPU budget.

## 36. Definition of done for the initial Windows Studio effort

The initial effort is complete when:

- `waveform-play.exe` plays through an explicitly selected Windows 11 sound
  card in shared and exclusive modes.
- `waveform-studio.exe` controls both the local runtime and RT1170 through one
  consistent control model.
- The studio exposes the source, transport, desired signal, existing fading,
  current static/impulse functionality, four continuous-CW slots, and master
  output controls.
- Scenarios and command journals are versioned, saveable, and replayable.
- Each mutation has accepted and applied status with frame position.
- Device, source, seed, graph, counters, format, and final state are retained.
- Unsupported planned interference controls cannot masquerade as active.
- The architecture accepts new registered modules without modifying the audio
  backend or session controller.
- The architecture accepts new targets without modifying DSP modules.
- Windows convenience, Windows evidence, RT1170, analog capture, decoder, and
  conformance results remain explicitly separate claims.
- All new technical modules have the required four-level documentation and toy
  worked examples.

## 37. Recommended first implementation slice

The first slice should be deliberately vertical:

1. Consolidate the portable runtime target, command, descriptor, and scenario
   contracts.
2. Add null and file sinks under the new runtime boundary.
3. Add WASAPI endpoint enumeration and exclusive/shared rendering.
4. Implement `waveform-play.exe` with existing source and live controls.
5. Add the Avalonia shell with target selection, transport, master, meters, and one
   generated module strip.
6. Connect all currently supported fading, static, and continuous-CW controls.
7. Save and replay one scenario and its command journal on both local Windows
   and RT1170 targets.

This slice proves the extension architecture before the project invests in the
larger family of new interference models.
