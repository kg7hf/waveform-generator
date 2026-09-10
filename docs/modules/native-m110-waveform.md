# Native M110B Waveform Generator

## 1. Scope

This guide covers `native-m110/`, the project-local streaming M110B encoder used
behind the generic waveform-source interface. It explains payload framing, body
planning, FEC, interleave, symbol production, pulse shaping, and host/target use.

This is a transmit-only adapter. Decoder behavior and formal compliance results
belong to the external decoder and retained evidence, not this module.

## 2. 50,000-foot view

The native source turns payload bytes into an M110B serial-tone transmission:

```text
payload bytes -> LSB-first bits -> EOM/flush/padding -> FEC/repetition
-> interleave -> 8-PSK/probes -> preamble -> RRC shaping + 1800 Hz -> 48 kHz PCM
```

It keeps one information/interleave block and one encoded segment at a time, so
memory use is bounded even when the message lasts for minutes.

Supported command profiles are 75S/L, 150S/L, 300S/L, 600S/L, 1200S/L,
2400S/L, and 4800U. They are 13 explicit combinations, not an arbitrary
rate/interleave cross-product.

## 3. How to use it

Create a host artifact with a text payload:

```powershell
build\host-release\host\tx\wfg_m110_tx.exe 600 long hello.wav "HELLO WORLD"
```

Create one from binary bytes:

```powershell
build\host-release\host\tx\wfg_m110_tx.exe 1200 short payload.wav --file payload.bin
```

The program accepts rates `75`, `150`, `300`, `600`, `1200`, `2400`, and `4800`;
interleave is `short`, `long`, or `zero`. The selected pair must be a supported
M110B body mode. Output suffix chooses WAV or raw PCM.

Target generation selects encoder `M110B` and an opaque profile equivalent to
the rate/interleave pair. The caller supplies payload bytes, declared length,
encoder workspace, sink, and optional trailing silence.

## 4. Scientist and maintainer view

### Fixed signal geometry

The body symbol rate is 2400 baud, the carrier is 1800 Hz, and audio is 48 kHz.
There are exactly 20 audio samples per symbol. Pulse shaping uses a root-raised
cosine response and preserves carrier phase across bounded render windows.

### Framing and coding

User octets enter least-significant bit first. The framed information stream
adds the 32-bit EOM word `0x4B65A5B2`, a 144-bit coder flush, and block padding.
The K=7 rate-1/2 encoder is continuous across body blocks. The 300 and 150 modes
repeat coded pairs two and four times. The 75 mode adds its own orthogonal
spreading behavior. The 4800 mode is uncoded.

Resetting FEC state at every block would change the waveform and concentrate
errors at boundaries. The streaming source therefore retains encode state across
segments until the transmission flush.

### Bounded workspace

`native_m110::Source` owns fixed arrays for at most 11,520 information bits,
23,040 coded bits, an interleaver matrix, one segment, a shaping window, and a
480-frame render buffer. The RT1170 catalog reserves 128 KiB for an encoder.
Changes must account for alignment and actual target map use.

### Evidence boundary and debugging

Inspect the `BodyTransmissionPlan`, payload byte counts, fetched-byte count,
source status, total frames, generator hashes, and external decode result. A
self-generated waveform decoded by related code is strong regression evidence
but not automatically an independent conformance verdict.

## 5. 5th-grader view

The modem writes your message on many shuffled cards. It adds an "end" card,
makes backup clues for damaged cards, shuffles them so one burst of noise does
not ruin neighboring clues, and turns groups of bits into points around a circle.
Then it plays those points as a smooth 1800 Hz sound.

The receiver has to reverse those steps in the opposite order.

## 6. Toy worked example

The ASCII letter `A` is hexadecimal `0x41`. M110 payload bits enter LSB first:

```text
0x41 binary as normally printed: 01000001
bits entering the framer:         1 0 0 0 0 0 1 0
```

For a deliberately tiny toy block, imagine those eight bits followed by an EOM
marker and zeros until the block is full. The real encoder then produces two
coded bits per information bit for a rate-1/2 mode, interleaves them, groups the
channel bits into symbol decisions, inserts known probes, and prepends a mode
preamble. At 20 samples per symbol, ten transmitted symbols occupy 200 audio
samples or about 4.17 ms.

The toy block omits the standard block sizes and full 32+144-bit trailer so it can
fit on one screen. It explains the direction and ordering, not a compliance vector.

## 7. Code map and change checklist

| Responsibility | Files |
|---|---|
| Streaming adapter | `native-m110/source.hpp`, `native-m110/source.cpp` |
| Generic WAV adapter | `native-m110/wav_generator.*` |
| Whole-transmission planning | `native-m110/core/transmitter.*` |
| Burst framing and modulation | `native-m110/core/body_waveform.*` |
| Mode table | `native-m110/core/waveform.*` |
| FEC, interleave, whitening | `native-m110/core/fec.*`, `interleaver.*`, `scrambler.*` |
| Preamble and RRC taps | `native-m110/core/synchronization.*` |

When changing a mode, update the descriptor/designator tables, host and streaming
paths, supported CLI/profile list, all 13 combination tests as applicable, target
memory accounting, generated hashes, and independent decoder evidence.
