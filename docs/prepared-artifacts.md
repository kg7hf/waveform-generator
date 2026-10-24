# Inspecting prepared board artifacts offline

The RT1170 `PREPARE` operation publishes a completed WAV and JSON identity
manifest. The source WAV, scenario, and optional secondary WAV remain separate
inputs on the card; they are not copied beside the output. In particular, the
board refuses an output basename when a matching `.SCN` already exists, because
playback could otherwise apply the impairment scenario a second time.
`host/prepared_artifacts.py` verifies a copy of the WAV/manifest pair from an
ordinary local directory. It never searches removable drives, contacts the
board or reads USB. The files must first be copied to the
computer by a separate, authorized workflow.
Keep the copied directory and supplied inputs unchanged during verification;
the command does not lock other applications out of those files.

The manifest schema is `waveform-generator.prepared/2`. Version 1 artifacts
retain their original metadata but are not accepted by these version 2 tools.
A candidate is accepted
only when the manifest is complete, its retention state is `temporary` or
`keep`, and its identities are internally consistent. The supported output is
mono, 48 kHz, signed PCM24. The verifier checks the canonical RIFF header and
length, zero RIFF pad byte when present, output frame count, full WAV SHA-256,
and PCM SHA-256. It validates the declared source, scenario, secondary hashes,
and engine digest formats, but does not recompute the scenario hash or engine
digest from the output. The latter is a pre-quantization engine statistic and
cannot be derived from the saved PCM. `reuse` checks the original source,
scenario, and optional secondary hashes against the manifest. The manifest
also binds seed, input and output frame counts, firmware commit, and algorithm
version 1.

Use the commands from the repository root, substituting the expected firmware
commit from the intended preparation build:

```powershell
python host/prepared_artifacts.py list D:/captures/prepared
python host/prepared_artifacts.py verify D:/captures/prepared CASE01
python host/prepared_artifacts.py archive D:/captures/prepared CASE01 D:/archives/run01
python host/prepared_artifacts.py reuse D:/captures/prepared CASE01 `
  --firmware-commit CURRENT_FIRMWARE_COMMIT `
  --seed 17 `
  --source D:/inputs/SOURCE.WAV `
  --scenario D:/inputs/CASE.SCN `
  --secondary D:/inputs/SECOND.WAV
```

`list` validates every `.JSN` candidate and fails if any candidate is malformed
or lacks its matching `.WAV`; it does not silently omit damaged artifacts. It
also refuses an output basename with a neighboring `.SCN`. It returns a
bounded JSON listing. `verify` checks the WAV/manifest pair without requiring
the original input files. `reuse` requires the expected firmware commit, seed,
original source WAV, and original scenario. If a
secondary source was used, its original WAV is also required. Each supplied
input must match the manifest's filename and content hash. The source WAV's
format is not reparsed here; its exact content hash is bound to a manifest
created only after firmware-side WAV validation. The command reports
`reusable: true` only when those checks pass. It does not render, copy, rename,
or publish anything, and it does not decide whether an artifact is suitable
for a particular experiment.

Each output name is an upper-case 8.3 basename, such as `CASE01`, with exactly
one `.JSN` and `.WAV` member. The tool rejects path traversal, symlink or
junction components, case-colliding directory entries, malformed JSON, duplicate JSON
keys, non-finite JSON numbers, unsupported fields, hash mismatches, and
unexpected WAV data. Limits are 4,096 directory entries, 8 KiB per manifest,
8,191 bytes per scenario, and the maximum size representable by the supported
RIFF layout. Inspection commands only read files; no existing artifact is
deleted or overwritten by any command.

## Archiving a copied output pair

`archive DIRECTORY NAME DESTINATION` verifies the source pair, then creates a
new destination directory beneath an existing parent, outside the source
directory. It refuses an existing
destination, even an empty one. It copies the WAV and original JSN bytes in
bounded blocks, checks their identities, flushes and synchronizes the files,
and verifies the copied pair before reporting success. Both `temporary` and
`keep` outputs can be archived; their original retention field and preparation
identity remain unchanged. The source pair is never moved or modified.

An `INCOMPLETE.json` marker remains in the destination until verification
succeeds. `list`, `verify`, `reuse` and further archive attempts refuse a source
directory with that marker, including a differently cased spelling. Failed
copies retain their partial files and marker for inspection. Use a new
destination for another attempt; there is no automatic cleanup or resume.
Do not remove the marker to promote a failed copy. Keep the source and
destination exclusive to this operation while it runs. These filesystem checks
do not lock out another writer or guarantee directory-entry durability across
a sudden power loss.

This command archives only the already rendered WAV and its board identity
record. It does not copy the original source, scenario or secondary inputs,
change the board manifest into the `retain_waveform.py` archive format, or
establish rerender reproducibility. Keep those inputs and their provenance
separately; `reuse` still requires their original names and matching hashes.
There is no board discovery or automatic cache-selection workflow. A matching
manifest is evidence of identity, not proof of independent analog, protocol or
playback acceptance.

## Comparing a board preparation with the C++ reference

`host/compare_prepared.py` compares a copied board pair with a verified
`retain_waveform.py` render directory. It only reads existing local files.
Create the reference with the portable C++ Signal Lab renderer using the
same clean source, exact scenario bytes and effective seed as the board job:

```powershell
python host/retain_waveform.py prepare `
  --renderer path/to/signal_lab_render.exe `
  --source D:/inputs/SOURCE.WAV --scenario D:/inputs/CASE.SCN `
  --seed 17 --retention archive --output-dir D:/references/case01
python host/compare_prepared.py D:/captures/prepared CASE01 D:/references/case01 `
  --firmware-commit CURRENT_FIRMWARE_COMMIT `
```

Use the actual board build identities for the two required identity options.
Keep the renderer executable and its build provenance with the experiment;
the retained reference records its executable SHA-256. The comparison checks
recorded provenance, not the authenticity of how those records were produced.
Neither original executable has to run during the comparison.

Both artifacts must pass their own validation first. Source SHA-256, exact
scenario SHA-256, effective seed and optional secondary-source SHA-256 must
agree before outputs are compared. A semantically equivalent but reformatted
scenario is a different input for this check. Input filenames and retention
choices can differ across the two artifact formats; their content identities
must agree. A reference with multiple secondary sources is not supported by
the board pair format.

The JSON report uses schema `waveform-generator.prepared-comparison/2` and
reports separate frame-count, whole-WAV SHA-256 and PCM SHA-256 checks.
Exit status 0 means all three match, 1 means valid comparable artifacts have
different output, and 2 means an invalid artifact, expected-build mismatch or
incompatible input identity prevented comparison. Incomplete directories are
refused. Reference manifest, renderer-report and scenario JSON files are each
limited to 1 MiB before parsing. Keep both directories unchanged while the
command runs.

The board's engine digest describes pre-quantization samples; the host
retained digest describes packed PCM. Those two FNV values are deliberately
not compared. Exact saved-file parity does not establish physical playback,
analog fidelity, sustained rendering speed or standards acceptance.

This is the same-engine generation check in the
[testing guide](testing.md#prepared-waveform-comparisons). PathSim uses a separate
implementation and random process; its fading/noise outputs are not expected
to match these files byte-for-byte. PathSim agreement still requires matched
channel conditions and modem-result comparisons across independent runs.
