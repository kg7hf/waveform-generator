# Optional waveform encoder plugins

## 50,000-foot view

The normal waveform generator starts with a WAV file. It can play that file on
RT1170, add CW/noise/fades, render variants, and capture or replay experiments
without an encoder. The default build does not fetch, include or link a plugin.

A plugin is a separately supplied source module that converts payload bytes to
audio. It is selected at build time and statically linked into the host library
or writable RT1170 player. There is no runtime DLL loader. Different modules can
implement different modulation families without changing the player or effects.

```text
Existing WAV -------------------------------+--> playback + live effects
Payload -> optional Encoder -> PCM24 WAV ----+
                                            +--> offline effects -> mixed WAV
```

## How to use it

### Default public build

```powershell
cmake --preset host-release -DWFG_ENABLE_ENCODER_PLUGINS=OFF
cmake --build --preset host-release --parallel
cmake --preset rt1170-player-release -DWFG_ENABLE_ENCODER_PLUGINS=OFF
cmake --build --preset rt1170-player-release --parallel
```

The firmware's `INFO?` reports `encoders: []`. Generation and payload-upload
requests return `ENCODERS_DISABLED`. WAV files, writable USB mass storage, SD
playback, and live effects remain available. A read-only player returns
`READ_ONLY_IMAGE` and excludes encoder sources even if the plugin option is on.

### Public example module

Use separate build directories so public and plugin binaries are easy to identify:

```powershell
cmake --preset host-release -B build/host-example `
  -DWFG_ENABLE_ENCODER_PLUGINS=ON -DWFG_ENCODER_PLUGIN_DIR=examples/encoder-plugin
cmake --build build/host-example --parallel

cmake --preset rt1170-player-release -B build/rt1170-example `
  -DWFG_ENABLE_ENCODER_PLUGINS=ON -DWFG_ENCODER_PLUGIN_DIR=examples/encoder-plugin
cmake --build build/rt1170-example --parallel
```

The [example adapter](../examples/encoder-plugin/tone_encoder.hpp) registers
`TONE`. Its profile is an integer frequency in Hz from 1 through 20000. Each
payload byte adds 480 frames (10 ms) of tone at a peak amplitude of 0.125; byte
values are ignored. The shared WAV writer adds one second of silence and writes
mono 48 kHz packed PCM24. This demonstrates the interface, not a radio protocol.

With that firmware loaded, a nonempty `/WG/PAYLOAD.BIN` staged, playback stopped,
and card ownership handed to firmware, send through the existing live controller:

```text
INFO?
ENCODE TONE 1000 PAYLOAD.BIN TONE.WAV
STATUS?
```

Poll until generation completes before `LOAD:TONE.WAV` and `PLAY`. A 100-byte
payload gives one second of tone plus one second of tail silence. Use a new
output name for each generation. The example does not opt into the old
rate/interleave upload protocol. Existing WAVs and live CW controls need no plugin.

### Separate repositories and optional submodules

Keep the main repository's `.gitmodules` limited to its public dependencies.
An unconditionally listed private submodule would cause recursive public clones
to ask for credentials. Instead, select a module through `WFG_ENCODER_PLUGIN_DIR`.
It may point to any local checkout, including a submodule in a separate wrapper:

```text
personal-workspace/       # separately maintained repository
  waveform-generator/    # public project submodule
  encoders/              # separately published/private module submodule
```

Run CMake from `waveform-generator` with `-DWFG_ENCODER_PLUGIN_DIR=../encoders`.
Alternatively, clone a module into ignored `private/encoder-module` and use that
path. `CMakeUserPresets.json` is also ignored for machine-specific build settings.
CMake performs no clone or network operation. Enabling plugins with a missing
module is a configure error; it never silently substitutes another encoder.

Git submodules inherently record a commit in their parent repository. No extra
source hashes, lock file or controlled-delivery process is needed here.

## Scientist and maintainer view

A module provides `wfg-plugin.cmake` with absolute paths in
`WFG_ENCODER_PLUGIN_SOURCES` and optional `WFG_ENCODER_PLUGIN_INCLUDE_DIRS`.
Use `CMAKE_CURRENT_LIST_DIR` to construct those paths. It implements
`waveform_generator::encoder_plugin_descriptors()` declared in
[`encoder_registry.hpp`](../waveform-source/encoder_registry.hpp). The public
example is a complete minimal module. One selected module can register multiple
encoders. An optional module `CMakeLists.txt` adds host tools/tests; firmware
compiles the source list directly with the target's flags.

The descriptor array and its strings must have static lifetime. The catalog
allows at most eight unique IDs (1..15 uppercase letters, digits or underscores).
Versions use 1..63 ASCII letters, digits, underscores, hyphens, dots or slashes. These bounds keep
wire messages and manifest fields bounded and JSON-safe. Invalid descriptors
make the entire catalog unavailable. Constructors must check the supplied buffer
size and alignment before placement construction, and return null on failure.

Each encoder fits caller-owned storage of at most 128 KiB aligned to 32 bytes.
No heap allocation, exceptions, RTTI or global mutable encoder state is needed.
Implement `configure`, `read_float`, `total_frames`, `status`, `stop` and the
legacy PCM16 reader from `waveform_source::Encoder`. Production uses normalized
float and quantizes once to PCM24; do not introduce PCM16 in that path. The
payload source and workspace must outlive the job. Declare the exact frame count,
honor every read capacity, reject unsupported profiles, and fail on premature
payload EOF. The WAV owner checks failures before publishing completed output.

`legacy_default` is opt-in and unique. It routes old `GENERATE:rate:interleave`
and DD008 upload commands to an adapter supporting those profiles. Explicit
`ENCODE ID PROFILE ...` always resolves that ID; unknown IDs never fall back.
Profile syntax and modulation details belong to the module. This interface is a
source contract, not a stable binary ABI: rebuild plugins with the project.

The public suite covers an empty catalog, example PCM24 generation, payload
failure, invalid profiles, workspace construction, disabled/read-only requests,
and artifact-owner I/O failures using a test adapter. Add modulation-specific
tests inside each module. Host tests and target linking do not establish analog
performance or standards conformance.

## Plain-language view and worked example

The player is a record player with effects knobs. A plugin is another way to
make a record. Removing that record-making attachment leaves the player and its
knobs working. For the example, three payload bytes make 30 ms of tone; the shared
writer adds its one-second tail. The result contains 49,440 PCM24 frames and can
be loaded, mixed with CW, or rendered again like any other compatible WAV.

## Custom controls and module tutorial

The [module-author tutorial](custom-encoder-tutorial.md) walks through copying the
example, defining custom profile parameters, controlling an encoder over CDC,
and extending the firmware/host parsers when a new command is needed.

## Licensing

Module boundaries organize source and builds; they do not change the licenses of
the code being combined. Preserve upstream notices and review distribution terms
when distributing a combined program. Keeping local modifications private does
not itself require publishing them; see the [GNU GPL FAQ](https://www.gnu.org/licenses/gpl-faq.en.html#GPLRequireSourcePostedPublic).
