# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Host-only contract tests for the wideband noise-spur evidence runner."""

from __future__ import annotations

import json
import math
import sys
import tempfile
import unittest
import wave
import datetime as dt
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

import run_wideband_validation as runner  # noqa: E402


def good_folds(*, detected: bool = True, floor: float = -82.0, worst: float = -78.0) -> dict:
    return {
        "folds": [{"period": period, "threshold_dbc": floor, "worst_dbc": worst, "above_threshold": 0}
                  for period in runner.FOLD_OUTPUT_PERIODS],
        "positive_control": {"period": 2048, "injected_dbc": -68.0, "measured_dbc": -68.1, "detected": detected},
    }


def good_lines(*, floor: float = -81.0, worst: float | None = None) -> dict:
    return {"floor_dbc": floor, "worst_line_dbc": worst, "detected": 0, "lines": []}


class FakeAnalysis:
    def __init__(self, *, lines: dict | None = None, folds: dict | None = None) -> None:
        self.lines = lines or good_lines()
        self.folds = folds or good_folds()
        self.line_calls: list[dict] = []
        self.fold_calls: list[dict] = []

    def line_search(self, path, start, count, reference_power, exclude, explicit_hz, segment_s, *, band_hz):
        self.line_calls.append({"path": path, "start": start, "count": count, "reference_power": reference_power,
                                "exclude": exclude, "explicit_hz": explicit_hz, "segment_s": segment_s,
                                "band_hz": band_hz})
        return self.lines

    def fold_harmonics(self, path, start, count, reference_power, periods, derived, *, inject_dbc, band_hz):
        self.fold_calls.append({"path": path, "start": start, "count": count, "reference_power": reference_power,
                                "periods": periods, "derived": derived, "inject_dbc": inject_dbc,
                                "band_hz": band_hz})
        return self.folds


class FakeRenderer:
    def __init__(self) -> None:
        self.calls: list[Path] = []

    def __call__(self, scenario_path: Path, wav_path: Path, sidecar_path: Path, config: runner.Config) -> dict:
        scenario = json.loads(scenario_path.read_text(encoding="utf-8"))
        bandwidth = scenario["noise_bandwidth_hz"]
        wav_path.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(wav_path), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(3)
            output.setframerate(runner.FS_HZ)
            output.writeframes(bytes(config.frames * 3))
        sidecar = {
            "schema": "signal-lab.watterson-render/1",
            "tool": "wfg_watterson_render",
            "tool_version": "1",
            "algorithm_version": 1,
            "frames_in": config.frames,
            "frames_out": config.frames,
            "clipped_samples": 0,
            "noise_bandwidth_hz": bandwidth,
            "noise_band_low_hz": 300.0,
            "noise_band_high_hz": 300.0 + bandwidth,
            "reference_rms": config.reference_rms,
            "source_gain_db": 0.0,
            "source_digest": f"silence-{bandwidth}",
            "output_digest": f"digest-{bandwidth}",
        }
        sidecar_path.write_text(json.dumps(sidecar), encoding="utf-8")
        self.calls.append(wav_path)
        return {"wall_seconds": 0.01, "stdout": f"rendered {bandwidth}", "stderr": ""}


class ConfigTests(unittest.TestCase):
    def test_rejects_invalid_duration_and_contract_parameters(self):
        for config in (runner.Config(duration_seconds=math.nan), runner.Config(duration_seconds=-1),
                       runner.Config(duration_seconds=3.000001), runner.Config(duration_seconds=3.1, bands_hz=(6000,)),
                       runner.Config(duration_seconds=3.1, seed=-1), runner.Config(duration_seconds=3.1, snr_db=9.0),
                       runner.Config(duration_seconds=3.1, reference_rms=0.2),
                       runner.Config(duration_seconds=3.1, line_segment_seconds=0.0),
                       runner.Config(duration_seconds=3.1, deadline_utc="2026-10-12T12:00:00+00:00")):
            with self.subTest(config=config), self.assertRaises(ValueError):
                config.validate()

    def test_default_campaign_is_five_hours_and_exact_three_bands(self):
        config = runner.Config()
        config.validate()
        self.assertEqual(config.duration_seconds, 18000.0)
        self.assertEqual(config.bands_hz, (6000, 9000, 12000))
        self.assertEqual(config.frames, 864_000_000)
        self.assertEqual(config.line_segment_seconds, 100.0)

    def test_default_gate_uses_repository_local_hold_location(self):
        self.assertEqual(runner.DEFAULT_HOLD_MARKERS, (
            runner.REPO / "build/main/bringup/AB_HOLD",
        ))

    def test_deadline_must_be_explicit_utc_z_timestamp(self):
        self.assertEqual(runner.parse_deadline("2026-10-12T12:00:00Z"),
                         dt.datetime(2026, 10, 12, 12, 0, tzinfo=dt.timezone.utc))
        for value in ("2026-10-12T12:00:00", "2026-10-12T12:00:00+00:00", "bad"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                runner.parse_deadline(value)


class VerdictTests(unittest.TestCase):
    def test_floor_below_limit_and_detected_control_still_marks_short_run_smoke_only(self):
        result = runner.evaluate_row(good_lines(), good_folds(), duration_seconds=30.0)
        self.assertEqual(result["status"], "inconclusive")
        self.assertTrue(result["floor_pass"])
        self.assertTrue(result["control_pass"])
        self.assertFalse(result["acceptance_eligible"])
        self.assertIn("short render is smoke-only", result["inconclusive_reasons"][-1])

    def test_insufficient_measurement_floor_is_inconclusive_not_pass_or_fail(self):
        result = runner.evaluate_row(good_lines(floor=-66.0), good_folds(floor=-69.0), duration_seconds=18000.0)
        self.assertEqual(result["status"], "inconclusive")
        self.assertFalse(result["floor_pass"])
        self.assertTrue(result["control_pass"])
        self.assertFalse(result["acceptance_eligible"])

    def test_failed_positive_control_fails_the_row(self):
        result = runner.evaluate_row(good_lines(), good_folds(detected=False), duration_seconds=18000.0)
        self.assertEqual(result["status"], "fail")
        self.assertFalse(result["control_pass"])
        self.assertIn("positive control", result["errors"][0])

    def test_smoke_missed_control_is_inconclusive_when_matching_period_floor_is_too_high(self):
        folds = good_folds(detected=False)
        folds["folds"][0]["threshold_dbc"] = -51.16985983455835  # period 2048; same order as the real smoke report
        folds["positive_control"]["measured_dbc"] = -72.07178732751908
        result = runner.evaluate_row(good_lines(floor=-49.318), folds, duration_seconds=32.0)
        self.assertEqual(result["status"], "inconclusive")
        self.assertEqual(result["control_status"], "inconclusive")
        self.assertEqual(result["control_period"], 2048)
        self.assertAlmostEqual(result["control_detection_floor_dbc"], -51.16985983455835)
        self.assertFalse(result["control_pass"])
        self.assertEqual(result["errors"], [])
        self.assertTrue(any("positive-control floor" in reason for reason in result["inconclusive_reasons"]))

    def test_detected_control_within_tolerance_is_valid_even_if_fold_floor_is_weak(self):
        folds = good_folds(floor=-51.0)
        result = runner.evaluate_row(good_lines(floor=-49.0), folds, duration_seconds=32.0)
        self.assertEqual(result["control_status"], "pass")
        self.assertTrue(result["control_pass"])
        self.assertEqual(result["status"], "inconclusive")  # line/fold floors still block a pass

    def test_control_uses_its_period_floor_not_another_period_floor(self):
        folds = good_folds(detected=False)
        folds["folds"][0]["threshold_dbc"] = -75.0  # control period 2048 is resolvable
        folds["folds"][1]["threshold_dbc"] = -51.0  # unrelated period 1024 is not
        result = runner.evaluate_row(good_lines(), folds, duration_seconds=18000.0)
        self.assertEqual(result["control_status"], "fail")
        self.assertEqual(result["control_detection_floor_dbc"], -75.0)
        self.assertTrue(any("positive control" in error for error in result["errors"]))

    def test_malformed_control_fails_even_when_its_floor_is_inadequate(self):
        folds = good_folds(detected=False, floor=-51.0)
        folds["positive_control"].pop("measured_dbc")
        result = runner.evaluate_row(good_lines(floor=-49.0), folds, duration_seconds=32.0)
        self.assertEqual(result["status"], "fail")
        self.assertEqual(result["control_status"], "fail")
        self.assertTrue(any("missing or malformed" in error for error in result["errors"]))

    def test_detected_spur_over_limit_fails_even_when_floor_is_clear(self):
        result = runner.evaluate_row(good_lines(worst=-69.5), good_folds(), duration_seconds=18000.0)
        self.assertEqual(result["status"], "fail")
        self.assertTrue(result["floor_pass"])
        self.assertIn("line-search spur", result["errors"][0])

    def test_missing_fold_measurement_is_inconclusive(self):
        folds = good_folds()
        folds["folds"][0]["worst_dbc"] = None
        result = runner.evaluate_row(good_lines(), folds, duration_seconds=18000.0)
        self.assertEqual(result["status"], "inconclusive")
        self.assertIn("worst-harmonic level is unavailable", result["inconclusive_reasons"][0])

    def test_raw_fold_maximum_above_limit_does_not_fail_without_detection_and_floor(self):
        folds = good_folds(floor=-65.0, worst=-68.0)
        result = runner.evaluate_row(good_lines(), folds, duration_seconds=18000.0)
        self.assertEqual(result["status"], "inconclusive")
        self.assertEqual(result["errors"], [])
        self.assertFalse(result["floor_pass"])
        self.assertTrue(any("detection floor" in reason for reason in result["inconclusive_reasons"]))

    def test_statistically_detected_fold_over_limit_fails(self):
        folds = good_folds(floor=-75.0, worst=-69.0)
        for row in folds["folds"]:
            row["above_threshold"] = 1
        result = runner.evaluate_row(good_lines(), folds, duration_seconds=18000.0)
        self.assertEqual(result["status"], "fail")
        self.assertTrue(any("harmonic" in error and "exceeds" in error for error in result["errors"]))

    def test_deadline_gate_stops_at_boundary_after_hold_check(self):
        calls = []
        gate = runner.campaign_gate(
            (Path("hold-a"),), "2026-10-12T12:00:00Z",
            now_utc=lambda: dt.datetime(2026, 10, 12, 12, 0, tzinfo=dt.timezone.utc),
            hold_check=lambda markers: calls.append(markers),
        )
        with self.assertRaisesRegex(RuntimeError, "refusing to start the next"):
            gate()
        self.assertEqual(calls, [(Path("hold-a"),)])


class SidecarTests(unittest.TestCase):
    def sidecar(self, bandwidth: int) -> dict:
        return {"schema": "signal-lab.watterson-render/1", "tool": "wfg_watterson_render", "tool_version": "1",
                "frames_in": runner.Config(duration_seconds=3.1).frames,
                "frames_out": runner.Config(duration_seconds=3.1).frames, "clipped_samples": 0,
                "noise_bandwidth_hz": bandwidth, "noise_band_low_hz": 300.0,
                "noise_band_high_hz": 300.0 + bandwidth, "reference_rms": runner.REFERENCE_RMS,
                "source_gain_db": 0.0, "source_digest": "source", "output_digest": "output"}

    def test_wrong_or_missing_noise_band_is_rejected(self):
        config = runner.Config(duration_seconds=3.1)
        for data in (self.sidecar(3000), {key: value for key, value in self.sidecar(6000).items()
                                         if key != "noise_bandwidth_hz"}):
            with self.subTest(data=data), self.assertRaisesRegex(ValueError, "noise band"):
                runner.validate_sidecar(data, config, 6000)


class CampaignTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "campaign"
        self.config = runner.Config(duration_seconds=3.1)
        self.renderer = FakeRenderer()
        self.analysis = FakeAnalysis()
        self.gate_calls = 0

    def gate(self):
        self.gate_calls += 1

    @staticmethod
    def identity(config: runner.Config, version: str = "renderer-a") -> dict:
        return {"schema": runner.SCHEMA, "config": config.as_json(),
                "scenarios": [{"bandwidth_hz": row["bandwidth_hz"], "sha256": row["scenario_sha256"]}
                              for row in runner.expected_rows(config)],
                "renderer": {"sha256": version},
                "analysis": "fake-analysis", "runner": "fake-runner"}

    def run_once(self, *, resume: bool = False, version: str = "renderer-a") -> dict:
        return runner.run_campaign(self.root, Path("fake-renderer"), self.config, resume=resume,
                                   render_fn=self.renderer, analysis=self.analysis, gate=self.gate,
                                   identity_override=self.identity(self.config, version))

    def test_three_rows_use_band_edges_frame_margins_control_and_gate(self):
        report = self.run_once()
        self.assertEqual([row["bandwidth_hz"] for row in report["rows"]], [6000, 9000, 12000])
        self.assertEqual(report["status"], "inconclusive")
        self.assertEqual(len(self.renderer.calls), 3)
        self.assertEqual(len(self.analysis.line_calls), 3)
        self.assertEqual(len(self.analysis.fold_calls), 3)
        self.assertEqual(self.gate_calls, 12)  # row start + render + Welch line search + harmonic fold per row
        for line, fold, bandwidth in zip(self.analysis.line_calls, self.analysis.fold_calls, (6000, 9000, 12000)):
            self.assertEqual(line["band_hz"], (300.0, 300.0 + bandwidth))
            self.assertEqual(line["reference_power"], runner.REFERENCE_RMS ** 2)
            self.assertEqual((line["start"], line["count"]), (48000, 52800))
            self.assertEqual(line["segment_s"], 1.1)
            self.assertEqual(fold["band_hz"], line["band_hz"])
            self.assertEqual(fold["periods"], (2048,))
            self.assertEqual(fold["derived"], {1024: 2048, 256: 1024, 64: 256})
            self.assertEqual(fold["inject_dbc"], -68.0)
        manifest = json.loads((self.root / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(set(manifest["rows"]), {"noise-6k", "noise-9k", "noise-12k"})
        for row in report["rows"]:
            self.assertRegex(row["artifacts"]["wav"]["sha256"], r"^[0-9a-f]{64}$")
            self.assertRegex(row["artifacts"]["sidecar"]["sha256"], r"^[0-9a-f]{64}$")
            self.assertEqual(row["source"]["kind"], "digital_silence")
            self.assertIsNotNone(row["renderer"]["algorithm_version"])
        self.assertTrue((self.root / "report.json").is_file())
        self.assertTrue((self.root / "summary.md").is_file())

    def test_exact_resume_reuses_rows_and_report_without_rendering_or_reanalysis(self):
        first = self.run_once()
        initial_report = (self.root / "report.json").read_bytes()
        initial_calls = (len(self.renderer.calls), len(self.analysis.line_calls), len(self.analysis.fold_calls))
        second = self.run_once(resume=True)
        self.assertEqual(second, first)
        self.assertEqual((len(self.renderer.calls), len(self.analysis.line_calls), len(self.analysis.fold_calls)), initial_calls)
        self.assertEqual((self.root / "report.json").read_bytes(), initial_report)

    def test_resume_with_different_renderer_identity_refuses_without_overwrite(self):
        self.run_once()
        before = (self.root / "renders/noise-6k.wav").read_bytes()
        with self.assertRaisesRegex(ValueError, "resume identity differs"):
            self.run_once(resume=True, version="renderer-b")
        self.assertEqual((self.root / "renders/noise-6k.wav").read_bytes(), before)
        self.assertEqual(len(self.renderer.calls), 3)

    def test_collision_without_exact_resume_refuses_before_render(self):
        self.root.mkdir()
        collision = self.root / "renders/noise-6k.wav"
        collision.parent.mkdir()
        collision.write_bytes(b"keep")
        with self.assertRaisesRegex(FileExistsError, "non-empty"):
            self.run_once()
        self.assertEqual(collision.read_bytes(), b"keep")
        self.assertEqual(self.renderer.calls, [])

    def test_hold_marker_blocks_work(self):
        marker = self.root / "AB_HOLD"
        marker.parent.mkdir(parents=True)
        marker.touch()
        with self.assertRaisesRegex(RuntimeError, "AB_HOLD"):
            runner.default_gate((marker,))

    def test_resume_refuses_modified_wav_hash(self):
        self.run_once()
        wav = self.root / "renders/noise-6k.wav"
        wav.write_bytes(wav.read_bytes() + b"changed")
        with self.assertRaisesRegex(ValueError, "artifact changed or missing"):
            self.run_once(resume=True)


if __name__ == "__main__":
    unittest.main()
