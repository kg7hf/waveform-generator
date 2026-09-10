#!/usr/bin/env python3
"""Unit tests for the Phase-1 signal_lab engine, reference sizing and decoder-output scoring.

Run from the waveform-generator root:  python -m unittest tests.test_signal_lab -v
Optional cross-checks against the parent M110 repository (pathsim-campaign.py PN-11 and
corner table, tools/pcm_stress.py numerics) require WFG_PARENT_REFERENCE_TESTS=1.
"""

import importlib.util
import contextlib
import io
import json
import math
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "host"))

import m110_reference  # noqa: E402
import m110_score  # noqa: E402
import corpus_phase1  # noqa: E402
from signal_lab import FS  # noqa: E402
from signal_lab.awgn import Awgn, BandNoise, filter_kernel  # noqa: E402
from signal_lab.cw import Cw  # noqa: E402
from signal_lab.fade import Fade  # noqa: E402
from signal_lab.impulse import Impulse  # noqa: E402
from signal_lab.render import RenderError, render  # noqa: E402
from signal_lab.rng import FAMILY_BACKGROUND_NOISE, generator  # noqa: E402
from signal_lab.sample_slip import SampleSlip  # noqa: E402
from signal_lab.scenario import ScenarioError, validate  # noqa: E402
from signal_lab.stream import Pipeline  # noqa: E402
from signal_lab.wav_io import read_float, sha256_file, wav_info, write_pcm16  # noqa: E402

PARENT = ROOT.parent.parent
PATHSIM_CAMPAIGN = PARENT / "tools" / "pathsim-campaign.py"
PCM_STRESS = PARENT / "tools" / "pcm_stress.py"
PARENT_REFERENCE_TESTS = os.environ.get("WFG_PARENT_REFERENCE_TESTS") == "1"


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def synthetic_signal(seconds=4.0, seed=3):
    """A band-limited pseudo-signal at -8.4 dBFS RMS with the reference's flat envelope."""
    rng = np.random.Generator(np.random.PCG64(seed))
    n = int(seconds * FS)
    x = np.convolve(rng.standard_normal(n), filter_kernel(), mode="same")
    x *= 0.3803 / math.sqrt(np.mean(x * x))
    return np.clip(x, -0.98, 0.98)


class Pn11Tests(unittest.TestCase):
    def test_known_prefix_and_period(self):
        data = m110_reference.pn11_payload(4200)
        self.assertEqual(data[:6].hex(), "80301e0cc7fb")
        self.assertEqual(data[:2047], data[2047:4094])

    @unittest.skipUnless(PARENT_REFERENCE_TESTS and PATHSIM_CAMPAIGN.exists(), "parent reference tests not enabled or unavailable")
    def test_matches_pathsim_campaign(self):
        campaign = load_module(PATHSIM_CAMPAIGN, "pathsim_campaign")
        for length in (1, 100, 4188, 43766):
            self.assertEqual(m110_reference.pn11_payload(length), campaign.pn11_payload(length))

    @unittest.skipUnless(PARENT_REFERENCE_TESTS and PATHSIM_CAMPAIGN.exists(), "parent reference tests not enabled or unavailable")
    def test_corner_table_matches_campaign(self):
        campaign = load_module(PATHSIM_CAMPAIGN, "pathsim_campaign")
        spec = json.loads((ROOT / "corpus" / "phase1-corpus.json").read_text(encoding="utf-8"))
        corners = {rate: snr for rate, _il, _d, _s, snr, _t, _p in campaign.CONDITIONS}
        for rate, value in spec["corner_snr_db"].items():
            if rate.startswith("_"):
                continue
            self.assertEqual(float(value), float(corners[rate]), rate)


class FramingTests(unittest.TestCase):
    def test_predicted_samples_match_measured_transmitter(self):
        # Measured with m110_tx_to_pcm on 2026-09-06 (waveform samples + 48000 trailing silence).
        cases = {(600, "long", 1000): 921600, (600, "long", 1): 460800, (300, "short", 1): 86400,
                 (300, "short", 100): 201600, (300, "short", 1000): 1353600, (1200, "short", 1000): 374400,
                 (300, "long", 1000): 1612800, (1200, "long", 1000): 691200, (600, "short", 1000): 691200}
        for (rate, interleave, payload), waveform in cases.items():
            _preamble, _blocks, total = m110_reference.predicted_samples(rate, interleave, payload)
            self.assertEqual(total, waveform + FS, (rate, interleave, payload))

    def test_duration_sizing_lands_near_target(self):
        for rate in (300, 600, 1200):
            for interleave in ("long", "short"):
                payload = m110_reference.payload_bytes_for_duration(rate, interleave, 120.0)
                _p, _b, total = m110_reference.predicted_samples(rate, interleave, payload)
                self.assertTrue(112 <= total / FS <= 122, (rate, interleave, total / FS))


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
        stage = Awgn({"snr_db": 10})
        out = self.run_stage(stage)
        noise = out - self.signal
        snr = 20 * math.log10(self.reference / math.sqrt(np.mean(noise ** 2)))
        self.assertAlmostEqual(snr, 10.0, delta=0.3)
        spectrum = np.abs(np.fft.rfft(noise)) ** 2
        freqs = np.fft.rfftfreq(len(noise), 1 / FS)
        outside = spectrum[(freqs < 200) | (freqs > 3400)].sum() / spectrum.sum()
        self.assertLess(outside, 0.01)

    def test_repeated_awgn_is_independent_and_preserves_first_legacy_stream(self):
        # A preceding fade must not change the first AWGN's historical stream 0.
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
        legacy = BandNoise(generator(42, FAMILY_BACKGROUND_NOISE, 0), stationary=True)
        legacy_noise = np.concatenate([legacy.next(min(4096, len(self.signal) - first))
                                       for first in range(0, len(self.signal), 4096)]) * (self.reference * 10 ** (-10 / 20))
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
            self.assertEqual(wav_info(Path(temp) / "a.wav")["encoding"], "pcm_s16le")
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

    @unittest.skipUnless(PARENT_REFERENCE_TESTS and PCM_STRESS.exists(), "parent reference tests not enabled or unavailable")
    def test_matches_pcm_stress_for_shared_models(self):
        """AWGN + raised-cosine fade + CW must reproduce the owner's pcm_stress renderer bit-exactly."""
        pcm_stress = load_module(PCM_STRESS, "pcm_stress")
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            write_pcm16(temp / "src.wav", synthetic_signal(3.0))
            profile = {"schema": pcm_stress.SCHEMA, "input_kind": "signal_only", "seed": 4242,
                       "reference": {"start_seconds": 0.5, "duration_seconds": 1.0}, "output_gain_db": -6.0,
                       "noise": {"snr_db": 9.0},
                       "fades": [{"start_seconds": 1.0, "depth_db": 18, "attack_seconds": 0.0625, "hold_seconds": 0.125, "recovery_seconds": 0.0625}],
                       "tones": [{"start_seconds": 0.0, "duration_seconds": 3.0, "frequency_hz": 1800, "level_db": -6.0, "ramp_seconds": 0.01, "phase_degrees": 30}]}
            (temp / "profile.json").write_text(json.dumps(profile), encoding="utf-8")
            pcm_stress.render(temp / "src.wav", temp / "profile.json", temp / "ref.wav")
            scenario = {"test_id": "x", "seed": 4242, "source": {"path": "src.wav"}, "reference": {"start_seconds": 0.5, "duration_seconds": 1.0},
                        "source_gain_db": -6.0,
                        "impairments": [{"type": "fade", "depth_db": 18, "duration_ms": 250, "shape": "raised_cosine", "starts_seconds": [1.0]},
                                        {"type": "awgn", "snr_db": 9.0},
                                        {"type": "cw", "frequency_hz": 1800, "ci_db": 6.0, "ramp_seconds": 0.01, "phase_degrees": 30}]}
            render(scenario, temp / "ours.wav", base_dir=temp)
            theirs, _ = read_float(temp / "ref.wav")
            ours, _ = read_float(temp / "ours.wav")
            self.assertEqual(len(theirs), len(ours))
            self.assertLessEqual(int(np.max(np.abs(theirs - ours) * 32768)), 1)


class ScoreParserTests(unittest.TestCase):
    LOG = """capture=x.wav samples=969600 peak=-1.3_dBFS clipped_samples=140 engine=siso anchor=earliest
burst=0 start=0.000 timing=streaming correlation=0.8370 frequency_offset_hz=-2.09 preamble_symbol=0 detected_segment=4796 countdown=13 first_body_symbol=11516 supporting_segments=17 mode=600/long
  training_frequency_hz=-2.11
  payload_hex=00112233
  stream_release=none stream_faults=0 stream_eom_found=1 stream_blocks=3 stream_bytes=4 stream_bit_errors=0 stream_exact=1
  eom=found eom_bit_errors=0
  exact_payload=yes expected_bytes=4 received_bytes=4 bit_errors=0 compared_bits=32 missing_bytes=0
stream_summary blocks=4440 bursts_completed=1 loss_of_lock=0 missing_eom=0 discontinuities=0 frontend_clipped=140
summary decoded_bursts=1 exact_payloads=1 required_exact_payloads=1 clipped_samples=140 audio_quality=pass
"""

    def test_parse_and_flatten(self):
        record = m110_score.parse_decoder_output(self.LOG)
        flat = m110_score.flatten(record, 4, bytes.fromhex("00112233"))
        self.assertTrue(flat["acquired"] and flat["complete_message"] and flat["eom_detected"])
        self.assertEqual((flat["mode_detected"], flat["interleaver_detected"]), (600, "long"))
        self.assertEqual(flat["ber"], 0.0)
        self.assertEqual(flat["clipped_samples"], 140)
        self.assertEqual(flat["training_frequency_hz"], -2.11)
        self.assertEqual(flat["alignment"]["correct_prefix_bytes"], 4)

    def test_alignment_detects_shift(self):
        expected = bytes(range(256)) * 4
        received = expected[:100] + expected[105:]  # five bytes vanished at offset 100
        diag = m110_score.alignment_diagnostics(received.hex(), expected)
        self.assertEqual(diag["first_mismatch_byte"], 100)
        self.assertEqual(diag["tail_best_shift_bytes"], 5)
        self.assertTrue(diag["tail_realigned"])


class CorpusRecoveryTests(unittest.TestCase):
    def test_score_cache_and_record_use_actual_wav_for_both_sidecar_schemas(self):
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            decoder = temp / "decoder.exe"
            decoder.write_bytes(b"fixed decoder identity")
            payload = temp / "payload.bin"
            payload.write_bytes(bytes.fromhex("00112233"))
            for artifact_key in ("wav", "output"):
                with self.subTest(sidecar_schema=artifact_key):
                    wav = temp / (artifact_key + ".wav")
                    wav.write_bytes(b"first WAV")
                    sidecar = {"test_id": artifact_key, "mode": "600L", artifact_key: {"sha256": sha256_file(wav)}}
                    expected = {"path": str(payload)}
                    if artifact_key == "wav":
                        sidecar.update(kind="reference_perfect", payload=expected)
                    else:
                        sidecar.update(source_reference={"payload": expected, "mode": "600L"},
                                       tags={"family": "noise"})
                    wav.with_suffix(".json").write_text(json.dumps(sidecar), encoding="utf-8")
                    with patch("m110_score.run_decoder", return_value=([], 0, 0.0, ScoreParserTests.LOG)) as run, \
                            contextlib.redirect_stdout(io.StringIO()):
                        first = corpus_phase1.stage_score([wav], decoder, ["siso"], 1, False)
                        self.assertEqual(run.call_count, 1)
                        unchanged = corpus_phase1.stage_score([wav], decoder, ["siso"], 1, False)
                        self.assertEqual(run.call_count, 1)
                        self.assertEqual(first, unchanged)
                        wav.write_bytes(b"regenerated WAV")
                        sidecar[artifact_key]["sha256"] = sha256_file(wav)
                        wav.with_suffix(".json").write_text(json.dumps(sidecar), encoding="utf-8")
                        regenerated = corpus_phase1.stage_score([wav], decoder, ["siso"], 1, False)
                        self.assertEqual(run.call_count, 2)
                        self.assertEqual(regenerated["siso"][0]["wav_sha256"], sha256_file(wav))
                        # Changed input bytes must also invalidate a cache when its sidecar is stale.
                        wav.write_bytes(b"replaced WAV with stale sidecar")
                        replaced = corpus_phase1.stage_score([wav], decoder, ["siso"], 1, False)
                        self.assertEqual(run.call_count, 3)
                        self.assertEqual(replaced["siso"][0]["wav_sha256"], sha256_file(wav))

    def test_verify_reports_missing_cases_and_mismatches(self):
        with tempfile.TemporaryDirectory() as temp, contextlib.redirect_stdout(io.StringIO()):
            temp = Path(temp)
            write_pcm16(temp / "src.wav", np.full(128, 0.25))
            scenario = {"test_id": "present", "source": {"path": "src.wav"}, "source_gain_db": 0}
            output = temp / "present.wav"
            sidecar = render(scenario, output, base_dir=temp)
            present = (scenario, output)
            missing = (dict(scenario, test_id="missing"), temp / "missing.wav")
            self.assertEqual(corpus_phase1.stage_verify([present], 1), [])
            self.assertEqual(corpus_phase1.stage_verify([present, missing], 1), ["missing"])
            sidecar["output"]["pcm"]["sha256"] = "incorrect digest"
            output.with_suffix(".json").write_text(json.dumps(sidecar), encoding="utf-8")
            self.assertEqual(corpus_phase1.stage_verify([present], 1), ["present"])

    def test_verify_missing_corpus_fails_command_and_empty_plan_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(corpus_phase1.main(["--root", temp, "--stage", "verify"]), 4)
        with self.assertRaisesRegex(RenderError, "no scenarios"):
            corpus_phase1.stage_verify([], 1)


if __name__ == "__main__":
    unittest.main()
