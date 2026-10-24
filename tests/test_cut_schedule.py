# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""CUT schedules on the host (host/cut_schedule.py): the command's spelling,
the board's rules, seeded generation, the cut stream and its tally, and the
prediction of a play's USB? values, cross-checked against the firmware's own
cut stage and log (tests/cut_schedule_tests.cpp --predict, named by the
WFG_CUT_PREDICTOR environment variable that ctest sets). The cuts act on the
engine's output, after the channel: a clean card's output is the card, and a
scenario play's is the host render (host/render/signal_lab_render, named by
WFG_SIGNAL_LAB_RENDER), on which the prediction is made. With WFG_FSRAND_WAV
naming image A's CUT card (FSRAND.WAV), image A's CUT rows 6.1 to 6.5
reproduce with the values the bench measured on 2057673. No board, no
ports."""

import hashlib
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "host"))
import cut_schedule as cuts


FRAMES = 20_000


def write_card(path: Path, samples: list[int]) -> bytes:
    """A mono 48 kHz PCM24 WAV of the samples; returns its data chunk."""
    data = b"".join((sample & 0xFFFFFF).to_bytes(3, "little") for sample in samples)
    header = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + len(data) + len(data) % 2, b"WAVE", b"fmt ", 16, 1, 1,
                         48_000, 144_000, 3, 24, b"data", len(data))
    path.write_bytes(header + data + b"\0" * (len(data) % 2))
    return data


def full_range(count: int, seed: int) -> list[int]:
    """Full-range PCM24 with both extremes and some exact zeros."""
    rng = random.Random(seed)
    return [-(1 << 23), (1 << 23) - 1, 0] + [rng.randint(-(1 << 23), (1 << 23) - 1) for _ in range(count - 3)]


def expected_bytes(data: bytes, entries: list[cuts.CutEntry]) -> bytes:
    """The cut stream's bytes built the plain way, independent of cuts.apply():
    `data` is the engine's output (a clean card, or a scenario's render)."""
    frames = len(data) // 3
    out = bytearray()
    at = 0
    for entry in entries:
        if entry.offset >= frames:
            continue
        out += data[at * 3:entry.offset * 3]
        at = entry.offset
        if entry.kind == "zero":
            out += bytes(entry.length * 3)
        else:
            at = min(frames, at + entry.length)
    out += data[at * 3:]
    return bytes(out)


class SpellingAndRules(unittest.TestCase):
    def test_round_trip(self):
        command = "CUT:0:1:zero,48000:4800:drop,96000:480:zero,96000:480:drop"
        entries = cuts.parse_command(command)
        self.assertEqual(entries[1], cuts.CutEntry(48000, 4800, "drop"))
        self.assertEqual(cuts.render_command(entries), command)
        self.assertEqual(cuts.parse_command("CUT"), [])
        self.assertEqual(cuts.render_command([]), "CUT")

    def test_widest_schedule_fits_one_request(self):
        entries = [cuts.CutEntry(4_294_967_295 - 15 + index, 1, "zero") for index in range(16)]
        command = cuts.render_command(entries)
        self.assertEqual(cuts.parse_command(command), entries)
        self.assertLessEqual(len(f"4294967295 {command}\r\n"), 384)

    def test_bad_spellings(self):
        for text in ("CUT:", "CUT:1:1:drop,", "CUT:01:1:drop", "CUT:1:1:DROP", "CUT:1:1", "CUT:1:1:drop:2", "CUT:-1:1:drop",
                     "CUT:4294967296:1:drop", "CUT 1:1:drop", "CUTS", "CUT:" + ",".join(["1:1:zero"] * 17)):
            with self.subTest(text=text), self.assertRaises(cuts.CutError) as caught:
                cuts.parse_command(text)
            self.assertEqual(caught.exception.code, cuts.BAD_REQUEST)

    def test_rules_and_codes(self):
        cases = {
            "10:0:drop": cuts.CUT_RANGE,
            "10:4801:zero": cuts.CUT_RANGE,
            "10:1:zero,9:1:zero": cuts.CUT_UNSORTED,
            "10:5:drop,14:1:zero": cuts.CUT_OVERLAP,
            "10:5:drop,10:1:zero": cuts.CUT_OVERLAP,
            "10:5:zero,10:1:zero": cuts.CUT_OVERLAP,
            "0:4800:drop,4800:4800:drop,9600:4800:drop,14400:1:drop": cuts.CUT_DROP_RUN,
            "0:4800:drop,4800:4800:drop,9600:4800:drop,14400:4800:zero,14400:1:drop": cuts.CUT_DROP_RUN,
            "0:4800:drop,4800:4800:drop,9600:4800:drop,19199:4800:drop": cuts.CUT_DROP_RUN,
        }
        for text, code in cases.items():
            with self.subTest(text=text), self.assertRaises(cuts.CutError) as caught:
                cuts.validate(cuts.parse_entries(text))
            self.assertEqual(caught.exception.code, code)
        for text in ("10:5:zero,10:5:drop", "10:5:drop,15:5:zero", "10:5:drop,15:5:drop", "10:5:zero,11:5:zero",
                     "0:4800:drop,4800:4800:drop,9600:4800:drop", "0:4800:drop,4800:4800:drop,9600:4800:drop,19200:4800:drop",
                     "0:4800:drop,4800:4800:drop,9600:4800:drop,14401:1:drop"):
            with self.subTest(text=text):
                cuts.validate(cuts.parse_entries(text))
        self.assertEqual(cuts.drop_balance(cuts.parse_entries("0:4800:drop,4800:4800:drop,9600:4800:drop,19200:4800:drop")), 14_400)
        self.assertEqual(cuts.drop_balance(cuts.parse_entries("100:10:zero,200:300:drop,600:50:drop")), 300)


class RendererSummary(unittest.TestCase):
    def test_engine_status_from_the_summary(self):
        line = ("output=C:\\cards\\a b\\R.WAV frames_in=480000 frames_out=480100 clipped=3 peak_dbfs=-1.25 digest=2b548ae934716e5f "
                "source_digest=30d8f9e86de0e5a6 stages=3 events=7/6/1 arena=1024\n")
        self.assertEqual(cuts.engine_status("a warning first\n" + line),
                         {"engine_frames_in": 480_000, "engine_frames_out": 480_100, "engine_clipped": 3, "engine_digest_hi": 0x2B548AE9,
                          "engine_digest_lo": 0x34716E5F, "engine_source_digest_hi": 0x30D8F9E8, "engine_source_digest_lo": 0x6DE0E5A6,
                          "engine_events_applied": 6, "engine_events_dropped": 1})
        with self.assertRaises(cuts.RenderError):
            cuts.engine_status("output=R.WAV frames_in=1\n")

    def test_no_wav_past_32_bit_sizes(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "LONG.WAV"
            with self.assertRaisesRegex(ValueError, "do not fit a WAV"), cuts.open_output(path, cuts.MAX_WAV_FRAMES + 1):
                pass
            self.assertFalse(path.exists(), "nothing is written")


class Generation(unittest.TestCase):
    def test_seeded_and_valid(self):
        first = cuts.generate(11, file_frames=480_000, count=16, min_spacing=4_800)
        self.assertEqual(first, cuts.generate(11, file_frames=480_000, count=16, min_spacing=4_800))
        self.assertNotEqual(first, cuts.generate(12, file_frames=480_000, count=16, min_spacing=4_800))
        self.assertEqual(len(first), 16)
        for earlier, later in zip(first, first[1:]):
            self.assertGreaterEqual(later.offset - earlier.covered_end(), 4_800)
        self.assertTrue(all(entry.covered_end() <= 480_000 and entry.offset < 480_000 for entry in first))

    def test_limits_and_mix(self):
        drops = cuts.generate(3, file_frames=100_000, count=5, min_length=10, max_length=20, zero_fraction=0.0)
        zeros = cuts.generate(3, file_frames=100_000, count=5, min_length=10, max_length=20, zero_fraction=1.0)
        self.assertTrue(all(entry.kind == "drop" and 10 <= entry.length <= 20 for entry in drops))
        self.assertTrue(all(entry.kind == "zero" for entry in zeros))
        self.assertEqual(cuts.generate(1, file_frames=10, count=0), [])
        with self.assertRaises(ValueError):
            cuts.generate(1, file_frames=100, count=16, min_length=10, max_length=10, zero_fraction=0.0)
        with self.assertRaises(cuts.CutError):
            cuts.generate(1, file_frames=100_000, count=17)

    def test_short_cards_and_many_zeros_stay_valid(self):
        # Without --min-spacing two ZEROs that draw the same gap once landed on
        # one offset, which the board refuses (CUT_OVERLAP); seed 2 did.
        for fraction in (1.0, 0.5):
            for seed in range(400):
                with self.subTest(fraction=fraction, seed=seed):
                    entries = cuts.generate(seed, file_frames=2_000, count=16, max_length=50, zero_fraction=fraction)
                    self.assertEqual(len(entries), 16)
                    self.assertTrue(all(entry.covered_end() < 2_000 for entry in entries))
                    for earlier, later in zip(entries, entries[1:]):
                        if earlier.kind == later.kind == "zero":
                            self.assertGreater(later.offset, earlier.offset)
        tight = cuts.generate(5, file_frames=16, count=16, zero_fraction=1.0)
        self.assertEqual([entry.offset for entry in tight], list(range(16)), "16 ZEROs fit a 16-frame card, one per frame")
        with self.assertRaises(ValueError):
            cuts.generate(5, file_frames=15, count=16, zero_fraction=1.0)
        result = subprocess.run([sys.executable, str(Path(cuts.__file__)), "generate", "--seed", "2", "--file-frames", "2000",
                                 "--count", "16", "--mix", "zero"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(cuts.validate(cuts.parse_command(result.stdout.strip()))), 16)

    def test_command_line(self):
        with tempfile.TemporaryDirectory() as folder:
            card = Path(folder) / "CARD.WAV"
            write_card(card, full_range(FRAMES, 5))
            result = subprocess.run([sys.executable, str(Path(cuts.__file__)), "generate", "--seed", "4", "--card", str(card),
                                     "--count", "4", "--mix", "zero", "--json"], capture_output=True, text=True, check=True)
            made = json.loads(result.stdout)
            entries = [cuts.CutEntry(**entry) for entry in made["entries"]]
            self.assertEqual(made["file_frames"], FRAMES)
            self.assertEqual(made["command"], cuts.render_command(entries))
            self.assertTrue(all(entry.kind == "zero" for entry in entries))


class Prediction(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.card = Path(self.folder.name) / "CARD.WAV"
        self.data = write_card(self.card, full_range(FRAMES, 9))

    def tearDown(self):
        self.folder.cleanup()

    def test_without_cuts_the_card_itself(self):
        result = cuts.predict(self.card, [])
        crc = zlib.crc32(self.data)
        self.assertEqual((result["uac_play_crc32_l"], result["uac_play_crc32_r"]), (crc, crc))
        self.assertEqual((result["uac_play_sent_frames"], result["uac_play_file_frames"]), (FRAMES, FRAMES))
        self.assertEqual((result["uac_play_cut_count"], result["cut_first_position"]), (0, None))

    def test_zero_only_and_drop_only(self):
        zeros = cuts.parse_entries("0:100:zero,10000:4800:zero,19999:1:zero")
        drops = cuts.parse_entries("0:100:drop,10000:4800:drop,19990:100:drop")
        for entries, sent, zero_frames, drop_frames in ((zeros, FRAMES + 4901, 4901, 0), (drops, FRAMES - 4910, 0, 4910)):
            with self.subTest(entries=entries):
                result = cuts.predict(self.card, entries, first_frame_index=4800)
                self.assertEqual(result["uac_play_sent_frames"], sent)
                self.assertEqual(result["uac_play_crc32_l"], zlib.crc32(expected_bytes(self.data, entries)))
                self.assertEqual((result["uac_play_cut_zero_frames"], result["uac_play_cut_drop_frames"]), (zero_frames, drop_frames))
                self.assertEqual(result["uac_play_file_frames"], FRAMES)
                self.assertEqual(result["uac_play_cut_count"], 3)
                self.assertEqual(result["uac_play_cut_first_index"], 4800)
                self.assertEqual(result["uac_play_last_frame_index"] - 4800 + 1, sent)

    def test_past_end_and_clipping(self):
        entries = cuts.parse_entries("19995:100:drop,20095:5:zero,30000:1:drop")
        result = cuts.predict(self.card, entries)
        self.assertEqual((result["uac_play_cut_rejected"], result["uac_play_cut_drop_frames"]), (2, 5))
        self.assertEqual((result["uac_play_sent_frames"], result["uac_play_cut_count"]), (FRAMES - 5, 1))
        # The entries past the end count once the whole stream was sent, in no shorter prefix.
        short = cuts.predict(self.card, entries, sent=FRAMES - 6)
        self.assertEqual((short["uac_play_cut_rejected"], short["uac_play_cut_count"]), (0, 0))

    def test_prefix_and_output_wav(self):
        entries = cuts.parse_entries("1000:4800:zero,9000:2000:drop")
        expected = expected_bytes(self.data, entries)
        output = Path(self.folder.name) / "EXPECTED.WAV"
        stopped = cuts.predict(self.card, entries, sent=3000, output_wav=output)
        self.assertEqual(stopped["uac_play_crc32_l"], zlib.crc32(expected[:9000]))
        self.assertEqual((stopped["uac_play_cut_count"], stopped["uac_play_cut_zero_frames"]), (1, 2000))
        self.assertEqual(stopped["uac_play_file_frames"], 1000)
        self.assertEqual(output.read_bytes()[44:44 + 9000], expected[:9000])
        before_drop = cuts.predict(self.card, entries, sent=9000 + 4800)
        self.assertEqual((before_drop["uac_play_cut_count"], before_drop["uac_play_cut_drop_frames"]), (1, 0))
        after_drop = cuts.predict(self.card, entries, sent=9000 + 4800 + 1)
        self.assertEqual((after_drop["uac_play_cut_count"], after_drop["uac_play_cut_drop_frames"]), (2, 2000))
        self.assertEqual(after_drop["uac_play_file_frames"], 11001)
        with self.assertRaises(ValueError):
            cuts.predict(self.card, entries, sent=FRAMES + 4800 - 2000 + 1)


def firmware_predict(data_path: Path, command: str, sent: int | None = None) -> dict:
    """The firmware's cut stage and log over raw PCM24 data (WFG_CUT_PREDICTOR --predict)."""
    arguments = [os.environ["WFG_CUT_PREDICTOR"], "--predict", str(data_path), command]
    if sent is not None:
        arguments.append(str(sent))
    result = subprocess.run(arguments, capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


def data_chunk(path: Path) -> bytes:
    """A mono PCM24 WAV's data chunk."""
    offset, frames = cuts.card_data(path)
    with path.open("rb") as wav:
        wav.seek(offset)
        return wav.read(frames * 3)


@unittest.skipUnless(os.environ.get("WFG_CUT_PREDICTOR"), "WFG_CUT_PREDICTOR names the firmware stage's predictor (ctest sets it)")
class FirmwareCrossCheck(unittest.TestCase):
    """The Python predictor against the firmware's cut stage and log on the same card."""

    def firmware(self, data_path: Path, command: str, sent: int | None) -> dict:
        return firmware_predict(data_path, command, sent)

    def test_schedules_and_prefixes(self):
        schedules = ["CUT", "CUT:0:100:zero", "CUT:0:100:drop", "CUT:19950:100:drop", "CUT:19999:4800:zero",
                     "CUT:5000:480:zero,5000:480:drop", "CUT:1000:50:drop,1050:50:zero,1050:25:drop",
                     "CUT:10:1:zero,19999:1:drop,20000:1:zero,25000:4800:drop"]
        for seed in range(6):
            schedules.append(cuts.render_command(cuts.generate(seed, file_frames=FRAMES, count=16, min_length=1, max_length=1200,
                                                               zero_fraction=seed / 5)))
        with tempfile.TemporaryDirectory() as folder:
            card = Path(folder) / "CARD.WAV"
            data = write_card(card, full_range(FRAMES, 21))
            raw = Path(folder) / "CARD.PCM"
            raw.write_bytes(data)
            for command in schedules:
                entries = cuts.parse_command(command)
                whole = cuts.predict(card, entries)
                prefixes = sorted({0, 1, whole["stream_frames"] // 3, whole["stream_frames"] - 1,
                                   *(cut.position + delta for cut in cuts.apply(entries, FRAMES).applied for delta in (-1, 0, 1))})
                for sent in [None, *prefixes]:
                    if sent is not None and not 0 <= sent <= whole["stream_frames"]:
                        continue
                    with self.subTest(command=command, sent=sent):
                        mine = cuts.predict(card, entries, sent=sent)
                        theirs = self.firmware(raw, command, sent)
                        for key, value in theirs.items():
                            self.assertEqual(mine[key], value, key)


# A scenario whose every output frame depends on where it lies in the engine's
# time (a fade, AWGN, a CW carrier), and one whose output is longer than its
# card (100 samples duplicated at 0.1 s).
CHANNEL = {"schema": "signal-lab.scenario/1", "test_id": "cut-channel", "seed": 7, "reference": "auto", "reference_rms": 0.5,
           "source_gain_db": -12, "clip_policy": "saturate",
           "impairments": [{"type": "fade", "depth_db": 6, "duration_ms": 50, "first_seconds": 0.1, "period_seconds": 0.2},
                           {"type": "awgn", "snr_db": 20}, {"type": "cw", "frequency_hz": 900, "ci_db": 12}]}
SLIP = {"schema": "signal-lab.scenario/1", "test_id": "cut-slip", "seed": 3, "reference": "auto", "reference_rms": 0.5,
        "source_gain_db": -12, "clip_policy": "saturate",
        "impairments": [{"type": "awgn", "snr_db": 30},
                        {"type": "sample_slip", "kind": "duplicate", "length_samples": 100, "placement": "single", "at_seconds": 0.1}]}


@unittest.skipUnless(os.environ.get("WFG_SIGNAL_LAB_RENDER") and os.environ.get("WFG_CUT_PREDICTOR"),
                     "WFG_SIGNAL_LAB_RENDER and WFG_CUT_PREDICTOR name the host renderer and the firmware stage's predictor (ctest sets them)")
class RenderPrediction(unittest.TestCase):
    """A scenario play's cuts act on the engine's output: the prediction is the
    schedule applied to the host render, by this module and by the firmware's
    own stage, and the expected stream is the render with the cuts made."""

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.root = Path(self.folder.name)
        self.render = Path(os.environ["WFG_SIGNAL_LAB_RENDER"])
        self.card = self.root / "CARD.WAV"
        write_card(self.card, full_range(FRAMES, 13))

    def tearDown(self):
        self.folder.cleanup()

    def scenario(self, name: str, document: dict) -> Path:
        path = self.root / name
        path.write_text(json.dumps(document), encoding="utf-8", newline="\n")
        return path

    def check_on_render(self, scenario: Path, command: str, seed: int | None = None) -> dict:
        entries = cuts.parse_command(command)
        tag = scenario.stem + ("" if seed is None else f"-{seed}")
        rendered = self.root / f"{tag}-RENDER.WAV"
        expected_wav = self.root / f"{tag}-EXPECTED.WAV"
        result = cuts.predict_render(self.card, scenario, self.render, entries, seed=seed, render_wav=rendered, output_wav=expected_wav)
        render_data = data_chunk(rendered)
        raw = self.root / f"{tag}-RENDER.PCM"
        raw.write_bytes(render_data)
        on_render = {name: value for name, value in result.items() if name != "engine_status"}
        self.assertEqual(cuts.predict(rendered, entries), {**on_render, "card_frames": len(render_data) // 3},
                         "predict_render is predict on the render")
        for key, value in firmware_predict(raw, command).items():
            self.assertEqual(result[key], value, key)
        expected = expected_bytes(render_data, entries)
        self.assertEqual(data_chunk(expected_wav), expected, "the expected stream is the render with the cuts made")
        self.assertEqual(result["uac_play_crc32_l"], zlib.crc32(expected))
        self.assertEqual((result["card_frames"], result["output_frames"]), (FRAMES, len(render_data) // 3))
        self.assertEqual((result["engine_status"]["engine_frames_in"], result["engine_status"]["engine_frames_out"]), (FRAMES, len(render_data) // 3),
                         "the STATUS engine fields come from the renderer's summary")
        return result

    def test_cuts_act_on_the_render(self):
        channel = self.scenario("CHANNEL.SCN", CHANNEL)
        command = "CUT:3000:480:drop,9000:480:zero,12000:4800:drop,19999:100:zero"
        result = self.check_on_render(channel, command)
        self.assertEqual((result["uac_play_sent_frames"], result["uac_play_cut_count"], result["uac_play_file_frames"]),
                         (FRAMES - 480 - 4800 + 480 + 100, 4, FRAMES))
        on_card = cuts.predict(self.card, cuts.parse_command(command))
        self.assertNotEqual(result["uac_play_crc32_l"], on_card["uac_play_crc32_l"], "a scenario play is not predicted from its card")
        seeded = self.check_on_render(channel, command, seed=8)
        self.assertNotEqual(seeded["uac_play_crc32_l"], result["uac_play_crc32_l"], "the seed reaches the render")

    def test_offsets_count_output_frames(self):
        slip = self.scenario("SLIP.SCN", SLIP)
        result = self.check_on_render(slip, "CUT:4000:50:zero,10000:480:drop,20050:100:drop")
        self.assertEqual(result["output_frames"], FRAMES + 100, "the slip lengthens the output past the card")
        self.assertEqual((result["uac_play_cut_count"], result["uac_play_cut_rejected"], result["uac_play_cut_drop_frames"]), (3, 0, 480 + 50),
                         "a DROP past the card's last frame applies inside the output, cut short at its end")

    def test_command_line(self):
        channel = self.scenario("CHANNEL.SCN", CHANNEL)
        command = "CUT:100:10:zero,5000:20:drop"
        arguments = [sys.executable, str(Path(cuts.__file__)), "predict", "--card", str(self.card), "--cut", command]
        result = subprocess.run(arguments + ["--scenario", str(channel), "--render", str(self.render), "--seed", "9"],
                                capture_output=True, text=True, check=True)
        self.assertEqual(json.loads(result.stdout), cuts.predict_render(self.card, channel, self.render, cuts.parse_command(command), seed=9))
        for extra in (["--scenario", str(channel)], ["--render", str(self.render)], ["--seed", "1"]):
            with self.subTest(extra=extra):
                refused = subprocess.run(arguments + extra, capture_output=True, text=True)
                self.assertEqual(refused.returncode, 2, refused.stderr)
        broken = self.root / "BROKEN.SCN"
        broken.write_text("{bad scenario}", encoding="utf-8")
        with self.assertRaises(cuts.RenderError):
            cuts.predict_render(self.card, broken, self.render, [])


# Image A's CUT rows on FSRAND.WAV (build/main/bringup/w4/CHECKLIST.md row 6,
# w4/expected-values.json), as the bench measured them on 2057673 at
# 2026-10-09 01:28-02:00Z, where the cuts acted on the card before the
# engine. A clean card's output is the card, so after the channel they are
# the same. (command, cut_count, drop, zero, rejected, sent, first position, CRC)
FSRAND_FRAMES = 2_880_000
FSRAND_CRC = 0x93B97321
IMAGE_A_ROWS = {
    "6.1 ZERO only": ("CUT:480000:144:zero,1440000:2400:zero,2400000:4800:zero", 3, 0, 7344, 0, 2_887_344, 480_000, 0xF8EA55BF),
    "6.2 DROP only": ("CUT:480000:144:drop,1440000:2400:drop,2400000:4800:drop", 3, 7344, 0, 0, 2_872_656, 480_000, 0xD4A76BBC),
    "6.3 mixed, 16": ("CUT:48000:1:zero,96001:1:drop,240000:4800:zero,240000:4800:drop,480003:4800:drop,484803:480:zero,720000:2400:zero,"
                      "960007:2400:drop,962407:144:drop,1200000:144:zero,1440000:4800:zero,1680001:4800:drop,1920000:1:zero,1920000:1:drop,"
                      "2400000:999:zero,2879999:4800:drop", 16, 16947, 13625, 0, 2_876_678, 48_000, 0x0AD9F805),
    "6.4 past the end": ("CUT:960000:2400:zero,2880000:4800:drop", 1, 0, 2400, 1, 2_882_400, 960_000, 0x5ED0E17A),
    "6.5 seeded": ("CUT:249985:4776:zero,311356:507:zero,504653:4728:drop,580974:4797:zero,954749:3250:drop,1035966:407:zero,1159831:1812:zero,"
                   "1601291:382:drop,1686226:4561:zero,1757988:1091:zero,2004946:2373:zero,2098308:3434:zero,2377812:1182:zero,2533153:4430:drop,"
                   "2637665:965:zero,2814903:4677:zero", 16, 12790, 30582, 0, 2_897_792, 249_985, 0x64A4A2D3),
}


@unittest.skipUnless(os.environ.get("WFG_FSRAND_WAV") and os.environ.get("WFG_CUT_PREDICTOR"),
                     "WFG_FSRAND_WAV names image A's CUT card (cmake -DWFG_FSRAND_WAV=...); WFG_CUT_PREDICTOR the firmware stage's predictor")
class ImageARows(unittest.TestCase):
    """Image A's CUT rows 6.1 to 6.5 on the host: this module and the
    firmware's cut stage give the values the bench measured, unchanged."""

    @classmethod
    def setUpClass(cls):
        cls.card = Path(os.environ["WFG_FSRAND_WAV"])
        cls.folder = tempfile.TemporaryDirectory()
        cls.raw = Path(cls.folder.name) / "FSRAND.PCM"
        data = data_chunk(cls.card)
        cls.raw.write_bytes(data)
        cls.data_crc = zlib.crc32(data)
        cls.sha256 = hashlib.sha256(cls.card.read_bytes()).hexdigest()

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def test_the_card_is_image_as(self):
        self.assertEqual((cuts.card_data(self.card)[1], self.data_crc), (FSRAND_FRAMES, FSRAND_CRC))
        self.assertEqual(self.sha256, "7bbbefe6fc44bd89d172bb6ebc795c951cc7cd0eb2596d24ee9188c8bc260866")

    def test_rows(self):
        for row, (command, count, drop, zero, rejected, sent, first, crc) in IMAGE_A_ROWS.items():
            expected = {"uac_play_cut_count": count, "uac_play_cut_drop_frames": drop, "uac_play_cut_zero_frames": zero,
                        "uac_play_cut_rejected": rejected, "uac_play_sent_frames": sent, "uac_play_file_frames": FSRAND_FRAMES,
                        "cut_first_position": first, "uac_play_crc32_l": crc, "uac_play_crc32_r": crc, "stream_frames": sent}
            with self.subTest(row=row):
                host = cuts.predict(self.card, cuts.parse_command(command), first_frame_index=0)
                board = firmware_predict(self.raw, command)
                self.assertEqual({key: host[key] for key in expected}, expected, "host/cut_schedule.py")
                self.assertEqual({key: board[key] for key in expected}, expected, "the firmware's cut stage")
                self.assertEqual((host["uac_play_cut_first_index"], host["uac_play_last_frame_index"]), (first, sent - 1))

    def test_seeded_row_is_the_generators(self):
        made = cuts.generate(7, file_frames=FSRAND_FRAMES, count=16, zero_fraction=0.5, min_spacing=48_000)
        self.assertEqual(cuts.render_command(made), IMAGE_A_ROWS["6.5 seeded"][0])


if __name__ == "__main__":
    unittest.main()
