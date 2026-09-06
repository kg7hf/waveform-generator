# PR 1 review: phased M110B test-waveform and dynamic interference generator

Date: 2026-09-06  
Original review disposition: **Request changes**  
Current disposition: **All ten findings addressed; offline validation passed**  
Scope: Phases 1–3 in the standalone waveform-generator repository, nested under `M110-modem/tools/waveform-generator`.  
Reviewed range: `1b5aa0d..dc27cf6`  
Reviewed HEAD: `dc27cf62ee22718d0d3535461c96beafc2751bb3` on `main`.

The original review found **one P1 and nine P2 issues**. All ten have been addressed in the working tree, along with all four valid Copilot comments on [PR #1](https://github.com/kg7hf/waveform-generator/pull/1). Two Copilot comments duplicate the original findings; the other two concern inconsistent slip limits and diagnostics. No commit, push, or GitHub review-thread resolution was performed.

The finding descriptions and source locations below preserve the evidence from the reviewed commit. Each resolution describes the corrected behavior. Historical hardware results remain evidence for the original binary; the corrected firmware has been cross-built but not run on a board.

## Findings

### 1. [P1] Validate available history before duplicating samples

Location: [signal-lab/src/sample_slip.cpp](https://github.com/kg7hf/waveform-generator/blob/dc27cf62ee22718d0d3535461c96beafc2751bb3/signal-lab/src/sample_slip.cpp#L121), lines 121–126.

A preceding deletion can leave less delivered history than a duplicate requires. Deleting 499 samples at frame 0, then duplicating 400 at frame 500, passes configuration but reaches `history_[0 - 399 + index]`.

Repeated identical host runs emitted different digests and reported a **+424.58 dBFS** internal peak. Both lengths are within the documented 1024-sample limit.

Check available delivered history before subtracting and reject or explicitly handle an unsatisfiable duplicate.

**Resolution — addressed:** Configuration now models delivered history after prior deletions and rejects a duplicate whose required history is unavailable. The runtime path also checks actual history before subtraction. Regression coverage includes the original delete-499/duplicate-400 case and direct-stage defensive handling.

### 2. [P2] Reserve output capacity for the remaining source samples

Location: [signal-lab/src/sample_slip.cpp](https://github.com/kg7hf/waveform-generator/blob/dc27cf62ee22718d0d3535461c96beafc2751bb3/signal-lab/src/sample_slip.cpp#L114), lines 114–118.

The duplicate admission check reserves space for the output prefix and duplicate, but omits the remaining input. `copy_input` then silently discards source samples when capacity is exhausted.

For 8192 source frames and a 400-sample duplicate at frame 480, 2048-frame blocks produce **8448 frames instead of 8592**, while reporting zero dropped events; 100-frame blocks produce the correct length.

Preserve pending output or reject unsupported expansion before consuming input.

**Resolution — addressed:** Configuration conservatively limits cumulative duplicate expansion to 256 frames in every possible 2048-source-frame window, independently of caller block size. Larger or denser schedules are rejected before input is consumed. The runtime admission check reserves space for all remaining source samples. Tests cover rejection of the original 400-frame case, clustered overflow, accepted 256-frame expansion, and duplicates exactly 2048 source frames apart. The 1024-frame deletion limit and existing fixed buffers are preserved.

### 3. [P2] Apply the fade capacity limit to concurrent events

Location: [signal-lab/src/fade.cpp](https://github.com/kg7hf/waveform-generator/blob/dc27cf62ee22718d0d3535461c96beafc2751bb3/signal-lab/src/fade.cpp#L78), lines 78–87.

All events starting within a block are admitted before any are processed or retired, so the eight-active-fade limit also drops nonoverlapping events.

A scenario with 32 one-millisecond rectangular fades spaced two milliseconds apart applies **8 and drops 24** with 2048-frame blocks; 100-frame blocks apply all 32 and produce a different digest.

Advance admission and retirement by event time so results remain independent of block size.

**Resolution — addressed:** Fade processing advances between event boundaries, retiring completed fades before admitting new ones. The eight-slot limit now counts simultaneous fades. Tests verify all 32 nonoverlapping fades and true overlap saturation, including equal-time ends/starts, across different block sizes.

### 4. [P2] Enforce `clip_policy: reject` in the portable engine

Location: [signal-lab/src/mixer.cpp](https://github.com/kg7hf/waveform-generator/blob/dc27cf62ee22718d0d3535461c96beafc2751bb3/signal-lab/src/mixer.cpp#L158), lines 158–174.

The parser accepts `clip_policy: reject` and configuration records it in `saturate_`, but processing never reads that flag and always clamps.

With `source_gain_db: 20` and no impairment stages, the host renderer successfully writes a WAV containing **8192 clipped samples** despite the rejection policy.

Propagate a clipping failure, or reject this unsupported policy during configuration, rather than silently changing its meaning between Python and C++.

**Resolution — addressed:** The engine now latches ProcessError::clipping and returns zero output for the offending block; later calls consume nothing until reconfiguration. Consumed-source and clipping diagnostics remain available, while output frame totals, digest, peak, and energy do not include the rejected block. The host renderer returns failure before writing artifacts; run_pipeline propagates failure; the RT1170 stops through its existing fault path and reports engine_state=3, engine_error=7, player error 16. Tests cover failure latching, reconfiguration, valid zero-output deletion, pipeline propagation, and preservation of pre-existing host artifacts.

### 5. [P2] Measure output levels from the emitted PCM

Location: [host/signal_lab/render.py](https://github.com/kg7hf/waveform-generator/blob/dc27cf62ee22718d0d3535461c96beafc2751bb3/host/signal_lab/render.py#L126), lines 126–129. The same issue exists in the [C++ mixer](https://github.com/kg7hf/waveform-generator/blob/dc27cf62ee22718d0d3535461c96beafc2751bb3/signal-lab/src/mixer.cpp#L151) and [host renderer](https://github.com/kg7hf/waveform-generator/blob/dc27cf62ee22718d0d3535461c96beafc2751bb3/host/render/signal_lab_render.cpp#L344).

Peak and energy are accumulated before saturation, then published as output WAV measurements.

The retained 600L severe-impulse sidecar reports **+17.7967 dBFS peak and −3.9374 dBFS RMS**; its actual PCM measures **0 dBFS peak and −7.72885 dBFS RMS**. The C++ mixer and renderer have the same issue.

Compute output metrics from quantized PCM, retaining separately named pre-clipping metrics if useful.

**Resolution — addressed:** Both renderers calculate output peak and RMS from emitted, quantized PCM16. The severe-impulse Python reproduction now reports 0 dBFS peak and -7.728851 dBFS RMS without changing its PCM hash. Re-rendered C++ MIX-005 reports 0 dBFS peak and -16.430161 dBFS RMS with unchanged WAV bytes. Python and C++ renderer metadata versions are 0.1.1; historical manifests remain unchanged.

### 6. [P2] Validate the reference WAV identity before reusing its score

Location: [host/corpus_phase1.py](https://github.com/kg7hf/waveform-generator/blob/dc27cf62ee22718d0d3535461c96beafc2751bb3/host/corpus_phase1.py#L181), lines 181–184.

Reference sidecars store their hash under `wav.sha256`, but this cache check reads only `output.sha256` and defaults to the cached score's own hash.

After regenerating a reference, the default score stage can therefore reuse its previous result whenever the decoder is unchanged. A reproduction with changed WAV contents and updated reference metadata returned the old score without calling `score_one`.

Handle both sidecar schemas and compare the current artifact identity.

**Resolution — addressed:** Score reuse hashes the actual WAV and compares that identity with the cached score, independent of sidecar schema. score_one also records the actual input-file hash. Tests cover unchanged input reuse, regenerated references and impaired WAVs, and replaced files with stale sidecars.

### 7. [P2] Fail corpus verification when expected sidecars are missing

Location: [host/corpus_phase1.py](https://github.com/kg7hf/waveform-generator/blob/dc27cf62ee22718d0d3535461c96beafc2751bb3/host/corpus_phase1.py#L264), lines 264–266.

Missing sidecars are silently skipped. Running the documented verify stage against an absent corpus with the normal 135-case plan prints:

```text
verify: 0 scenarios re-rendered, 0 mismatches
```

The command exits zero. Partial corpora likewise pass without checking their missing cases.

Treat missing expected verification inputs as failures and require the checked count to match the selected plan.

**Resolution — addressed:** Verification records missing sidecars as failures, includes planned and checked counts in its summary, and rejects empty plans. Tests cover complete, partial, missing, mismatched, and empty inputs; the absent-corpus command now returns nonzero. The existing corpus also passed a complete 135/135 regeneration with zero PCM mismatches.

### 8. [P2] Give additional AWGN stages independent random streams

Location: [host/signal_lab/awgn.py](https://github.com/kg7hf/waveform-generator/blob/dc27cf62ee22718d0d3535461c96beafc2751bb3/host/signal_lab/awgn.py#L70), line 70.

Every AWGN instance uses stream index zero even though multiple AWGN stages are accepted. Two equal-level stages consequently generate identical samples and increase noise power by **6.0206 dB rather than approximately 3 dB** for independent sources.

Preserve the first stream's `pcm_stress` compatibility if required, but assign independent streams to subsequent instances or explicitly reject multiple AWGN stages.

**Resolution — addressed:** AWGN uses its zero-based AWGN-instance index for stream selection. The first instance remains stream zero regardless of its position in the pipeline, preserving existing corpus samples; additional instances receive independent streams. Resolved metadata records the selected index. Tests verify legacy first-stream identity and independent equal-level noise addition. Full corpus verification found no PCM changes.

### 9. [P2] JSON-escape strings written into render sidecars

Location: [host/render/signal_lab_render.cpp](https://github.com/kg7hf/waveform-generator/blob/dc27cf62ee22718d0d3535461c96beafc2751bb3/host/render/signal_lab_render.cpp#L340), lines 340–341.

Paths and `test_id` are interpolated directly into JSON string literals. Ordinary Windows paths containing backslashes make the generated sidecar invalid: an absolute `D:\source\repos` path reproduced an invalid-escape JSON parse failure despite renderer exit code zero. Some backslash sequences instead silently alter the recorded path.

Use JSON string escaping for all emitted string values.

**Resolution — addressed:** The renderer JSON-escapes paths and preserves the original semantic value of test_id strings. Integration tests parse sidecars containing native Windows paths, quotes, backslashes, control characters, and Unicode. Metadata write failures now return nonzero.

### 10. [P2] Export the effective reference RMS into the target scenario

Location: [host/render/signal_lab_render.cpp](https://github.com/kg7hf/waveform-generator/blob/dc27cf62ee22718d0d3535461c96beafc2751bb3/host/render/signal_lab_render.cpp#L318), lines 318–322.

The host honors `--reference-rms`, but target export leaves an existing `reference_rms` untouched. With an input value of `0.3` and CLI override `0.1`, the exported `PLAY.SCN` still contains `0.3`.

A CW reproduction produced host digest `2997394dfe156a7b`, while the exported scenario produced `65e6e7d07d89bc43`.

Replace the existing top-level field with the effective value so the target scenario reproduces its paired host render.

**Resolution — addressed:** Target export uses parsed top-level members, replacing the existing reference_rms with the effective value at 17 significant digits. Nested members, string contents, and escaped key spellings cannot fool replacement. Tests verify identical target-replay WAV bytes and digests after an override. CLI overrides must be finite, positive, and fully numeric.

## Original review validation

| Check | Result |
|---|---|
| `cmake --preset host-release` | Configured successfully |
| `cmake --build --preset host-release --parallel` | Passed |
| `ctest --preset host-release` | 8/8 passed |
| `python -m unittest tests.test_signal_lab` | 16/16 passed |
| `python tests/test_contracts.py --root .` | Passed |
| `cmake --build --preset rt1170-player-release --parallel` | Linked; DTCM 67.85%, FIFO 128 KiB in OCRAM2 |

Retained logs reconcile the **six completed playbacks**, matching host/source digests, zero underruns, and the reported final FIFO measurements. These results support the tested scenarios; they do not cover the boundary cases reproduced in the findings above.

The main C++ reproductions are retained locally in `build/review-engine/repro.py`. This is an ignored build artifact, not a tracked dependency of the review.

## Original review boundaries

- No source files were changed during the review; this document records the completed review.
- No hardware was accessed. Hardware conclusions came from retained artifacts.
- The full 135-file corpus regeneration was not rerun.
- The documented one-PLAY-per-boot behavior and 1024-sample slip-run cap were treated as declared limitations, not review defects.
- The pre-existing untracked phased-implementation prompt and unrelated parent-repository changes were preserved.


## Copilot comment assessment and disposition

All four comments were submitted against the reviewed `dc27cf6` commit and are valid. The fixes below are local; the comments have not been replied to or marked resolved on GitHub.

| Copilot comment | Assessment | Disposition |
|---|---|---|
| [3944738219: duplicate history underflow](https://github.com/kg7hf/waveform-generator/pull/1#discussion_r3944738219) | Valid; duplicates finding 1. Earlier deletions can invalidate the history requirement. | Addressed by delivered-history preflight and the runtime guard. |
| [3944738240: ignored clipping rejection](https://github.com/kg7hf/waveform-generator/pull/1#discussion_r3944738240) | Valid; duplicates finding 4. The parsed policy was not enforced. | Addressed by latched processing failure and propagation through all callers. |
| [3944738256: parser/engine slip-limit mismatch](https://github.com/kg7hf/waveform-generator/pull/1#discussion_r3944738256) | Valid consistency issue. The portable parser admitted 48000-sample runs while the engine capped runs at 1024. | Parser and engine now share max_slip_length=1024. The parser validates integer range before conversion. Python retains its separately documented offline capability; portable duplicate schedules also undergo the output-expansion budget check. |
| [3944738270: incorrect limit in diagnostic](https://github.com/kg7hf/waveform-generator/pull/1#discussion_r3944738270) | Valid. The engine's error incorrectly advertised 4096. | Corrected to 1..1024, with boundary and invalid-length regressions. |

## Fix validation

Validation applies to the uncommitted fixes on top of `dc27cf62ee22718d0d3535461c96beafc2751bb3`, exclusively in this waveform-generator repository.

| Check | Result |
|---|---|
| Host Release configure and build | Passed |
| `ctest --preset host-release --output-on-failure` | 9/9 passed, including the new renderer integration suite |
| Host Debug configure/build and `ctest --preset host-debug --output-on-failure` | Passed; 9/9 tests passed |
| `python -m unittest tests.test_signal_lab` | 21 discovered: 18 passed, 3 explicit parent-reference skips |
| Renderer artifact integration tests | 7 passed |
| `python tests/test_contracts.py --root .` | Passed |
| `python host/corpus_phase1.py --stage verify` | 135/135 re-rendered, 0 PCM mismatches, 0 missing sidecars |
| MIX-003 host replay against retained hardware-run input | WAV bytes unchanged; output digest 4d8958d7f45d9d20 |
| MIX-005 host replay against retained hardware-run input | WAV bytes unchanged; output digest d2146c161d5d44a8 |
| RT1170 player Release build | Linked; DTCM 177872 bytes (67.85%), FIFO 128 KiB in OCRAM2, flash text 114212 bytes |

Both portable replays also preserve source digest `b8168de6c8edeb95`, frame counts, clipping counts, and event counts. They verify compatibility of the corrected host engine with the retained inputs and outputs; they are not new hardware runs.

The three parent-reference tests are now explicitly opt-in through `WFG_PARENT_REFERENCE_TESTS=1`. They were skipped to keep this task inside the selected repository. Standalone numerical, corpus, and artifact tests ran. The Windows Store Python runtime encountered temporary-directory ACL errors under the sandbox; the same tests passed under approved execution outside that sandbox, with TEMP/TMP directed inside the repository.

Local logs and compatibility results are retained under `build/review-pr-1/` and `build/python-review-fixes/`. These ignored working artifacts supplement this record and are not a sealed qualification package. The source-import ledger's m110-025 local hash and size were refreshed without changing historical donor identity.

No board was opened, reset, flashed, or run. Runtime clipping-fault behavior on the RT1170 is implemented and cross-build checked, but new on-device timing and execution evidence remain unmeasured. Earlier valid audio may already have played before a later block is rejected; playback is streaming and does not pre-scan the whole source.
