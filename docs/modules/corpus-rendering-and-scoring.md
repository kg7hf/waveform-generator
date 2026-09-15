# Corpus Rendering and Decoder Scoring

## 1. Scope

This guide covers the corpus specification, reference generation,
scenario validation and rendering, external decoder scoring, summaries, and
deterministic re-render verification.

## 2. 50,000-foot view

A corpus is a reproducible experiment matrix. It starts from perfect references,
proves those references decode exactly, derives impaired WAVs from declared
scenarios, scores each WAV with named decoder configurations, and publishes
hash-bearing manifests and summaries.

```text
spec -> payloads + perfect references -> reference gate -> impaired WAVs
-> decoder results -> CSV/JSON summaries -> deterministic verification
```

The orchestrator coordinates tools; it does not hide their identities or convert
a failed prerequisite into a skipped success.

## 3. How to use it

Plan without producing files:

```powershell
python host/corpus.py --stage plan
```

Run the complete pipeline with explicit producer and decoder executables:

```powershell
python host/corpus.py `
  --spec corpus/default-corpus.json `
  --tx ..\..\build\host-windows-release\tools\m110_tx_to_pcm.exe `
  --decoder ..\..\build\host-windows-release\tools\m110_app_decode.exe `
  --engines siso,adaptive --jobs 4 --stage all
```

Re-render and compare retained hashes:

```powershell
python host/corpus.py --stage verify
```

Score ad hoc WAVs:

```powershell
python host/m110_score.py --decoder path\m110_app_decode.exe `
  --engine siso --jobs 4 --summary build\runs\scores input1.wav input2.wav
```

Existing outputs are reused unless `--overwrite` is explicit. Reuse includes
identity checks; a filename alone is insufficient.

## 4. Scientist and maintainer view

### Stage gates

| Stage | Required result |
|---|---|
| `refs` | Payload and perfect reference artifacts with hashes |
| `validate` | Exact expected payload, EOM, and BER 0 before deriving impairments |
| `render` | Scenario-bound WAV and sidecar for every planned case |
| `score` | Decoder identity, engine, output, byte counts, and error metrics |
| `summary` | Corpus manifest plus machine-readable CSV/JSON aggregates |
| `verify` | Fresh temporary re-render matches every retained PCM hash |

The `all` stage follows that order. A bad perfect reference blocks dependent
cases. Do not reinterpret an impaired decoder failure if the same-gain control
or perfect-reference gate is missing.

### Experiment identity

Retain spec hash, payload hash, source WAV/PCM hash, scenario hash, seed, renderer
identity, sidecar, decoder executable identity, decoder arguments, expected bytes,
actual bytes, EOM result, missing/extra data, and score output.

Fixed-offset byte comparison should report missing and extra bytes separately.
Silent realignment can hide timing/sample-slip defects.

### Parallelism and reproducibility

`--jobs` changes process concurrency, not scenario seeds or output identity.
Random stages must be deterministic per scenario/stage. Summaries must be sorted
by declared matrix identity rather than task completion order.

Corpus results are engineering evidence for the listed sources, scenarios,
decoder, and seeds. A single deterministic realization is not a statistical
channel qualification campaign.

## 5. 5th-grader view

Imagine testing umbrellas. First you check that every umbrella works with no rain.
Then you use a recipe to spray light rain, heavy rain, and wind. You label every
umbrella and every spray recipe, record the result, and later repeat the spray to
make sure it was the same test.

If an umbrella already leaks with no rain, you stop. It would be unfair to blame
the wind machine.

## 6. Toy worked example

Suppose a tiny spec has two modes and two impairment cases:

```text
modes: 600L, 1200S
cases: clean, CW-6dB, FADE-20dB
```

The matrix contains six WAVs: two perfect references and four impaired derivatives.
If both `siso` and `adaptive` engines score every WAV, there are twelve decoder
result records. A summary row should identify at least mode, case, seed, WAV hash,
engine, expected bytes, received bytes, EOM, and bit/byte errors.

If `1200S clean` fails its exact reference gate, both dependent `1200S` impaired
cases are blocked rather than scored as interference failures.

## 7. Code map and change checklist

| Responsibility | Files |
|---|---|
| Matrix/spec | `corpus/default-corpus.json` |
| Orchestration | `host/corpus.py` |
| Python rendering | `host/render_scenario.py`, `host/signal_lab/` |
| Decoder scoring | `host/m110_score.py` |
| Schemas | `schema/` |
| Retained outputs | `test-vectors/simulated-interference/`, `manifests/` |

New campaign dimensions need explicit IDs, bounded counts, deterministic seed
rules, source/control arms, summary columns, verification behavior, and a statement
of whether generated WAVs or only recipes/manifests belong in version control.

## 8. Maintainer walkthrough and format migration

`plan` expands the spec into identified mode/case/seed jobs without a decoder.
`refs` invokes the explicit external transmitter and binds payload/producer
identity. `validate` checks clean-reference decode before dependent impaired
cases; `render`, `score`, `summary` and `verify` retain each layer's evidence.
Do not count a rejected clean reference as an impairment-induced failure.

The Python renderer now emits PCM24. When the historical external reference
producer returns PCM16, `m110_reference.py` retains those original bytes in a
`.producer-pcm16.wav` artifact and promotes samples to PCM24. The reference
sidecar records the original identity and conversion; promotion does not add
source precision. Old PCM16 corpus hashes remain historical evidence and cannot
be compared directly with new PCM24 container/data hashes.

The generator package version/hash and explicit encoding distinguish runs.
Regenerate and revalidate a campaign when changing encoding, generator or score
rules; do not relabel existing results. Test planning, scoring parsers, exact
payload/EOM gates and regeneration using the host suites; full external-decoder
campaign execution is a separate integration operation.
