# Watterson stage: validation method and acceptance

`signal-lab/validation/run_watterson_validation.py` is the host validation entry point. It measures the Watterson renderer against the built host pathsim chain and checks the criteria, statistical limits, determinism and channel coverage defined in [the design](watterson-stage.md). The runner writes rows, identities and a summary under the selected work directory; those generated results are not source documentation.

## Prerequisites

The default host build and unit tests do not need external reference data. The full cross-simulator battery is an optional external-data check: supply `--lane-root` with the separately obtained lane probes, analyzers, reference renders and decoder. Those materials are not included in this repository. Criterion 5 scoring with `--score-c5 <manifest>` needs only the manifest and its completed decode records.

A full criterion 5 comparison also requires the lane decoder to process the generated renders using the generated decode script. The runner cannot complete that comparison with the repository alone. `--quick` and selected-criterion runs are smoke or partial runs; they do not establish full coverage acceptance.

## Run the checks

From the repository root, build the host renderer and pathsim chain:

```powershell
cmake --preset host-release -B build/host-release
cmake --build build/host-release --target wfg_watterson_render signal_lab_render --parallel 8
cmake -S channel-sim -B build/pathsim-host -G "MinGW Makefiles"
cmake --build build/pathsim-host --parallel 8
```

Run the complete external comparison with a lane-data directory that you are authorized to use:

```powershell
python -B signal-lab/validation/run_watterson_validation.py --build build/host-release --pathsim-build build/pathsim-host --lane-root 'C:/reference-data'
```

Use `--quick` for a smoke run, or `--criteria 1,2,...` to select criteria. The default run includes all criteria and is lengthy. The full criterion 5 verdict depends on separately decoding its generated renders and scoring the resulting manifest. The `--score-c5` mode is for that scoring step.

Run repository-side validation tests with:

```powershell
python -B -m unittest discover -s signal-lab/validation -p "test_*.py"
```

Optional pathsim parity checks also require the external reference tree:

```powershell
python -B channel-sim/gates/gate0_reference_parity.py --build build/pathsim-host --lane-root 'C:/reference-data'
python -B channel-sim/gates/run_gates.py --build build/pathsim-host --lane-root 'C:/reference-data'
```

## Criterion 5 decode comparison

Criterion 5 is optional and needs decoded records from the separately supplied decoder. The base ladder has eight stage/pathsim comparisons: 2400L MPP at 18, 14 and 10 dB plus AWGN at 10 dB; 1200L MPP at 12 and 8 dB; and 600L MPP at 7 and 4 dB. The base run uses seeds 1–4 per simulator; rank-sum results with fewer than 12 seeds are informational only.

For the equivalent-SNR offset, each ladder needs at least three SNR points spaced 2 dB apart and at least four seeds per simulator at each point. The target is BER 1e-3; the allowed stage/pathsim offset is 0.5 dB for AWGN and 1.0 dB for fading. The extended threshold comparison uses 12 seeds per simulator at the listed three-point ladders. A rank-sum verdict uses the exact two-sided test at alpha 0.05/8 = 0.00625 and requires 12 seeds per simulator. D.6.2 static-channel rows are checked against BER <= 1e-5 at 50 dB; report the stage and pathsim verdicts separately.
## Acceptance boundaries

Judge each criterion only under its defined input, duration, analyzer and statistical rule in the design and runner. Smoke runs do not count as full acceptance. Keep results bounded to host rendering and the tested reference data. These checks do not establish board behavior, analog fidelity, modem conformance or receiver performance on a physical radio.

The Doppler truncation criterion retains an open interpretation: the selected 1% truncation satisfies the per-spur reading, while integrated power outside the main lobe exceeds the -60 dBc limit. Do not report the criterion as unconditional until the applicable interpretation is resolved. The runner and design document the measurement and the competing readings.
