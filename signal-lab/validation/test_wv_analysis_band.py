# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Focused synthetic coverage for selectable spectral-analysis bands."""

import math
import sys
import tempfile
import unittest
import wave
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wv_analysis as wa  # noqa: E402


def legacy_line_search_summary(path: Path, start: int, count: int, reference_power: float,
                               explicit_hz: list[float], segment_s: float) -> dict:
    """Pre-band-selectable default implementation, retained here as a baseline oracle."""
    f, power, info = wa.welch_hann(path, start, count, int(round(segment_s * wa.FS_HZ)))
    inband = (f >= wa.BAND_HZ[0]) & (f <= wa.BAND_HZ[1])
    keep = inband.copy()
    median = np.zeros_like(power)
    for low in np.arange(wa.BAND_HZ[0], wa.BAND_HZ[1], 100.0):
        selected = (f >= low) & (f < low + 100.0) & keep
        if np.any(selected):
            median[(f >= low) & (f < low + 100.0)] = np.median(power[selected])
    dof = 2.0 * info['dof_segments']
    threshold = wa.chi2_ratio_quantile(dof)
    enbw = info['enbw_hz']
    mean = median / (1.0 - 2.0 / (9.0 * dof)) ** 3
    ratio = np.where(keep & (mean > 0), power / np.where(mean > 0, mean, 1.0), 0.0)
    detected = np.flatnonzero(ratio > threshold)
    lines = []
    for index in detected[np.argsort(-ratio[detected])][:20]:
        lines.append({'frequency_hz': float(f[index]), 'ratio_db': wa.db10(float(ratio[index])),
                      'level_dbc': wa.db10(max(float(power[index] - mean[index]), 1e-300) * enbw / reference_power)})
    floor = wa.db10(float(np.median(mean[keep])) * (threshold - 1.0) * enbw / reference_power)
    explicit = []
    for frequency in explicit_hz:
        if wa.BAND_HZ[0] <= frequency <= wa.BAND_HZ[1]:
            index = int(round(frequency / info['bin_hz']))
            local = slice(max(0, index - 1), index + 2)
            peak = int(np.argmax(power[local])) + max(0, index - 1)
            if keep[peak]:
                excess = float(power[peak] - mean[peak])
                explicit.append({'frequency_hz': frequency, 'ratio_db': wa.db10(float(ratio[peak])),
                                 'level_dbc': wa.db10(excess * enbw / reference_power) if excess > 0 else None})
    return {'threshold_ratio_db': wa.db10(threshold), 'floor_dbc': floor, 'detected': len(detected), 'lines': lines,
            'worst_line_dbc': max((line['level_dbc'] for line in lines), default=None),
            'explicit': explicit, 'worst_explicit_dbc': max((line['level_dbc'] for line in explicit if line['level_dbc'] is not None), default=None)}


class SpectralBandTests(unittest.TestCase):
    @staticmethod
    def write_pcm16(path: Path, samples: np.ndarray) -> None:
        codes = np.clip(np.rint(samples * 32768.0), -32768, 32767).astype('<i2')
        with wave.open(str(path), 'wb') as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(int(wa.FS_HZ))
            output.writeframes(codes.tobytes())

    def test_band_validation_is_finite_ordered_and_inside_nyquist(self):
        invalid_bands = (
            (-1.0, 3000.0), (300.0, 24001.0), (1000.0, 1000.0),
            (2000.0, 1000.0), (float('nan'), 1000.0), (300.0, float('inf')),
            (True, 1000.0), ('300', 1000.0), (300.0,), (300.0, 1000.0, 1200.0),
        )
        for band in invalid_bands:
            with self.subTest(band=band):
                with self.assertRaises(ValueError):
                    wa._validated_band(band)
        self.assertEqual(wa._validated_band((0, wa.FS_HZ / 2)), (0.0, wa.FS_HZ / 2))

    def test_line_search_detects_wideband_positive_control_and_ignores_out_of_band_tone(self):
        frames = int(wa.FS_HZ * 4)
        index = np.arange(frames)
        rng = np.random.default_rng(20261012)
        noise = rng.normal(0.0, 0.001, frames)
        in_band_amplitude = math.sqrt(2.0 * 10.0 ** (-68.0 / 10.0))
        out_band_amplitude = math.sqrt(2.0 * 10.0 ** (-40.0 / 10.0))
        with tempfile.TemporaryDirectory() as directory:
            for width, in_band_hz in ((6000, 4500.0), (9000, 6600.0), (12000, 8700.0)):
                with self.subTest(width_hz=width):
                    band = (300.0, 300.0 + width)
                    in_band = noise + in_band_amplitude * np.cos(2.0 * math.pi * in_band_hz * index / wa.FS_HZ)
                    out_band = noise + out_band_amplitude * np.cos(2.0 * math.pi * 18000.0 * index / wa.FS_HZ)
                    in_path = Path(directory) / f'in-band-{width}.wav'
                    out_path = Path(directory) / f'out-band-{width}.wav'
                    self.write_pcm16(in_path, in_band)
                    self.write_pcm16(out_path, out_band)
                    detected = wa.line_search(in_path, 0, frames, 1.0, [], [in_band_hz], 1.0, band_hz=band)
                    excluded = wa.line_search(out_path, 0, frames, 1.0, [], [18000.0], 1.0, band_hz=band)
                    self.assertAlmostEqual(detected['worst_explicit_dbc'], -68.0, delta=0.2)
                    self.assertGreater(detected['detected'], 0)
                    self.assertLess(detected['floor_dbc'], -70.0)
                    self.assertEqual(detected['false_alarm_probability_per_bin'], wa.FALSE_ALARM_PER_TEST)
                    self.assertEqual(detected['tested_bins'], width + 1)
                    self.assertAlmostEqual(detected['family_wise_false_alarm_upper_bound'], (width + 1) * 1e-6)
                    self.assertEqual(excluded['explicit'], [])
                    self.assertEqual(excluded['detected'], 0)
                    self.assertLess(excluded['floor_dbc'], -70.0)

            out_path = Path(directory) / 'legacy-default.wav'
            self.write_pcm16(out_path, noise + out_band_amplitude * np.cos(2.0 * math.pi * 18000.0 * index / wa.FS_HZ))
            legacy_default = wa.line_search(out_path, 0, frames, 1.0, [], [15000.0], 1.0)
            explicit_default = wa.line_search(out_path, 0, frames, 1.0, [], [15000.0], 1.0, band_hz=wa.BAND_HZ)
            baseline = legacy_line_search_summary(out_path, 0, frames, 1.0, [15000.0], 1.0)

        self.assertEqual(legacy_default, explicit_default)
        for key, value in baseline.items():
            self.assertEqual(legacy_default[key], value, key)

    def test_inclusive_upper_edge_now_receives_a_median_and_line_test(self):
        frames = int(wa.FS_HZ * 4)
        index = np.arange(frames)
        rng = np.random.default_rng(3300)
        noise = rng.normal(0.0, 0.001, frames)
        amplitude = math.sqrt(2.0 * 10.0 ** (-68.0 / 10.0))
        samples = noise + amplitude * np.cos(2.0 * math.pi * 3300.0 * index / wa.FS_HZ)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'upper-edge.wav'
            self.write_pcm16(path, samples)
            current = wa.line_search(path, 0, frames, 1.0, [], [3300.0], 1.0)
            baseline = legacy_line_search_summary(path, 0, frames, 1.0, [3300.0], 1.0)

        self.assertAlmostEqual(current['worst_explicit_dbc'], -68.0, delta=0.2)
        self.assertTrue(any(line['frequency_hz'] == 3300.0 for line in current['lines']))
        self.assertFalse(any(line['frequency_hz'] == 3300.0 for line in baseline['lines']))

    def test_fold_harmonics_uses_wide_band_and_reports_familywise_bound(self):
        period = 480  # 100 Hz harmonic spacing, exactly coherent at 48 kHz.
        repetitions = 512
        frames = period * repetitions
        index = np.arange(frames)
        rng = np.random.default_rng(25)
        noise = rng.normal(0.0, 0.001, frames)
        in_band_amplitude = math.sqrt(2.0 * 10.0 ** (-68.0 / 10.0))
        out_band_amplitude = math.sqrt(2.0 * 10.0 ** (-40.0 / 10.0))
        with tempfile.TemporaryDirectory() as directory:
            for width, in_band_hz in ((6000, 4500.0), (9000, 6600.0), (12000, 8700.0)):
                with self.subTest(width_hz=width):
                    band = (300.0, 300.0 + width)
                    samples = (noise + in_band_amplitude * np.cos(2.0 * math.pi * in_band_hz * index / wa.FS_HZ) +
                               out_band_amplitude * np.cos(2.0 * math.pi * 18000.0 * index / wa.FS_HZ))
                    path = Path(directory) / f'fold-{width}.wav'
                    self.write_pcm16(path, samples)
                    result = wa.fold_harmonics(path, 0, frames, 1.0, (period,), {}, inject_dbc=-68.0, band_hz=band)

                    row = result['folds'][0]
                    expected_bins = width // 100 + 1
                    self.assertEqual(row['inband_count'], expected_bins)
                    self.assertEqual(row['tested_bins'], expected_bins)
                    self.assertEqual(row['false_alarm_probability_per_bin'], wa.FALSE_ALARM_PER_TEST)
                    self.assertAlmostEqual(row['family_wise_false_alarm_upper_bound'], expected_bins * 1e-6)
                    self.assertAlmostEqual(row['worst_dbc'], -68.0, delta=0.2)
                    self.assertLess(row['worst_frequency_hz'], band[1] + 1e-9)
                    self.assertLess(row['threshold_dbc'], -70.0)
                    control = result['positive_control']
                    self.assertTrue(control['detected'])
                    self.assertAlmostEqual(control['measured_dbc'], -68.0, delta=0.2)

    def test_spectral_analyzers_refuse_invalid_bands_before_reading_files(self):
        invalid = (float('nan'), 12000.0)
        with self.assertRaises(ValueError):
            wa.line_search(Path('missing.wav'), 0, 100, 1.0, [], [], band_hz=invalid)
        with self.assertRaises(ValueError):
            wa.fold_harmonics(Path('missing.wav'), 0, 100, 1.0, (8,), {}, band_hz=invalid)

    def test_empty_spectral_selection_and_too_short_inputs_refuse_clearly(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'silence.wav'
            self.write_pcm16(path, np.zeros(48000, dtype=np.float64))
            with self.assertRaisesRegex(ValueError, 'no non-DC'):
                wa.fold_harmonics(path, 0, 48000, 1.0, (480,), {}, band_hz=(301.0, 399.0))
            with self.assertRaisesRegex(ValueError, 'no FFT bins'):
                wa.line_search(path, 0, 48000, 1.0, [], [], 1.0, band_hz=(300.1, 300.2))
        with self.assertRaisesRegex(ValueError, 'one complete Welch segment'):
            wa.line_search(Path('missing.wav'), 0, 47999, 1.0, [], [], 1.0)
        with self.assertRaisesRegex(ValueError, 'complete harmonic-fold period'):
            wa.fold_harmonics(Path('missing.wav'), 0, 479, 1.0, (480,), {})


if __name__ == '__main__':
    unittest.main()
