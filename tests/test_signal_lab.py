#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Unit tests for the waveform-agnostic Python signal_lab effects engine.

Run from the waveform-generator root:  python -m unittest tests.test_signal_lab -v
"""

import math
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "host"))

from signal_lab import FS  # noqa: E402
from signal_lab.awgn import (NOISE_BAND_TAPS, SNR_REFERENCE_BANDWIDTH_HZ, Awgn, BandNoise, filter_kernel,  # noqa: E402
                             noise_band_enbw_hz, noise_band_kernel, noise_sigma)
from signal_lab.cw import Cw  # noqa: E402
from signal_lab.fade import Fade  # noqa: E402
from signal_lab.impulse import Impulse  # noqa: E402
from signal_lab.render import render  # noqa: E402
from signal_lab.rng import FAMILY_BACKGROUND_NOISE, generator  # noqa: E402
from signal_lab.sample_slip import SampleSlip  # noqa: E402
from signal_lab.scenario import ScenarioError, validate  # noqa: E402
from signal_lab.stream import Pipeline  # noqa: E402
from signal_lab.wav_io import read_float, wav_info, write_pcm16  # noqa: E402

def synthetic_signal(seconds=4.0, seed=3):
    """A band-limited pseudo-signal at -8.4 dBFS RMS with the reference's flat envelope."""
    rng = np.random.Generator(np.random.PCG64(seed))
    n = int(seconds * FS)
    x = np.convolve(rng.standard_normal(n), filter_kernel(), mode="same")
    x *= 0.3803 / math.sqrt(np.mean(x * x))
    return np.clip(x, -0.98, 0.98)


class NoiseBandTests(unittest.TestCase):
    """The AWGN noise band of awgn.py against the C++ stage's design (awgn.hpp, U29)."""

    @staticmethod
    def response(frequencies):
        kernel = noise_band_kernel()
        offset = np.arange(len(kernel)) - (len(kernel) - 1) / 2
        return np.cos(2 * np.pi * np.outer(frequencies, offset) / FS) @ kernel

    def test_kaiser_band_matches_the_cpp_design(self):
        kernel = noise_band_kernel()
        self.assertEqual(NOISE_BAND_TAPS, 583)
        self.assertEqual(len(kernel), 583)
        self.assertTrue(np.array_equal(kernel, kernel[::-1]))
        self.assertAlmostEqual(float(self.response([1800.0])[0]), 1.0, delta=1e-12)
        # U29: flat within +/-0.02 dB over 300-3300 Hz (design 7.3: -0.009 / +0.010 dB).
        level = 20 * np.log10(np.abs(self.response(np.arange(300.0, 3300.5, 5.0))))
        self.assertGreater(level.min(), -0.02)
        self.assertLess(level.max(), 0.02)
        # ENBW 3233.4 +/- 0.5 Hz.
        self.assertAlmostEqual(noise_band_enbw_hz(), 3233.4, delta=0.5)
        # Stopband from 3600 Hz (cutoff 3450 + half the 300 Hz transition): the Kaiser
        # estimate gives -59.6 dB at the edge, and the band's power from 3700 to 6000 Hz
        # averages -77 dB.
        stop = self.response(np.arange(3600.0, 24000.0, 5.0))
        self.assertLess(20 * math.log10(float(np.max(np.abs(stop)))), -59.0)
        far = self.response(np.arange(3700.0, 6000.0, 5.0))
        self.assertLess(10 * math.log10(float(np.mean(far ** 2))), -70.0)

    def test_sigma_refers_n0_x_3000_to_the_label(self):
        # N0 = 2 sigma^2 / fs, so N0 x 3000 Hz = R^2 10^(-SNR/10).
        for reference, snr_db in ((0.125, 10.0), (0.38, -3.0), (1.0, 120.0)):
            n0 = 2 * noise_sigma(reference, snr_db) ** 2 / FS
            self.assertAlmostEqual(10 * math.log10(n0 * SNR_REFERENCE_BANDWIDTH_HZ),
                                   20 * math.log10(reference) - snr_db, delta=1e-9)

    def test_overlap_save_equals_direct_convolution_across_chunks(self):
        excitation = np.random.Generator(np.random.PCG64(9)).standard_normal(20000)
        rng = np.random.Generator(np.random.PCG64(1))
        band = BandNoise(rng, noise_band_kernel(), stationary=False)
        filtered = np.concatenate([band.filter(excitation[:9000]), band.filter(excitation[9000:])])
        direct = np.convolve(excitation, noise_band_kernel())[:len(excitation)]
        self.assertLess(float(np.max(np.abs(filtered - direct))), 1e-12)


class AwgnStageTests(unittest.TestCase):
    """The AWGN stage's sidecar values and its closed SNR range, as the C++ stage's (d896a99, 5af03ba)."""

    REFERENCE_RMS = 0.125

    def prepared(self, snr_db):
        stage = Awgn({"snr_db": snr_db})
        stage.prepare(self.REFERENCE_RMS, FS, 7, 0)
        return stage

    def test_the_sidecar_states_the_noise_levels_and_the_band(self):
        resolved = self.prepared(10).resolved()
        # N0 x 3000 Hz is the label: R^2 10^(-SNR/10). The total noise is N0 x ENBW,
        # 10 log10(3233.4 / 3000) = 0.325 dB above it.
        n0_x_3000_dbfs = 20 * math.log10(self.REFERENCE_RMS) - 10
        enbw_db = 10 * math.log10(noise_band_enbw_hz() / SNR_REFERENCE_BANDWIDTH_HZ)
        self.assertAlmostEqual(resolved["n0_x_3000_dbfs"], n0_x_3000_dbfs, delta=1e-12)
        self.assertAlmostEqual(resolved["noise_target_rms_dbfs"], n0_x_3000_dbfs + enbw_db, delta=1e-12)
        self.assertAlmostEqual(resolved["noise_target_rms_dbfs"] - resolved["n0_x_3000_dbfs"], 0.325, delta=0.001)
        self.assertEqual(resolved["enbw_hz"], noise_band_enbw_hz())
        self.assertAlmostEqual(resolved["enbw_hz"], 3233.4, delta=0.5)
        self.assertEqual(resolved["fir_taps"], 583)
        self.assertEqual(resolved["band_hz"], [300.0, 3300.0])
        self.assertEqual((resolved["snr_db"], resolved["start_frame"], resolved["end_frame"]), (10.0, 0, FS))
        self.assertEqual((resolved["rng_family"], resolved["rng_index"]), (FAMILY_BACKGROUND_NOISE, 0))

    def test_the_snr_range_is_closed_at_120_db(self):
        for snr_db in (-120, 120, -120.0, 120.0):
            with self.subTest(snr_db=snr_db):
                self.assertEqual(self.prepared(snr_db).snr_db, float(snr_db))
        for snr_db in (-120.001, 120.001, float("inf"), float("nan")):
            with self.subTest(snr_db=snr_db), self.assertRaisesRegex(ValueError, "awgn.snr_db is out of range"):
                self.prepared(snr_db)


class ImpairmentLevelTests(unittest.TestCase):
    def setUp(self):
        self.signal = synthetic_signal()
        self.reference = math.sqrt(float(np.mean(self.signal ** 2)))

    def run_stage(self, stage, seed=5):
        pipeline = Pipeline([stage])
        pipeline.prepare(self.reference, len(self.signal), seed)
        return np.concatenate(list(pipeline.run(self.signal)))

    def test_cw_level_is_ci_relative_to_reference(self):
        stage = Cw({"frequency_hz": 1800, "ci_db": 6, "ramp_seconds": 0})
        out = self.run_stage(stage)
        tone = out - self.signal
        ratio_db = 20 * math.log10(self.reference / math.sqrt(np.mean(tone ** 2)))
        self.assertAlmostEqual(ratio_db, 6.0, delta=0.05)

    def test_awgn_level_and_band(self):
        # N0 x 3000 Hz is the commanded noise, so the total noise (N0 x ENBW) reads
        # 10 log10(3233.4 / 3000) = 0.325 dB above it. 4 s of noise in a 3.2 kHz band
        # estimate the total power to about 0.04 dB (one s.e.).
        stage = Awgn({"snr_db": 10})
        out = self.run_stage(stage)
        noise = out - self.signal
        snr = 20 * math.log10(self.reference / math.sqrt(np.mean(noise ** 2)))
        enbw_db = 10 * math.log10(noise_band_enbw_hz() / SNR_REFERENCE_BANDWIDTH_HZ)
        self.assertAlmostEqual(snr, 10.0 - enbw_db, delta=0.15)
        # 300-3300 Hz holds 3000 / ENBW of the noise (the kernel is flat there and its
        # transitions carry the rest); above 3700 Hz is stopband.
        spectrum = np.abs(np.fft.rfft(noise)) ** 2
        freqs = np.fft.rfftfreq(len(noise), 1 / FS)
        inside = spectrum[(freqs >= 300) & (freqs <= 3300)].sum() / spectrum.sum()
        self.assertAlmostEqual(inside, SNR_REFERENCE_BANDWIDTH_HZ / noise_band_enbw_hz(), delta=0.01)
        self.assertLess(spectrum[freqs > 3700].sum() / spectrum.sum(), 1e-4)

    def test_repeated_awgn_is_independent_and_preserves_first_legacy_stream(self):
        # A preceding fade must not change the first AWGN's stream 0.
        stages = [Fade({"depth_db": 0, "duration_ms": 100, "starts_seconds": []}),
                  Awgn({"snr_db": 10}), Awgn({"snr_db": 10})]
        pipeline = Pipeline(stages)
        pipeline.prepare(self.reference, len(self.signal), 42)
        noises = [[], []]
        for first in range(0, len(self.signal), 4096):
            block = np.zeros(min(4096, len(self.signal) - first))
            for index, stage in enumerate(stages[1:]):
                noises[index].append(stage.process(block, first))
        first_noise, second_noise = [np.concatenate(parts) for parts in noises]
        legacy = BandNoise(generator(42, FAMILY_BACKGROUND_NOISE, 0), noise_band_kernel(), stationary=True)
        legacy_noise = np.concatenate([legacy.next(min(4096, len(self.signal) - first))
                                       for first in range(0, len(self.signal), 4096)]) * noise_sigma(self.reference, 10)
        self.assertTrue(np.array_equal(first_noise, legacy_noise))
        self.assertFalse(np.array_equal(first_noise, second_noise))
        self.assertLess(abs(float(np.corrcoef(first_noise, second_noise)[0, 1])), 0.05)
        combined_db = 10 * math.log10(np.dot(first_noise + second_noise, first_noise + second_noise) /
                                     np.dot(first_noise, first_noise))
        self.assertAlmostEqual(combined_db, 10 * math.log10(2), delta=0.3)
        self.assertEqual([stage.resolved()["rng_index"] for stage in stages[1:]], [0, 1])

    def test_impulse_peak_and_schedule(self):
        stage = Impulse({"period_seconds": 1.0, "first_seconds": 0.5, "peak_db": 20, "decay_ms": 5, "ring_hz": 0, "phase_degrees": 0})
        out = self.run_stage(stage)
        added = out - self.signal
        starts = [e["start_frame"] for e in stage.events()]
        self.assertEqual(starts, [int(0.5 * FS + k * FS) for k in range(4)])
        self.assertAlmostEqual(added[starts[0]], self.reference * 10 ** (20 / 20), places=9)
        self.assertAlmostEqual(added[starts[0] + int(5e-3 * FS)], self.reference * 10 * math.exp(-1), delta=1e-6)

    def test_fade_depth_and_untouched_regions(self):
        stage = Fade({"depth_db": 18, "duration_ms": 250, "shape": "raised_cosine", "starts_seconds": [1.0]})
        out = self.run_stage(stage)
        self.assertTrue(np.array_equal(out[: FS], self.signal[: FS]))
        self.assertTrue(np.array_equal(out[int(1.25 * FS) + 1:], self.signal[int(1.25 * FS) + 1:]))
        stats = stage.stats()
        self.assertAlmostEqual(stats["minimum_gain_db"], -18.0, places=6)
        self.assertEqual(stats["faded_frames"], int(0.25 * FS))
        rect = Fade({"depth_db": 24, "duration_ms": 100, "shape": "rectangular", "starts_seconds": [2.0]})
        out = self.run_stage(rect)
        gain = out[int(2.0 * FS): int(2.1 * FS)] / self.signal[int(2.0 * FS): int(2.1 * FS)]
        self.assertTrue(np.allclose(gain, 10 ** (-24 / 20)))

    def test_fade_count_requires_a_positive_uint32_integer(self):
        params = {"depth_db": 18, "duration_ms": 250, "first_seconds": 0,
                  "period_seconds": 1}
        for count in (0, -1, 1.5, True, "2", 2 ** 32, float("inf")):
            with self.subTest(count=count), self.assertRaisesRegex(ValueError, "fade.count"):
                Fade(dict(params, count=count)).prepare(self.reference, len(self.signal), 0, 0)
        stage = Fade(dict(params, count=2))
        stage.prepare(self.reference, len(self.signal), 0, 0)
        self.assertEqual(stage.starts, [0, FS])


class SampleSlipTests(unittest.TestCase):
    def run_slip(self, params, data, block):
        stage = SampleSlip(params)
        pipeline = Pipeline([stage])
        pipeline.prepare(1.0, len(data), 0)
        return np.concatenate(list(pipeline.run(data, block=block))), stage

    def test_delete_and_duplicate_across_blocks(self):
        data = np.arange(40, dtype=np.float64)
        out, stage = self.run_slip({"events": [{"at_seconds": 10 / FS, "kind": "delete", "length_samples": 3}]}, data, block=8)
        self.assertEqual(out.tolist(), [v for v in range(40) if v not in (10, 11, 12)])
        self.assertEqual(stage.stats()["net_shift_samples"], -3)
        out, _ = self.run_slip({"events": [{"at_seconds": 10 / FS, "kind": "duplicate", "length_samples": 3}]}, data, block=8)
        self.assertEqual(out.tolist(), list(range(10)) + [7, 8, 9] + list(range(10, 40)))
        out, _ = self.run_slip({"events": [{"at_seconds": 6 / FS, "kind": "delete", "length_samples": 5}]}, data, block=8)
        self.assertEqual(out.tolist(), list(range(6)) + list(range(11, 40)))

    def test_placements(self):
        data = np.zeros(FS * 3)
        _out, stage = self.run_slip({"kind": "delete", "length_samples": 5, "placement": "spaced", "first_seconds": 0.5, "period_seconds": 1.0, "count": 3}, data, block=4096)
        self.assertEqual([e["frame"] for e in stage.applied], [int(0.5 * FS), int(1.5 * FS), int(2.5 * FS)])
        _out, stage = self.run_slip({"kind": "duplicate", "length_samples": 48, "placement": "clustered", "first_seconds": 1.0, "spacing_seconds": 0.01, "count": 4}, data, block=4096)
        self.assertEqual(stage.stats()["net_shift_samples"], 4 * 48)

    def test_validation_matches_portable_limits(self):
        data = np.arange(2048, dtype=np.float64)
        for length in (0, -1, 1.5, 1024.5, 1025, 48000, True, "1", float("inf")):
            with self.subTest(form="compact", length=length), self.assertRaisesRegex(ValueError, "length_samples"):
                self.run_slip({"kind": "delete", "length_samples": length,
                               "placement": "single", "at_seconds": 0}, data, block=2048)
            with self.subTest(form="explicit", length=length), self.assertRaisesRegex(ValueError, "length_samples"):
                self.run_slip({"events": [{"at_seconds": 0, "kind": "delete",
                                            "length_samples": length}]}, data, block=2048)
        for count in (0, -1, 1.5, 33, True, "2", float("inf")):
            with self.subTest(count=count), self.assertRaisesRegex(ValueError, "sample_slip.count"):
                self.run_slip({"kind": "delete", "length_samples": 1,
                               "placement": "spaced", "first_seconds": 0,
                               "period_seconds": 0.001, "count": count}, data, block=2048)
        out, _ = self.run_slip({"kind": "delete", "length_samples": 1024,
                                "placement": "single", "at_seconds": 0}, data, block=2048)
        self.assertEqual(len(out), 1024)


class ScenarioAndRenderTests(unittest.TestCase):
    def test_validation_rules(self):
        base = {"test_id": "t", "seed": 1, "source": {"path": "x.wav"}}
        validate(dict(base, impairments=[{"type": "cw", "frequency_hz": 1800, "ci_db": 3}, {"type": "sample_slip", "kind": "delete", "length_samples": 1, "placement": "single", "at_seconds": 1}]))
        with self.assertRaises(ScenarioError):
            validate(dict(base, impairments=[{"type": "sample_slip", "kind": "delete", "length_samples": 1, "placement": "single", "at_seconds": 1}, {"type": "cw", "frequency_hz": 1800, "ci_db": 3}]))
        with self.assertRaises(ScenarioError):
            validate(dict(base, bogus=1))
        with self.assertRaises(ScenarioError):
            validate(dict(base, impairments=[{"type": "nope"}]))
        with self.assertRaisesRegex(ScenarioError, "watterson requires the C\\+\\+ signal_lab_render"):
            validate(dict(base, impairments=[{"type": "watterson", "paths": [{"spread_hz": 1}]}]))
        with self.assertRaisesRegex(ScenarioError, "sample_clock is deferred"):
            validate(dict(base, impairments=[{"type": "sample_clock", "offset_ppm": 1}]))
        with self.assertRaisesRegex(ScenarioError, "frequency_shift requires the C\\+\\+ signal_lab_render"):
            validate(dict(base, impairments=[{"type": "frequency_shift", "mode": "sweep"}]))
        # At most two AWGN stages, as the C++ engine's typed slots.
        validate(dict(base, impairments=[{"type": "awgn", "snr_db": 20}, {"type": "awgn", "snr_db": 30}]))
        with self.assertRaisesRegex(ScenarioError, "awgn: at most 2 stages per scenario"):
            validate(dict(base, impairments=[{"type": "awgn", "snr_db": snr} for snr in (20, 30, 40)]))

    def test_python_wideband_support_boundary(self):
        base = {"test_id": "t", "seed": 1, "source": {"path": "x.wav"}}
        self.assertNotIn("noise_bandwidth_hz", validate(base))
        self.assertEqual(validate(dict(base, noise_bandwidth_hz=3000))["noise_bandwidth_hz"], 3000)
        for bandwidth in (6000, 9000, 12000):
            with self.subTest(bandwidth=bandwidth), self.assertRaisesRegex(ScenarioError, "wideband noise requires the C\\+\\+"):
                validate(dict(base, noise_bandwidth_hz=bandwidth))
        for bandwidth in (True, None, "3000", 3000.5, 24000):
            with self.subTest(bandwidth=bandwidth), self.assertRaisesRegex(ScenarioError, "noise_bandwidth_hz must be"):
                validate(dict(base, noise_bandwidth_hz=bandwidth))

    def test_python_second_source_names_the_supported_renderer(self):
        scenario = {"test_id": "second-source", "seed": 17, "source": {"path": "desired.wav"},
                    "impairments": [{"type": "second_source", "source_path": "interferer.wav", "si_db": 12}]}
        with self.assertRaisesRegex(ScenarioError, "second_source requires the C\\+\\+ signal_lab_render tool"):
            validate(scenario)

    def test_render_is_deterministic_and_hash_bound(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "src.wav"
            write_pcm16(source, synthetic_signal(3.0))
            scenario = {"test_id": "det", "seed": 42, "source": {"path": "src.wav"}, "source_gain_db": -12,
                        "impairments": [{"type": "fade", "depth_db": 18, "duration_ms": 250, "starts_seconds": [1.0]},
                                        {"type": "awgn", "snr_db": 7}, {"type": "cw", "frequency_hz": 1800, "ci_db": 6},
                                        {"type": "impulse", "rate_per_sec": 2, "peak_db": 20, "decay_ms": 20},
                                        {"type": "sample_slip", "kind": "delete", "length_samples": 5, "placement": "single", "at_seconds": 2.0}]}
            a = render(scenario, Path(temp) / "a.wav", base_dir=temp)
            b = render(scenario, Path(temp) / "b.wav", base_dir=temp)
            self.assertEqual(a["output"]["pcm"]["sha256"], b["output"]["pcm"]["sha256"])
            self.assertEqual(a["scenario_sha256"], b["scenario_sha256"])
            self.assertEqual(a["output"]["samples"], 3 * FS - 5)
            self.assertEqual(a["impairment_order"], ["fade", "awgn", "cw", "impulse", "sample_slip"])
            self.assertEqual(wav_info(Path(temp) / "a.wav")["encoding"], "pcm_s24le")
            c = render(dict(scenario, seed=43), Path(temp) / "c.wav", base_dir=temp)
            self.assertNotEqual(a["output"]["pcm"]["sha256"], c["output"]["pcm"]["sha256"])

    def test_output_levels_measure_emitted_saturated_pcm(self):
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            write_pcm16(temp / "src.wav", np.tile([0.75, -0.75, 0.125, -0.125], 2048))
            scenario = {"test_id": "saturation", "source": {"path": "src.wav"},
                        "source_gain_db": 20 * math.log10(2), "clip_policy": "saturate"}
            sidecar = render(scenario, temp / "out.wav", base_dir=temp)
            emitted, _ = read_float(temp / "out.wav")
            self.assertGreater(sidecar["output"]["clipped_samples"], 0)
            self.assertAlmostEqual(sidecar["output"]["peak_dbfs"], 20 * math.log10(np.max(np.abs(emitted))), places=13)
            self.assertAlmostEqual(sidecar["output"]["rms_dbfs"], 10 * math.log10(np.mean(emitted * emitted)), places=13)



if __name__ == "__main__":
    unittest.main()
