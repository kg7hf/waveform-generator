# Wideband noise spur validation method

`signal-lab/validation/run_wideband_validation.py` is the host-only check for the 6, 9 and 12 kHz AWGN output bands. Its default acceptance campaign renders three five-hour mono PCM24 silence records sequentially, with a -12 dBFS input reference, 10 dB SNR and seed 1. Shorter runs are smoke checks and remain inconclusive.

From the repository root, build the host renderer and run the campaign in a new output directory:

```powershell
cmake --preset host-release -B build/host-release
cmake --build build/host-release --target wfg_watterson_render --parallel 8
python -B signal-lab/validation/run_wideband_validation.py --build build/host-release --work build/host-release/work/wideband-validation
```

The runner locates the renderer in the build by default. `--renderer` can select a different build; its binary hash enters the result identity. The default abort marker is `build/main/bringup/AB_HOLD`; add other shared markers with repeatable `--hold-marker` arguments. A marker is checked at render and analysis boundaries and does not interrupt a step already running. `--resume` reuses only a matching campaign identity.

Acceptance requires the full duration, reported line and harmonic-fold detector floors below -70 dBc, no detected level above -70 dBc, and the injected -68 dBc control within 1 dB. Consult the runner for the exact statistical model and output fields. Passing this test covers the declared digital AWGN renders only; it does not establish analog noise, receiver performance, modem BER, board parity or full channel conformance. The host test suite includes `tests/test_wideband_profiles.py`, `tests/test_wideband_snr.py` and `tests/test_wideband_response.py` for profile, level and response contracts.
