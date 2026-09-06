# Phases 4–5 implementation and software validation

Implemented in the standalone waveform-generator checkout on 2026-09-06.
The earlier PR 1 fixes are retained. Parent M110 files were read for explicitly
authorized one-time imports; they are not build inputs and were not edited.

## Resulting workflow

Phase 5 is an artifact-creation utility. A PC may place a WAV directly on SD,
generate it with the local positional TX utility, or stage a BIN and request
on-board generation. The existing DD008 data/control transport also supports
uploading payload bytes before `CMD:SENDBUFFER` creates the WAV. Every successful
generation retains payload identity, a WAV and a hash-bearing manifest.

Phase 4 plays that retained WAV through a fixed scenario and optional live
controls. CW, static, fades and stepped sweeps are acknowledged at exact output
sample positions, exported and rendered offline for repeatable comparisons.
The encoder is outside the live playback chain.

The generic encoder/PCM interface and WAV writer accept alternate encoders.
M110-specific mode parsing and DSP stay in the M110 adapter; only the composition
catalog registers its factory. A fake encoder test proves the writer can link
and operate without the M110 implementation. M110B is currently the sole real
encoder; no M110D/JT8 implementation is claimed.

Usage and contracts are in [Phase 4](phase4-live-control.md) and
[Phase 5](phase5-artifact-generator.md). `WFG-LIVE/1` is an engineering control
protocol; the older frozen `WFG/1` qualification contract remains separate.

## Validation performed

| Check | Result |
| --- | --- |
| Host Release configure/build and complete CTest suite | 20/20 passed. |
| Portable controller and replay | Semantic levels/phase/sweeps, random-stream independence, bounded queue errors and PCM identity across different block sizes. |
| Actual player/CDC code with host I/O stubs | Playback reuse, STOP and fault cleanup, source measurement/rewind, rejected scenarios, partial USB writes, sequence validation and disconnect handling passed. |
| Actual TX USB adapter with host endpoint/owner stubs | DD008 handshake, binary-safe chunks, owner acknowledgements, backpressure, mode propagation and reconnect readback passed. |
| Actual SD artifact owner with in-memory FatFs | Upload and existing-BIN generation, bounded steps, hashes, existing-file protection, disconnect/reset and injected file/read/write/sync/publication failures passed. |
| M110 adapter | All 13 supported mode combinations match the imported whole-message host oracle; multi-block and irregular read sizes are covered. |
| Retained clean 600L reference | Native utility reproduced the complete 5,808,000-frame WAV byte-for-byte. |
| Existing MIX-003 / MIX-005 host artifacts | WAV bytes and stream digests remain unchanged. |
| RT1170 player Release | Linked successfully: DTCM 199,240 / 262,144 bytes (76.00%); OCRAM2 270,752 / 524,288 bytes (51.64%). |
| RT1170 read-only inspection Release | Linked successfully; symbol inspection confirms no FatFs write/rename, encoder or WAV-writer functions in the ELF. |
| Import provenance | Local source boundary test passed; exact donor/local hashes and selected-file state retained in `source-imports.json`. |
| RT1170 complete build-input audit | Passed with zero violations across 101 translation units, 99 dependency files and 119 linked map inputs. |
| Host complete build-input audit | Passed with zero violations across 56 translation units, 57 dependency files and 466 linked map inputs. |

The boundary auditor now distinguishes declared linker outputs, CMake utility
placeholders and in-memory MinGW PE linker records from physical input files.
Containment and real-file checks remain enforced; 18 internal audit regressions
passed. GNU host targets retain link maps for the complete build-input audit.

The clean reference WAV SHA-256 is
`9d45feabbe425bebeb7174ebd0586a6ef21348ce02fb0acc888fa896d6d44567`.
The unchanged MIX-003 and MIX-005 output digests are respectively
`4d8958d7f45d9d20` and `d2146c161d5d44a8`.

The complete suite includes 20 live-controller host tests, eight USB uploader
tests, 12 renderer integration tests, the generic-writer and native-oracle
tests, three firmware-owner/transport harnesses and the earlier contract,
capture, staging and signal-lab regressions. Three optional parent-reference
Python tests remain intentionally skipped in standalone default testing.

Python's Windows Store runtime encountered sandbox temporary-directory ACL
failures. The complete successful CTest run used the approved unsandboxed
execution path, with TEMP/TMP confined to this repository's `build/phase45-temp`.
No product behavior was changed to bypass that environment failure.

Retained local evidence includes `build/phase45-tests-final.log`,
`build/phase45-rt-build.log`, `build/phase45-readonly-build.log`,
`build/phase45-readonly-symbols.txt`, `build/phase45-portable-compatibility.log`,
and `build/native-m110/saved-reference-comparison.json`. Build evidence is local
and ignored; it must be retained explicitly when packaging a test record.
The complete boundary audits are retained in `build/phase45-rt-boundary.json`
and `build/phase45-host-boundary.json`. Final binary, manifest and evidence hashes
are indexed in `build/phase45-build-evidence.json`.

## Limits and hardware follow-up

The normal image now permits owner-task FatFs writes. The inspection image
retains a compile-time read-only FatFs profile, excludes the artifact writer
and refuses creation requests. Existing completed files are never overwritten
by the board utility. Failure may leave temporary files; publication is not a
power-loss transaction across a pair of FAT directory entries.

The USB task stack is 2048 words and the player task remains 4096 words. The
live controller and schedules use fixed capacities; the encoder and writer
workspaces reside in OCRAM2 beside the output FIFO. New memory sizes come from
the linker, not measured stack high-water marks.

No new firmware was flashed and no hardware was operated for this implementation.
On-target generation, SD durability, USB traffic under audio load, timing/stack
headroom, live replay digests and analog decoding need a new hardware run.
The imported transmitter keeps its original libm-based modulation math; host
oracle identity does not imply native ARM/host WAV byte identity. Retaining the
actual board-created WAV makes it available for direct hash and decode checks.

The existing linker warning about an RWX LOAD segment remains. These software
checks and cross-builds do not establish formal M110 conformance or satisfy the
separate one-hour qualification gate.
