#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
from pathlib import Path
import shutil
import struct
import unittest
import uuid


ROOT = Path(__file__).resolve().parents[1]
TEST_TEMP_ROOT = ROOT / "build" / "test-stage-wav"
TEST_TEMP_ROOT.mkdir(parents=True, exist_ok=True)
SPEC = importlib.util.spec_from_file_location("stage_wav", ROOT / "host" / "stage_wav.py")
assert SPEC is not None and SPEC.loader is not None
STAGE_WAV = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STAGE_WAV)


class StageWavTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_path = TEST_TEMP_ROOT / uuid.uuid4().hex
        self.temporary_path.mkdir()

    def tearDown(self) -> None:
        shutil.rmtree(self.temporary_path)

    def test_frozen_tone_generation_and_card_copy(self) -> None:
        tone = self.temporary_path / "tone.wav"
        facts = STAGE_WAV._write_tone_from_recipe(
            ROOT / "fixtures" / "tone-10s.json", tone, False
        )
        self.assertEqual(facts["file_bytes"], 960_044)
        self.assertEqual(facts["data_offset"], 44)
        self.assertEqual(facts["data_bytes"], 960_000)
        self.assertEqual(facts["samples"], 480_000)
        self.assertEqual(facts["duration_seconds"], 10.0)
        self.assertEqual(
            facts["file_sha256"],
            "17997fdffee61d60804c7d4a4943ed100a251ff19706354587739b757c505760",
        )
        self.assertEqual(
            facts["pcm_sha256"],
            "9771da8d42a4179bac40fe6ec740828364c8f406670532386490fda3640df6a8",
        )

        card = self.temporary_path / "card"
        card.mkdir()
        staged = STAGE_WAV.stage_wav(tone, card, False)
        self.assertTrue(staged["verified"])
        self.assertEqual(
            staged["source"]["file_sha256"], staged["destination_file_sha256"]
        )
        self.assertEqual((card / "WG" / "PLAY.WAV").read_bytes(), tone.read_bytes())
        self.assertEqual(
            (card / "WG" / "PLAY.WGM").read_text(encoding="ascii"),
            "WGM1\n"
            "wav_bytes=960044\n"
            "wav_sha256=17997fdffee61d60804c7d4a4943ed100a251ff19706354587739b757c505760\n"
            "data_offset=44\n"
            "data_bytes=960000\n"
            "pcm_sha256=9771da8d42a4179bac40fe6ec740828364c8f406670532386490fda3640df6a8\n"
            "samples=480000\n"
            "level_percent=70\n",
        )

    def test_rejects_an_unsupported_sample_rate(self) -> None:
        tone = self.temporary_path / "tone.wav"
        STAGE_WAV._write_tone_from_recipe(
            ROOT / "fixtures" / "tone-10s.json", tone, False
        )
        contents = bytearray(tone.read_bytes())
        struct.pack_into("<I", contents, 24, 44_100)
        tone.write_bytes(contents)
        with self.assertRaisesRegex(STAGE_WAV.WavError, "unsupported format"):
            STAGE_WAV.inspect_wav(tone)

    def test_rejects_truncated_chunk_data(self) -> None:
        tone = self.temporary_path / "tone.wav"
        STAGE_WAV._write_tone_from_recipe(
            ROOT / "fixtures" / "tone-10s.json", tone, False
        )
        tone.write_bytes(tone.read_bytes()[:-1])
        with self.assertRaisesRegex(STAGE_WAV.WavError, "RIFF length"):
            STAGE_WAV.inspect_wav(tone)


if __name__ == "__main__":
    unittest.main()
