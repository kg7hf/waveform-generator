# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""PLAYLIST files on the host (host/make_playlist.py): the board's parsing
rules with their codes and line numbers, cross-checked file by file against
the firmware's own parser (tests/playlist_tests.cpp --check, named by the
WFG_PLAYLIST_CHECKER environment variable that ctest sets); writing a file
from JSON items; the card checks; the predicted block values of each item:
frames sent, both CRCs, the cuts, the scenario's CRC and engine state, where
each USB item starts (worked by hand, and against the firmware's
next_start_index() through --next-start), rendered scenario items (the host
renderer, WFG_SIGNAL_LAB_RENDER) and the expected streams as WAVs; and split,
which cuts a battery's capture into one piece per item. No board, no ports."""

import io
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "host"))
import attenuation as att
import cut_schedule as cuts
import make_playlist as mp


def write_card(path: Path, samples: list[int]) -> bytes:
    """A mono 48 kHz PCM24 WAV of the samples; returns its data chunk."""
    data = b"".join((sample & 0xFFFFFF).to_bytes(3, "little") for sample in samples)
    header = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + len(data) + len(data) % 2, b"WAVE", b"fmt ", 16, 1, 1,
                         48_000, 144_000, 3, 24, b"data", len(data))
    path.write_bytes(header + data + b"\0" * (len(data) % 2))
    return data


def full_range(count: int, seed: int) -> list[int]:
    rng = random.Random(seed)
    return [-(1 << 23), (1 << 23) - 1, 0] + [rng.randint(-(1 << 23), (1 << 23) - 1) for _ in range(count - 3)]


BATTERY = (b"# lane acceptance battery: six items, 5 s gaps, USB route\r\n"
           b"C24L05.WAV 5 USB label=C24L05\r\n"
           b"C12L05.WAV 5 USB label=C12L05   # comment after an item\r\n"
           b"\r\n"
           b"C06L05.WAV\t5\tUSB\tlabel=C06L05\r\n"
           b"FSRAND.WAV 5 USB label=FSRAND cut=48000:480:zero,96000:480:drop\r\n"
           b"   # an indented comment\r\n"
           b"C06L05N.WAV 5 USB label=C06L05N scn=NOISE.SCN seed=4294967295\r\n"
           b"C24L10.WAV 5.000 USB label=C24L10")


def with_bad_line(bad: bytes, line: int) -> bytes:
    """A file whose line `line` is `bad`, after comments and blank lines, then one more item."""
    head = b"".join(b"\n" if before % 2 == 0 else b"# comment\n" for before in range(1, line))
    return head + bad + b"\nLAST.WAV 0 CODEC\n"


SEVENTEEN = b"A.WAV 0 USB cut=" + b",".join(f"{index * 10}:1:zero".encode() for index in range(17))
# Every printable character, space to "~": a comment may hold each of them.
PRINTABLE = bytes(range(0x20, 0x7F))
LONGEST_ITEM = b"A.WAV 3600 USB label=" + b"x" * 24 + b" scn=F.SCN seed=4294967295 # "
LONGEST = LONGEST_ITEM + b"#" * (mp.MAX_LINE_CHARS - len(LONGEST_ITEM) - 1) + b"\r"
# (file, board code, line): the C++ test's refusals and the edges of the limits.
CASES = [
    (BATTERY, "OK", 10),
    (with_bad_line(b"A.WAV 5", 3), "PLAYLIST_SYNTAX", 3),
    (with_bad_line(b"A.WAV", 1), "PLAYLIST_SYNTAX", 1),
    (with_bad_line(b"A.WAV 5 USB foo=1", 4), "PLAYLIST_SYNTAX", 4),
    (with_bad_line(b"A.WAV 5 USB label=a label=b", 2), "PLAYLIST_SYNTAX", 2),
    (with_bad_line(b"A.WAV 5 USB label=a label=b/", 2), "PLAYLIST_SYNTAX", 2),
    (with_bad_line(b"A.WAV 5 USB LABEL=a", 2), "PLAYLIST_SYNTAX", 2),
    (with_bad_line(b"A.WAV 5 USB\x01", 5), "PLAYLIST_SYNTAX", 5),
    (with_bad_line(b"A.WAV 5 USB # caf\xc3\xa9", 6), "PLAYLIST_SYNTAX", 6),
    (with_bad_line(b"A.WAV 5 USB # \x7f", 5), "PLAYLIST_SYNTAX", 5),
    (b"# " + PRINTABLE + b"\nA.WAV 0 CODEC # " + PRINTABLE + b"\n", "OK", 3),
    (with_bad_line(b"A.WAV 5\rUSB", 2), "PLAYLIST_SYNTAX", 2),
    (with_bad_line(b"\xef\xbb\xbfA.WAV 5 USB", 1), "PLAYLIST_SYNTAX", 1),
    (with_bad_line(b"a.wav 5 USB", 2), "PLAYLIST_FILE", 2),
    (with_bad_line(b"TOOLONGNA.WAV 5 USB", 2), "PLAYLIST_FILE", 2),
    (with_bad_line(b"../A.WAV 5 USB", 2), "PLAYLIST_FILE", 2),
    (with_bad_line(b"A.WAV 5s USB", 7), "PLAYLIST_GAP", 7),
    (with_bad_line(b"A.WAV 3600.5 USB", 7), "PLAYLIST_GAP", 7),
    (with_bad_line(b"A.WAV 05 USB", 7), "PLAYLIST_GAP", 7),
    (with_bad_line(b"A.WAV 5 usb", 3), "PLAYLIST_ROUTE", 3),
    (with_bad_line(b"A.WAV 5 codec", 3), "PLAYLIST_ROUTE", 3),
    (with_bad_line(b"A.WAV 5 Codec", 3), "PLAYLIST_ROUTE", 3),
    (with_bad_line(b"A.WAV 5 USB#x", 3), "PLAYLIST_ROUTE", 3),
    (with_bad_line(b"A.WAV 5 USB label=", 2), "PLAYLIST_LABEL", 2),
    (with_bad_line(b"A.WAV 5 USB label=" + b"x" * 25, 2), "PLAYLIST_LABEL", 2),
    (b"A.WAV 0 USB label=one\nB.WAV 0 USB\n# again\nC.WAV 0 USB label=one\n", "PLAYLIST_LABEL_DUPLICATE", 4),
    (with_bad_line(b"A.WAV 5 USB scn=fade.scn", 2), "PLAYLIST_SCN", 2),
    (with_bad_line(b"A.WAV 5 USB scn=F.SCN seed=01", 2), "PLAYLIST_SEED", 2),
    (with_bad_line(b"A.WAV 5 USB scn=F.SCN seed=4294967296", 2), "PLAYLIST_SEED", 2),
    (with_bad_line(b"A.WAV 5 USB seed=7", 3), "PLAYLIST_SEED_NO_SCN", 3),
    (with_bad_line(b"A.WAV 5 USB cut=1:1", 2), "PLAYLIST_CUT", 2),
    (with_bad_line(b"A.WAV 5 USB cut=1:1:drop,", 2), "PLAYLIST_CUT", 2),
    (with_bad_line(SEVENTEEN, 2), "PLAYLIST_CUT", 2),
    (with_bad_line(b"A.WAV 5 USB cut=1:4801:zero", 2), "CUT_RANGE", 2),
    (with_bad_line(b"A.WAV 5 USB cut=200:10:zero,100:10:drop", 2), "CUT_UNSORTED", 2),
    (with_bad_line(b"A.WAV 5 USB cut=100:10:drop,105:1:zero", 2), "CUT_OVERLAP", 2),
    (with_bad_line(b"A.WAV 5 USB cut=0:4800:drop,4800:4800:drop,9600:4800:drop,14400:1:drop", 2), "CUT_DROP_RUN", 2),
    (b"A.WAV 0 USB cut=0:4800:drop,4800:4800:drop,9600:4800:drop,19200:4800:drop\n", "OK", 2),
    (b"A.WAV 0 USB att=0\nB.WAV 0 CODEC att=-0.1\nC.WAV 0 USB att=-20 label=c\nD.WAV 0 USB att=-40.0 cut=1:1:zero\nE.WAV 0 USB att=-0\n", "OK", 6),
    (with_bad_line(b"A.WAV 5 USB att=3", 4), "PLAYLIST_ATT", 4),
    (with_bad_line(b"A.WAV 5 USB att=0.5", 4), "PLAYLIST_ATT", 4),
    (with_bad_line(b"A.WAV 5 USB att=-5.25", 3), "PLAYLIST_ATT", 3),
    (with_bad_line(b"A.WAV 5 USB att=-40.1", 3), "PLAYLIST_ATT", 3),
    (with_bad_line(b"A.WAV 5 USB att=-05", 3), "PLAYLIST_ATT", 3),
    (with_bad_line(b"A.WAV 5 USB att=", 3), "PLAYLIST_ATT", 3),
    (with_bad_line(b"A.WAV 5 USB att=-1 att=-2", 2), "PLAYLIST_SYNTAX", 2),
    (b"A.WAV 0 USB dur=1\nB.WAV 0 CODEC dur=86400\nC.WAV 0 USB dur=3600.5 att=-30 cut=172800000:10:zero\nD.WAV 1 USB dur=1.001\n", "OK", 5),
    (with_bad_line(b"A.WAV 5 USB dur=0", 5), "PLAYLIST_DUR", 5),
    (with_bad_line(b"A.WAV 5 USB dur=0.999", 5), "PLAYLIST_DUR", 5),
    (with_bad_line(b"A.WAV 5 USB dur=86400.001", 5), "PLAYLIST_DUR", 5),
    (with_bad_line(b"A.WAV 5 USB dur=100000", 5), "PLAYLIST_DUR", 5),
    (with_bad_line(b"A.WAV 5 USB dur=1.2345", 5), "PLAYLIST_DUR", 5),
    (with_bad_line(b"A.WAV 5 USB dur=01", 5), "PLAYLIST_DUR", 5),
    (with_bad_line(b"A.WAV 5 USB dur=", 5), "PLAYLIST_DUR", 5),
    (with_bad_line(b"A.WAV 5 USB dur=1 dur=2", 2), "PLAYLIST_SYNTAX", 2),
    (with_bad_line(b"A.WAV 5 USB # " + b"#" * mp.MAX_LINE_CHARS, 2), "PLAYLIST_LINE_TOO_LONG", 2),
    (LONGEST + b"\n" + b"#" * mp.MAX_LINE_CHARS, "OK", 2),
    (LONGEST + b"#\n", "PLAYLIST_LINE_TOO_LONG", 1),
    (b"".join(f"I{index}.WAV 0 CODEC\n".encode() for index in range(256)), "OK", 257),
    (b"".join(f"I{index}.WAV 0 CODEC\n".encode() for index in range(257)), "PLAYLIST_TOO_MANY", 257),
    (b"# nothing but comments\n\n   \t\n", "PLAYLIST_EMPTY", 0),
    (b"", "PLAYLIST_EMPTY", 0),
]


def host_verdict(data: bytes) -> tuple[str, int, list]:
    try:
        return "OK", 0, mp.parse_bytes(data)
    except mp.PlaylistError as error:
        return error.code, error.line, []


class ParserTests(unittest.TestCase):
    def test_codes_and_lines(self):
        for data, code, line in CASES:
            with self.subTest(data=data[:60]):
                verdict, at, _ = host_verdict(data)
                self.assertEqual(verdict, code)
                if code != "OK":
                    self.assertEqual(at, line)

    def test_battery(self):
        items = mp.parse_bytes(BATTERY)
        self.assertEqual([item.file for item in items], ["C24L05.WAV", "C12L05.WAV", "C06L05.WAV", "FSRAND.WAV", "C06L05N.WAV", "C24L10.WAV"])
        self.assertEqual([item.line for item in items], [2, 3, 5, 6, 8, 9])
        self.assertTrue(all(item.gap_frames == 240_000 and item.route == "usb" for item in items))
        self.assertEqual(items[3].cuts, (cuts.CutEntry(48000, 480, "zero"), cuts.CutEntry(96000, 480, "drop")))
        self.assertEqual((items[4].scenario, items[4].seed), ("NOISE.SCN", (1 << 32) - 1))

    def test_gaps(self):
        for text, frames in (("0", 0), ("5", 240_000), ("0.5", 24_000), ("0.25", 12_000), ("0.001", 48), ("3600", 172_800_000), ("10.010", 480_480)):
            with self.subTest(text=text):
                self.assertEqual(mp.parse_gap(text), frames)
                self.assertEqual(mp.parse_gap(mp.format_gap(frames)), frames)
        for text in ("", "-1", "+5", "05", "1.", ".5", "1.2345", "3600.001", "9999", "1e3", "5s"):
            with self.subTest(text=text):
                with self.assertRaises(mp.PlaylistError):
                    mp.parse_gap(text)
        self.assertEqual(mp.gap_from_json(0.25), "0.25")
        self.assertEqual(mp.gap_from_json(5), "5")
        with self.assertRaises(mp.PlaylistError):
            mp.gap_from_json(0.0005)

    def test_comment_characters(self):
        self.assertEqual(len(PRINTABLE), 95)
        items = mp.parse_bytes(b"# " + PRINTABLE + b"\nA.WAV 0 CODEC # " + PRINTABLE + b"\n")
        self.assertEqual([(item.file, item.route, item.line, item.label) for item in items], [("A.WAV", "codec", 2, None)])

    def test_names(self):
        self.assertTrue(all(mp.is_playlist_name(name) for name in ("BATTERY.PL", "LADDER", "ABCDEFGH.XYZ")))
        self.assertFalse(any(mp.is_playlist_name(name) for name in ("battery.pl", "ABCDEFGHI.PL", "A.PLAY", ".PL", "A.")))

    def test_codec_only_image(self):
        with self.assertRaises(mp.PlaylistError) as refused:
            mp.parse_bytes(b"# x\nA.WAV 0 USB\n", usb_offered=False)
        self.assertEqual((refused.exception.code, refused.exception.line), ("PLAYLIST_ROUTE_UNAVAILABLE", 2))

    @unittest.skipUnless(os.environ.get("WFG_PLAYLIST_CHECKER"), "the firmware parser's checker is named by WFG_PLAYLIST_CHECKER (ctest)")
    def test_matches_the_firmware_parser(self):
        checker = os.environ["WFG_PLAYLIST_CHECKER"]
        with tempfile.TemporaryDirectory() as folder:
            for index, (data, _, _) in enumerate(CASES):
                with self.subTest(data=data[:60]):
                    path = Path(folder) / f"CASE{index}.PL"
                    path.write_bytes(data)
                    board = json.loads(subprocess.run([checker, "--check", str(path)], capture_output=True, text=True, check=True).stdout)
                    code, line, items = host_verdict(data)
                    self.assertEqual(board["code"], code)
                    if code != "OK":
                        self.assertEqual(board["line"], line)
                        continue
                    host = mp.summary("X", items)["items"]
                    self.assertEqual(board["items"], host)


class AttenuationTests(unittest.TestCase):
    """att=: the gains' closed form, the firmware's table written from it, the arithmetic, and both against the firmware's."""

    def test_table_follows_its_closed_form(self):
        table = att.table()
        self.assertEqual((len(table), table[0], table[200], table[400]), (401, 1 << 31, 214_748_365, 21_474_836))
        for tenths, gain in enumerate(table):
            self.assertLessEqual(abs(gain - (1 << 31) * 10 ** (-tenths / 200)), 0.5 + 1e-6, tenths)
        self.assertTrue(all(louder > softer for louder, softer in zip(table, table[1:])), "the gain falls with every tenth")

    def test_the_firmware_header_is_the_generated_table(self):
        self.assertEqual((ROOT / att.HEADER_PATH).read_bytes(), att.header(), "common/attenuation_gains.hpp is host/attenuation.py's table")

    def test_arithmetic(self):
        unity, tenth = att.table()[0], att.table()[200]
        for sample in (-(1 << 23), (1 << 23) - 1, -1, 0, 1, *full_range(1000, 11)):
            self.assertEqual(att.apply(sample, unity), sample, "0 dB leaves every sample")
        self.assertEqual([att.apply(sample, tenth) for sample in (10_000, -10_000, 5, -5, 0)], [1000, -1000, 1, -1, 0])
        self.assertEqual([att.apply(sample, 1 << 30) for sample in (1, 3, -1, -3)], [0, 2, 0, -2], "ties round to even")
        self.assertTrue(all(att.apply(0, gain) == 0 for gain in att.table()), "zero stays zero")

    def test_spellings(self):
        for tenths in range(att.ENTRIES):
            self.assertEqual(att.parse(att.spelling(tenths)), tenths)
        self.assertEqual([att.parse(text) for text in ("0", "0.0", "-0", "-20", "-20.0", "-0.5", "-40")], [0, 0, 0, 200, 200, 5, 400])
        for text in ("1", "0.5", "+1", "-40.1", "-41", "-5.25", "-05", "-.5", "-5.", "", "-", "--1"):
            with self.subTest(text=text), self.assertRaises(att.AttenuationError):
                att.parse(text)
        self.assertEqual([mp.attenuation_from_json(value) for value in (-20, -6.5, 0, "-1.5")], ["-20", "-6.5", "0", "-1.5"])

    @unittest.skipUnless(os.environ.get("WFG_PLAYLIST_CHECKER"), "the firmware's table is printed by WFG_PLAYLIST_CHECKER (ctest)")
    def test_matches_the_firmware(self):
        checker = os.environ["WFG_PLAYLIST_CHECKER"]
        board = json.loads(subprocess.run([checker, "--gain-table"], capture_output=True, text=True, check=True).stdout)
        self.assertEqual(board, list(att.table()), "the firmware's gains, entry by entry")
        samples = [-(1 << 23), (1 << 23) - 1, -2, -1, 0, 1, 2, *full_range(300, 12)]
        for tenths in (0, 1, 5, 65, 199, 200, 201, 333, 399, 400):
            with self.subTest(tenths=tenths):
                attenuated = json.loads(subprocess.run([checker, "--attenuate", str(tenths), *(str(sample) for sample in samples)],
                                                       capture_output=True, text=True, check=True).stdout)
                self.assertEqual(attenuated, [att.apply(sample, att.table()[tenths]) for sample in samples], "the firmware's samples, one by one")


# The board's own noise scenario, as a lane scenario card would be (the
# player needs its reference_rms).
NOISE = {"schema": "signal-lab.scenario/1", "test_id": "noise", "seed": 42, "reference": "auto", "reference_rms": 0.25,
         "source_gain_db": -12, "clip_policy": "saturate", "impairments": [{"type": "awgn", "snr_db": 20}]}
# Eleven items: chained runs, a codec item, a scenario, a slip of unknown
# length, a gap of 0 and one under a second, and a card whose every frame
# is dropped.
PREDICTED = (b"A.WAV 5 USB label=a\nB.WAV 0.25 USB label=b cut=100:48:drop,1000:10:zero\nC.WAV 0.5 CODEC label=c\n"
             b"A.WAV 1 USB label=d\nD.WAV 2 USB label=e scn=NOISE.SCN seed=7\nB.WAV 0 USB label=f\n"
             b"C.WAV 1 USB label=g scn=SLIP.SCN\nD.WAV 0.5 USB label=h\nA.WAV 1 USB label=i\nE.WAV 3 USB label=j cut=0:480:drop\n"
             b"B.WAV 0 USB label=k\n")
# Each item's (scheduled, start_late, run_start_item, start_offset_frames),
# worked by hand: a 5002-frame card ends 5040 frames (105 packets) after its
# first, a 2000-frame card 2016 (42 packets).
PREDICTED_STARTS = [(False, 0, 0, 0), (True, 0, 0, 5040 + 240_000), (None, None, None, None), (False, 0, 3, 0), (True, 0, 3, 5040 + 48_000),
                    (True, 0, 3, 5040 + 48_000 + 2016 + 96_000), (True, 1, 6, 0), (None, 0, 7, 0), (True, None, 8, 0), (True, 0, 8, 5040 + 48_000),
                    (False, 0, 10, 0)]


def stereo_f32(frames: list[int]) -> bytes:
    """Mono PCM24 codes as the recorder's float32 stereo frames, L = R."""
    return b"".join(struct.pack("<ff", code / 8388608.0, code / 8388608.0) for code in frames)


def write_f32_capture(path: Path, data: bytes) -> None:
    """A stereo float32 48 kHz WAV, as the lane recorder writes its .f32.wav."""
    header = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + len(data), b"WAVE", b"fmt ", 16, 3, 2, 48_000, 384_000, 8, 32, b"data", len(data))
    path.write_bytes(header + data)


class WriteAndPredictTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.cards = Path(self.folder.name)
        self.data = {}
        for name, frames, seed in (("A.WAV", 5002, 1), ("B.WAV", 4800, 2), ("C.WAV", 3002, 3), ("D.WAV", 2000, 4), ("E.WAV", 480, 5)):
            self.data[name] = write_card(self.cards / name, full_range(frames, seed))
        (self.cards / "NOISE.SCN").write_text(json.dumps(NOISE))
        (self.cards / "SLIP.SCN").write_text(json.dumps({"schema": "signal-lab.scenario/1",
                                                          "impairments": [{"type": "sample_slip", "events": [{"at_seconds": 0.01}]}]}))

    def tearDown(self):
        self.folder.cleanup()

    def run_main(self, argv):
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch("sys.stdout", stdout), mock.patch("sys.stderr", stderr):
            code = mp.main(argv)
        return code, stdout.getvalue(), stderr.getvalue()

    def test_write_then_check_round_trips(self):
        items = [{"file": "A.WAV", "gap": 5, "route": "USB", "label": "a"},
                 {"file": "B.WAV", "gap": "0.25", "route": "USB", "label": "b", "cut": "100:48:drop,1000:10:zero"},
                 {"file": "C.WAV", "gap": 0.5, "route": "CODEC"},
                 {"file": "D.WAV", "gap": 0, "route": "USB", "scn": "NOISE.SCN", "seed": 7}]
        spec = self.cards / "items.json"
        spec.write_text(json.dumps(items))
        output = self.cards / "TEST.PL"
        code, stdout, _ = self.run_main(["write", "--items", str(spec), "--output", str(output), "--comment", "a test"])
        self.assertEqual(code, 0)
        self.assertTrue(output.read_bytes().startswith(b"# a test\nA.WAV 5 USB label=a\nB.WAV 0.25 USB label=b cut=100:48:drop,1000:10:zero\n"))
        written = json.loads(stdout)["items"]
        self.assertEqual([item["line"] for item in written], [2, 3, 4, 5])
        code, _, stderr = self.run_main(["write", "--items", str(spec), "--output", str(output)])
        self.assertEqual(code, 2, "an existing playlist is never replaced without --force")
        spec.write_text(json.dumps([{"file": "A.WAV", "gap": 5, "route": "USB", "seed": 1}]))
        code, _, stderr = self.run_main(["write", "--items", str(spec), "--output", str(self.cards / "BAD.PL")])
        self.assertEqual(code, 2)
        self.assertIn("PLAYLIST_SEED_NO_SCN", stderr)
        code, _, stderr = self.run_main(["write", "--items", str(spec), "--output", str(self.cards / "bad.pl")])
        self.assertIn("8.3", stderr)

    def check_predicted(self, *extra):
        playlist = self.cards / "P.PL"
        playlist.write_bytes(PREDICTED)
        expect = self.cards / "EXPECT.json"
        code, stdout, stderr = self.run_main(["check", str(playlist), "--cards", str(self.cards), "--expect", str(expect), *extra])
        self.assertEqual(code, 0, stderr)
        predicted = json.loads(expect.read_text())
        self.assertEqual(predicted, json.loads(stdout))
        self.assertEqual((predicted["schema"], predicted["on_time_gap_ms"]), (mp.SCHEMA, 1000))
        return predicted["items"]

    def test_predictions(self):
        wavs = self.cards / "expected"
        items = self.check_predicted("--expected-wavs", str(wavs))
        crc_a = zlib.crc32(self.data["A.WAV"])
        self.assertEqual((items[0]["uac_play_sent_frames"], items[0]["uac_play_file_frames"], items[0]["uac_play_crc32_l"], items[0]["uac_play_crc32_r"]),
                         (5002, 5002, crc_a, crc_a))
        cut_b = cuts.predict(self.cards / "B.WAV", cuts.parse_entries("100:48:drop,1000:10:zero"))
        for name in ("uac_play_sent_frames", "uac_play_file_frames", "uac_play_crc32_l", "uac_play_cut_count", "uac_play_cut_drop_frames",
                     "uac_play_cut_zero_frames", "cut_first_position"):
            self.assertEqual(items[1][name], cut_b[name], name)
        self.assertEqual((items[2]["route"], items[2]["uac_play_sent_frames"], items[2]["uac_play_file_frames"], items[2]["uac_play_crc32_l"]),
                         ("codec", 3002, 0, 0), "a codec item has no USB play")
        self.assertEqual((items[4]["samples_predicted"], items[4]["length_predicted"], items[4]["uac_play_crc32_l"], items[4]["uac_play_sent_frames"]),
                         (False, True, None, 2000), "an AWGN scenario keeps the length, not the samples")
        self.assertEqual((items[6]["length_predicted"], items[6]["uac_play_sent_frames"]), (False, None), "a sample slip changes the length")
        self.assertEqual((items[9]["uac_play_sent_frames"], items[9]["uac_play_cut_drop_frames"]), (0, 480), "every frame of E is dropped")
        stream = cuts.apply(cuts.parse_entries("100:48:drop,1000:10:zero"), 4800)
        with (self.cards / "B.WAV").open("rb") as card:
            expected = b"".join(cuts.stream_bytes(stream, card, 44, stream.frames))
        self.assertEqual((wavs / "001-b.WAV").read_bytes()[44:44 + len(expected)], expected)
        self.assertIsNone(items[4]["expected_wav"])
        self.assertEqual((items[0]["engine_status"], items[4]["engine_status"]), (None, None), "only a rendered item predicts the engine's STATUS fields")

    def test_attenuated_items(self):
        playlist = self.cards / "ATT.PL"
        playlist.write_bytes(b"A.WAV 1 USB label=twenty att=-20.0\nB.WAV 1 CODEC label=cut att=-6.5 cut=100:48:drop,1000:10:zero\nA.WAV 1 USB att=0\n")
        wavs = self.cards / "expected"
        code, stdout, stderr = self.run_main(["check", str(playlist), "--cards", str(self.cards), "--expected-wavs", str(wavs)])
        self.assertEqual(code, 0, stderr)
        items = json.loads(stdout)["items"]

        def samples(name):
            data = self.data[name]
            return [int.from_bytes(data[at:at + 3], "little", signed=True) for at in range(0, len(data), 3)]

        def pcm24(values):
            return b"".join((value & 0xFFFFFF).to_bytes(3, "little") for value in values)

        twenty = pcm24([att.apply(sample, att.table()[200]) for sample in samples("A.WAV")])
        self.assertEqual((items[0]["att_tenths_db"], items[0]["uac_play_sent_frames"], items[0]["uac_play_crc32_l"], items[0]["uac_play_crc32_r"]),
                         (200, 5002, zlib.crc32(twenty), zlib.crc32(twenty)), "-20 dB: the card times the table's gain, rounded half to even")
        self.assertEqual((wavs / "000-twenty.WAV").read_bytes()[44:44 + len(twenty)], twenty)
        # The codec item's cut stream, attenuated: the same as the attenuated card cut.
        stream = cuts.apply(cuts.parse_entries("100:48:drop,1000:10:zero"), 4800)
        with (self.cards / "B.WAV").open("rb") as card:
            cut = b"".join(cuts.stream_bytes(stream, card, 44, stream.frames))
        cut_then_attenuated = pcm24([att.apply(int.from_bytes(cut[at:at + 3], "little", signed=True), att.table()[65]) for at in range(0, len(cut), 3)])
        self.assertEqual((wavs / "001-cut.WAV").read_bytes()[44:44 + len(cut_then_attenuated)], cut_then_attenuated)
        self.assertEqual((items[1]["att_tenths_db"], items[1]["uac_play_sent_frames"], items[1]["uac_play_crc32_l"], items[1]["uac_play_cut_count"]),
                         (65, 4800 - 48 + 10, 0, 2), "a codec item has no USB CRC; its cuts and frames are predicted")
        self.assertEqual((items[2]["att_tenths_db"], items[2]["uac_play_crc32_l"]), (0, zlib.crc32(self.data["A.WAV"])), "att=0 is the card bit for bit")
        spec = self.cards / "att.json"
        spec.write_text(json.dumps([{"file": "A.WAV", "gap": 1, "route": "USB", "att": -6.5}, {"file": "B.WAV", "gap": 1, "route": "USB", "att": -20}]))
        code, _, stderr = self.run_main(["write", "--items", str(spec), "--output", str(self.cards / "W.PL")])
        self.assertEqual((code, (self.cards / "W.PL").read_bytes()), (0, b"A.WAV 1 USB att=-6.5\nB.WAV 1 USB att=-20\n"), stderr)
        spec.write_text(json.dumps([{"file": "A.WAV", "gap": 1, "route": "USB", "att": 3}]))
        code, _, stderr = self.run_main(["write", "--items", str(spec), "--output", str(self.cards / "X.PL")])
        self.assertEqual(code, 2)
        self.assertIn("PLAYLIST_ATT line 1", stderr)

    def looped_data(self, name: str, frames: int) -> bytes:
        """The card's data looped to `frames` frames: frame 0 again after its last."""
        data = self.data[name]
        whole, rest = divmod(frames * 3, len(data))
        return data * whole + data[:rest]

    def test_looped_items(self):
        playlist = self.cards / "LOOP.PL"
        playlist.write_bytes(b"A.WAV 1 USB label=loop dur=1\nA.WAV 1 USB label=seam dur=1.5 cut=4900:4800:drop,10004:480:zero,71990:100:drop\n"
                             b"B.WAV 1 CODEC label=codec dur=2 att=-6\nA.WAV 1 USB label=once\n")
        wavs = self.cards / "expected"
        code, stdout, stderr = self.run_main(["check", str(playlist), "--cards", str(self.cards), "--expected-wavs", str(wavs)])
        self.assertEqual(code, 0, stderr)
        items = json.loads(stdout)["items"]
        loop = self.looped_data("A.WAV", 48_000)
        self.assertEqual((items[0]["dur_frames"], items[0]["input_frames"], items[0]["uac_play_sent_frames"], items[0]["uac_play_file_frames"],
                          items[0]["uac_play_crc32_l"], items[0]["loops"], items[0]["end_cause"], items[0]["engine_deadline_misses"]),
                         (48_000, 48_000, 48_000, 48_000, zlib.crc32(loop), 9, "end", 0), "a 5002-frame card looped for 1 s: 9 whole passes")
        self.assertEqual((items[0]["seam_count"], items[0]["seam_positions"], items[0]["seam_positions_complete"]),
                         (9, [5002 * pass_ for pass_ in range(1, 10)], True), "every seam where the card's frame 0 comes again")
        self.assertEqual((wavs / "000-loop.WAV").read_bytes()[44:44 + len(loop)], loop)
        # The cut stream of the looped card, built by hand: a DROP of frames
        # 4900 to 9699 across the first seam, 480 zeros at the second (10004),
        # and the last DROP cut short by the duration's end (10 frames).
        seam = self.looped_data("A.WAV", 72_000)
        cut = seam[:4900 * 3] + seam[9700 * 3:10004 * 3] + bytes(480 * 3) + seam[10004 * 3:71990 * 3]
        self.assertEqual((items[1]["uac_play_sent_frames"], items[1]["uac_play_crc32_l"], items[1]["uac_play_cut_count"], items[1]["uac_play_cut_drop_frames"],
                          items[1]["uac_play_cut_zero_frames"], items[1]["cut_first_position"], items[1]["uac_play_file_frames"], items[1]["loops"]),
                         (len(cut) // 3, zlib.crc32(cut), 3, 4810, 480, 4900, 72_000, 14), "cuts across and at seams land on the looped stream's frames")
        self.assertEqual(items[1]["seam_positions"][:3], [None, 4900 + 304 + 480, 4900 + 304 + 480 + 5002],
                         "the first seam was dropped; the second comes after the zeros inserted before it, the third a pass later")
        codec = mp.attenuated(self.looped_data("B.WAV", 96_000), att.table()[60])
        self.assertEqual((items[2]["uac_play_sent_frames"], items[2]["uac_play_crc32_l"], items[2]["stream_crc32"], items[2]["loops"]),
                         (96_000, 0, zlib.crc32(codec), 20), "a codec item loops attenuated; its block reports no CRC")
        self.assertEqual((items[3]["dur_frames"], items[3]["uac_play_sent_frames"], items[3]["loops"], items[3]["seam_count"]), (None, 5002, 1, 0),
                         "an item without dur= plays its card once")

    def test_prefix_predictions(self):
        playlist = self.cards / "PRE.PL"
        playlist.write_bytes(b"A.WAV 1 USB label=loop dur=2 cut=5000:10:zero att=-20\nB.WAV 1 CODEC label=codec dur=1\n")
        stream = self.looped_data("A.WAV", 96_000)
        cut = stream[:5000 * 3] + bytes(30) + stream[5000 * 3:]
        twenty = mp.attenuated(cut, att.table()[200])
        for sent in (0, 4999, 5000, 5005, 5010, 50_000, 96_010):
            with self.subTest(sent=sent):
                code, stdout, stderr = self.run_main(["predict", str(playlist), "--cards", str(self.cards), "--item", "0", "--sent-frames", str(sent),
                                                      "--first-frame-index", "4800"])
                self.assertEqual(code, 0, stderr)
                prefix = json.loads(stdout)
                self.assertEqual((prefix["uac_play_sent_frames"], prefix["uac_play_crc32_l"], prefix["uac_play_cut_zero_frames"]),
                                 (sent, zlib.crc32(twenty[:sent * 3]), min(10, max(0, sent - 5000))), "the CRC of every prefix of the looped stream")
                self.assertEqual(prefix["loops"], prefix["uac_play_file_frames"] // 5002)
                self.assertEqual(prefix["uac_play_last_frame_index"], 4800 + sent - 1 if sent else 0)
                self.assertIsNone(prefix["end_cause"], "a prefix may end at any cause")
        code, stdout, _ = self.run_main(["predict", str(playlist), "--cards", str(self.cards), "--item", "1", "--sent-frames", "12345"])
        codec = json.loads(stdout)
        self.assertEqual((code, codec["uac_play_crc32_l"], codec["stream_crc32"]), (0, 0, zlib.crc32(self.looped_data("B.WAV", 12_345))))
        self.assertEqual(self.run_main(["predict", str(playlist), "--cards", str(self.cards), "--item", "2"])[0], 2, "no item 2")
        self.assertEqual(self.run_main(["predict", str(playlist), "--cards", str(self.cards), "--item", "0", "--sent-frames", "96011"])[0], 2,
                         "no more frames than the stream holds")

    def test_slip_items_without_a_render_predict_no_cut_counters(self):
        # A sample_slip scenario changes where the engine's output ends, which
        # decides whether an entry near it applies, is clipped or is rejected.
        playlist = self.cards / "SLIPCUT.PL"
        playlist.write_bytes(b"A.WAV 1 USB scn=SLIP.SCN cut=5001:10:drop,6000:1:zero\nA.WAV 1 USB scn=NOISE.SCN cut=5001:10:drop,6000:1:zero\n")
        code, stdout, stderr = self.run_main(["check", str(playlist), "--cards", str(self.cards)])
        self.assertEqual(code, 0, stderr)
        slip, noise = json.loads(stdout)["items"]
        for name in ("uac_play_cut_count", "uac_play_cut_drop_frames", "uac_play_cut_zero_frames", "uac_play_cut_rejected", "cut_first_position"):
            self.assertIsNone(slip[name], name)
        self.assertEqual((noise["uac_play_cut_count"], noise["uac_play_cut_drop_frames"], noise["uac_play_cut_rejected"], noise["uac_play_sent_frames"]),
                         (1, 1, 1, 5001), "a scenario that keeps the length: the DROP is clipped at the end, the ZERO past it rejected")

    def test_prefix_seams_are_those_sent(self):
        # A 5002-frame card looped for 1 s; two DROPs of output frames 10000 to
        # 15009 remove the seams at 10004 and 15006, and the seam at 20008
        # comes 5010 frames earlier, at 14998.
        playlist = self.cards / "SEAMS.PL"
        playlist.write_bytes(b"A.WAV 1 USB label=seams dur=1 cut=10000:4800:drop,14800:210:drop\n")
        cases = {None: (9, [5002, None, None, 14998]), 10_000: (1, [5002]), 10_001: (3, [5002, None, None]), 14_998: (3, [5002, None, None]),
                 14_999: (4, [5002, None, None, 14998])}
        for sent, (count, first) in cases.items():
            with self.subTest(sent=sent):
                argv = ["predict", str(playlist), "--cards", str(self.cards), "--item", "0"] + ([] if sent is None else ["--sent-frames", str(sent)])
                code, stdout, stderr = self.run_main(argv)
                self.assertEqual(code, 0, stderr)
                prediction = json.loads(stdout)
                self.assertEqual((prediction["seam_count"], prediction["seam_positions"][:4]), (count, first),
                                 "only the seams within the frames sent: a seam sent, or one whose DROP took effect")

    def test_streams_past_a_wav_are_predicted_without_one(self):
        self.assertEqual(cuts.MAX_WAV_FRAMES, 1_431_655_752)
        self.assertLessEqual(36 + 3 * cuts.MAX_WAV_FRAMES, cuts.UINT32_MAX, "the longest WAV's RIFF size fits 32 bits")
        self.assertGreater(36 + 3 * (cuts.MAX_WAV_FRAMES + 1) + 1, cuts.UINT32_MAX, "one frame more, with its pad byte, does not")
        with mock.patch.object(cuts, "MAX_WAV_FRAMES", 40_000):
            playlist = self.cards / "LONG.PL"
            playlist.write_bytes(b"A.WAV 1 USB label=long dur=1\nA.WAV 1 USB label=short\n")
            wavs = self.cards / "expected"
            code, stdout, stderr = self.run_main(["check", str(playlist), "--cards", str(self.cards), "--expected-wavs", str(wavs)])
            self.assertEqual(code, 0, stderr)
            long, short = json.loads(stdout)["items"]
            self.assertEqual((long["expected_wav"], long["uac_play_sent_frames"], long["uac_play_crc32_l"]),
                             (None, 48_000, zlib.crc32(self.looped_data("A.WAV", 48_000))), "a stream longer than a WAV holds is predicted, with no WAV")
            self.assertTrue(Path(short["expected_wav"]).is_file())
            output = self.cards / "LONG.WAV"
            code, _, stderr = self.run_main(["predict", str(playlist), "--cards", str(self.cards), "--item", "0", "--output-wav", str(output)])
            self.assertEqual(code, 2)
            self.assertIn("do not fit a WAV", stderr)
            self.assertFalse(output.exists())
            scenario = self.cards / "LONGSCN.PL"
            scenario.write_bytes(b"A.WAV 1 USB scn=NOISE.SCN dur=1\n")
            code, _, stderr = self.run_main(["check", str(scenario), "--cards", str(self.cards), "--render", str(self.cards / "no-renderer.exe")])
            self.assertEqual(code, 2, "refused, not a traceback")
            self.assertIn("without --render", stderr)

    def test_durations_round_trip(self):
        for text, frames in (("1", 48_000), ("1.001", 48_048), ("3600", 172_800_000), ("86400", 4_147_200_000), ("86399.999", 4_147_199_952)):
            with self.subTest(text=text):
                self.assertEqual(mp.parse_duration(text), frames)
                self.assertEqual(mp.parse_duration(mp.format_gap(frames)), frames)
        for text in ("0", "0.999", "86400.001", "100000", "01", "1.2345", "", "-1", "60s"):
            with self.subTest(text=text), self.assertRaises(mp.PlaylistError):
                mp.parse_duration(text)
        spec = self.cards / "dur.json"
        spec.write_text(json.dumps([{"file": "A.WAV", "gap": 1, "route": "USB", "dur": 3600}, {"file": "B.WAV", "gap": 1, "route": "USB", "dur": "1.5", "att": -3}]))
        code, stdout, stderr = self.run_main(["write", "--items", str(spec), "--output", str(self.cards / "D.PL")])
        self.assertEqual((code, (self.cards / "D.PL").read_bytes()), (0, b"A.WAV 1 USB dur=3600\nB.WAV 1 USB att=-3 dur=1.5\n"), stderr)
        self.assertEqual([item["dur_frames"] for item in json.loads(stdout)["items"]], [172_800_000, 72_000])
        spec.write_text(json.dumps([{"file": "A.WAV", "gap": 1, "route": "USB", "dur": 0.5}]))
        code, _, stderr = self.run_main(["write", "--items", str(spec), "--output", str(self.cards / "E.PL")])
        self.assertEqual(code, 2)
        self.assertIn("PLAYLIST_DUR line 1", stderr)

    def test_scenario_items_name_their_scenario(self):
        items = self.check_predicted()
        noise_crc = zlib.crc32((self.cards / "NOISE.SCN").read_bytes())
        self.assertEqual([(item["scn_crc32"], item["engine_state"]) for item in items[4:7]],
                         [(noise_crc, 1), (0, 0), (zlib.crc32((self.cards / "SLIP.SCN").read_bytes()), 1)])
        (self.cards / "A.SCN").write_text(json.dumps(NOISE))
        playlist = self.cards / "Q.PL"
        playlist.write_bytes(b"A.WAV 1 USB\n")
        code, stdout, _ = self.run_main(["check", str(playlist), "--cards", str(self.cards)])
        plain = json.loads(stdout)["items"][0]
        self.assertEqual((code, plain["samples_predicted"], plain["uac_play_crc32_l"], plain["scn_crc32"], plain["engine_state"]),
                         (0, True, zlib.crc32(self.data["A.WAV"]), 0, 0), "an item without scn= plays its card clean, whatever A.SCN the folder holds")

    def test_start_predictions(self):
        items = self.check_predicted()
        self.assertEqual([(item["scheduled"], item["start_late"], item["run_start_item"], item["start_offset_frames"]) for item in items],
                         PREDICTED_STARTS)

    @unittest.skipUnless(os.environ.get("WFG_PLAYLIST_CHECKER"), "the firmware's next_start_index() is printed by WFG_PLAYLIST_CHECKER (ctest)")
    def test_offsets_follow_the_firmware_rule(self):
        checker = os.environ["WFG_PLAYLIST_CHECKER"]
        near_wrap = (1 << 32) - 48
        triples = [(0, 0, 0), (0, 1, 0), (0, 47, 0), (0, 48, 0), (0, 49, 240_000), (4800, 5002, 240_000), (96, 2000, 48_000),
                   (near_wrap, 48, 48), (near_wrap, 5002, 172_800_000)]
        board = json.loads(subprocess.run([checker, "--next-start", *(str(value) for triple in triples for value in triple)],
                                          capture_output=True, text=True, check=True).stdout)
        host = [(first + mp.whole_chunk_frames(sent) + gap) & mp.UINT32_MAX for first, sent, gap in triples]
        self.assertEqual(host, board)
        items = self.check_predicted()
        parsed = mp.parse_bytes(PREDICTED)
        chained = [(index, item) for index, item in enumerate(items) if item["run_start_item"] is not None and item["run_start_item"] != index]
        self.assertTrue(chained)
        for index, item in chained:
            before = items[index - 1]
            rule = json.loads(subprocess.run([checker, "--next-start", str(before["start_offset_frames"]), str(before["uac_play_sent_frames"]),
                                              str(parsed[index - 1].gap_frames)], capture_output=True, text=True, check=True).stdout)
            self.assertEqual(rule, [item["start_offset_frames"]], f"item {index} starts where the firmware places it")

    @unittest.skipUnless(os.environ.get("WFG_SIGNAL_LAB_RENDER"), "the host renderer is named by WFG_SIGNAL_LAB_RENDER (ctest)")
    def test_render_predicts_scenario_items(self):
        render = Path(os.environ["WFG_SIGNAL_LAB_RENDER"])
        playlist = self.cards / "R.PL"
        playlist.write_bytes(b"D.WAV 1 USB label=n scn=NOISE.SCN seed=7\nD.WAV 1 CODEC label=m scn=NOISE.SCN\nA.WAV 1 USB label=c scn=NOISE.SCN cut=10:5:zero\n")
        wavs = self.cards / "expected"
        code, stdout, stderr = self.run_main(["check", str(playlist), "--cards", str(self.cards), "--expected-wavs", str(wavs), "--render", str(render)])
        self.assertEqual(code, 0, stderr)
        items = json.loads(stdout)["items"]

        def rendered(name, *extra, source="D.WAV"):
            output = self.cards / name
            subprocess.run([str(render), "--scenario", str(self.cards / "NOISE.SCN"), "--source", str(self.cards / source), "--output", str(output),
                            *extra], capture_output=True, text=True, check=True)
            return mp.stream_facts(output)

        seeded, own = rendered("SEEDED.WAV", "--seed", "7"), rendered("OWN.WAV")
        self.assertNotEqual(seeded[1], own[1], "the seed reaches the render")
        self.assertEqual((items[0]["samples_predicted"], items[0]["uac_play_sent_frames"], items[0]["uac_play_crc32_l"], items[0]["uac_play_crc32_r"]),
                         (True, seeded[0], seeded[1], seeded[1]), "a seeded scenario item is predicted from its render")
        self.assertEqual(mp.stream_facts(Path(items[0]["expected_wav"])), seeded)
        self.assertEqual((items[1]["samples_predicted"], items[1]["uac_play_sent_frames"], items[1]["uac_play_crc32_l"]), (True, own[0], 0),
                         "a codec item renders with the scenario's own seed and has no USB CRC")
        # The cuts act on the scenario's output: the prediction is the schedule
        # applied to the item's render, not to its card.
        rendered("A-OWN.WAV", source="A.WAV")
        entries = cuts.parse_entries("10:5:zero")
        on_render = cuts.predict(self.cards / "A-OWN.WAV", entries)
        self.assertEqual((items[2]["samples_predicted"], items[2]["uac_play_sent_frames"], items[2]["uac_play_crc32_l"], items[2]["uac_play_cut_count"],
                          items[2]["uac_play_cut_zero_frames"], items[2]["cut_first_position"], items[2]["uac_play_file_frames"]),
                         (True, 5002 + 5, on_render["uac_play_crc32_l"], 1, 5, 10, 5002),
                         "a scenario item with cut= is predicted on its render")
        self.assertEqual(mp.stream_facts(Path(items[2]["expected_wav"])), (5002 + 5, on_render["uac_play_crc32_l"]),
                         "its expected stream is the render with the cut made")
        self.assertNotEqual(items[2]["uac_play_crc32_l"], cuts.predict(self.cards / "A.WAV", entries)["uac_play_crc32_l"])
        # A looped scenario item renders its looped card as one input, and its
        # attenuation acts on the render.
        playlist.write_bytes(b"D.WAV 1 USB label=loop scn=NOISE.SCN dur=1 att=-3\n")
        code, stdout, stderr = self.run_main(["check", str(playlist), "--cards", str(self.cards), "--render", str(render)])
        self.assertEqual(code, 0, stderr)
        looped_item = json.loads(stdout)["items"][0]
        source = self.cards / "LOOPED.WAV"
        write_card(source, [int.from_bytes(self.data["D.WAV"][at:at + 3], "little", signed=True) for at in range(0, 2000 * 3, 3)] * 24)
        direct = subprocess.run([str(render), "--scenario", str(self.cards / "NOISE.SCN"), "--source", str(source), "--output", str(self.cards / "LOOPED-R.WAV")],
                                capture_output=True, text=True, check=True)
        rendered_data = (self.cards / "LOOPED-R.WAV").read_bytes()[44:44 + 48_000 * 3]
        self.assertEqual((looped_item["samples_predicted"], looped_item["uac_play_sent_frames"], looped_item["uac_play_crc32_l"], looped_item["loops"]),
                         (True, 48_000, zlib.crc32(mp.attenuated(rendered_data, att.table()[30])), 24),
                         "a looped scenario item is predicted from the render of its looped card, attenuated")
        # The STATUS engine fields after it are the render's of the looped
        # input: the digest is the engine's own, before the attenuation.
        self.assertEqual(looped_item["engine_status"], cuts.engine_status(direct.stdout))
        self.assertEqual((looped_item["engine_status"]["engine_frames_in"], looped_item["engine_status"]["engine_frames_out"]), (48_000, 48_000))
        self.assertEqual(items[2]["engine_status"]["engine_frames_out"], 5002, "a cut item's engine puts out its whole render; the cut acts after it")

    def test_card_checks(self):
        playlist = self.cards / "Q.PL"
        playlist.write_bytes(b"A.WAV 0 USB\n# x\nNONE.WAV 0 USB\n")
        code, _, stderr = self.run_main(["check", str(playlist), "--cards", str(self.cards)])
        self.assertEqual(code, 2)
        self.assertIn("PLAYLIST_WAV_MISSING line 3", stderr)
        (self.cards / "BAD.WAV").write_bytes(b"RIFX" + bytes(40))
        playlist.write_bytes(b"BAD.WAV 0 USB\n")
        self.assertIn("PLAYLIST_WAV_INVALID line 1", self.run_main(["check", str(playlist), "--cards", str(self.cards)])[2])
        playlist.write_bytes(b"A.WAV 0 USB scn=NONE.SCN\n")
        self.assertIn("PLAYLIST_SCN_MISSING line 1", self.run_main(["check", str(playlist), "--cards", str(self.cards)])[2])
        code, stdout, _ = self.run_main(["check", str(playlist)])
        self.assertEqual((code, json.loads(stdout)["items"][0]["scn"]), (0, "NONE.SCN"), "without --cards only the file is checked")
        self.assertEqual(self.run_main(["check", str(playlist), "--expect", str(self.cards / "E.json")])[0], 2, "--expect needs --cards")
        self.assertEqual(self.run_main(["check", str(playlist), "--render", "signal_lab_render"])[0], 2, "--render needs --cards")


def stereo_f32_pairs(pairs: list[tuple[int, int]]) -> bytes:
    """(left, right) PCM24 codes as float32 stereo frames."""
    return b"".join(struct.pack("<ff", left / 8388608.0, right / 8388608.0) for left, right in pairs)


def write_pcm_capture(path: Path, samples: list[int], bits: int, channels: int = 2, extensible_float: bool = False) -> None:
    """A 48 kHz capture of the samples on every channel: PCM of `bits` bits holding them as the board's stream does (16 bits the top 16, 32
    bits << 8), or with extensible_float an extensible float32 one."""
    width = bits // 8
    if extensible_float:
        data = b"".join(struct.pack("<f", sample / 8388608.0) * channels for sample in samples)
        guid = struct.pack("<H", 3) + bytes.fromhex("000000001000800000aa00389b71")
        fmt = struct.pack("<HHIIHHHHI", 0xFFFE, channels, 48_000, 48_000 * 4 * channels, 4 * channels, 32, 22, 32, 3) + guid
    else:
        value = {16: lambda s: s >> 8, 24: lambda s: s, 32: lambda s: s << 8}[bits]
        data = b"".join((value(sample) & ((1 << bits) - 1)).to_bytes(width, "little") * channels for sample in samples)
        fmt = struct.pack("<HHIIHH", 1, channels, 48_000, 48_000 * width * channels, width * channels, bits)
    path.write_bytes(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(data)) + b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt +
                     b"data" + struct.pack("<I", len(data)) + data)


class CompareTests(unittest.TestCase):
    """compare: a capture or a split piece against an expected stream, sample for sample on every channel."""

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.root = Path(self.folder.name)
        self.samples = full_range(3000, 7)
        self.expected = self.root / "EXPECTED.WAV"
        write_card(self.expected, self.samples)

    def tearDown(self):
        self.folder.cleanup()

    def compare(self, capture: Path, *extra: str) -> tuple[int, dict | None, str]:
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch("sys.stdout", stdout), mock.patch("sys.stderr", stderr):
            code = mp.main(["compare", "--expected", str(self.expected), "--capture", str(capture), *extra])
        return code, json.loads(stdout.getvalue()) if code in (0, 1) else None, stderr.getvalue()

    def test_a_float_capture_with_silence_around_the_stream(self):
        capture = self.root / "CAP.f32.wav"
        write_f32_capture(capture, stereo_f32([0] * 300 + self.samples + [0] * 200))
        code, result, stderr = self.compare(capture)
        self.assertEqual(code, 0, stderr)
        self.assertEqual((result["ok"], result["capture_start_frame"], result["frames_compared"], result["first_mismatch"], result["silent_after"]),
                         (True, 300, 3000, None, True))
        self.assertEqual(result["format"], {"kind": "float", "bits": 32, "channels": 2})

    def test_a_wrong_sample_on_one_channel(self):
        pairs = [(0, 0)] * 300 + [(sample, sample) for sample in self.samples]
        pairs[300 + 1234] = (self.samples[1234], self.samples[1234] ^ 1)
        capture = self.root / "BAD.f32.wav"
        write_f32_capture(capture, stereo_f32_pairs(pairs))
        code, result, _ = self.compare(capture)
        self.assertEqual(code, 1)
        self.assertEqual(result["first_mismatch"], {"frame": 1234, "channel": 1, "expected": self.samples[1234], "got": self.samples[1234] ^ 1})

    def test_a_float_that_is_no_24_bit_sample(self):
        data = bytearray(stereo_f32_pairs([(0, 0)] * 10 + [(sample, sample) for sample in self.samples]))
        data[(10 + 5) * 8:(10 + 5) * 8 + 4] = struct.pack("<f", float("nan"))
        capture = self.root / "NAN.f32.wav"
        write_f32_capture(capture, bytes(data))
        code, result, _ = self.compare(capture)
        self.assertEqual((code, result["first_mismatch"]), (1, {"frame": 5, "channel": 0, "expected": self.samples[5], "got": None}))

    def test_a_prefix_and_the_silence_after_it(self):
        capture = self.root / "PART.f32.wav"
        write_f32_capture(capture, stereo_f32([0] * 100 + self.samples[:1000] + [0] * 2500))
        self.assertEqual(self.compare(capture, "--sent-frames", "1000")[0], 0, "the first 1000 frames, then silence")
        code, result, _ = self.compare(capture)
        self.assertEqual((code, result["first_mismatch"]["frame"]), (1, 1000), "the whole stream is not there")
        noisy = self.root / "NOISY.f32.wav"
        write_f32_capture(noisy, stereo_f32([0] * 100 + self.samples + [0, 5, 0]))
        code, result, _ = self.compare(noisy)
        self.assertEqual((code, result["first_mismatch"], result["silent_after"]), (1, None, False), "sound after the stream fails it")

    def test_capture_formats(self):
        for bits, stream_bits in ((24, 24), (32, 24), (16, 16), (32, 16), (24, 16)):
            with self.subTest(bits=bits, stream_bits=stream_bits):
                truncated = [sample >> 8 << 8 for sample in self.samples] if stream_bits == 16 else self.samples
                capture = self.root / f"PCM{bits}-{stream_bits}.WAV"
                write_pcm_capture(capture, [0] * 50 + truncated + [0] * 50, bits)
                code, result, stderr = self.compare(capture, "--stream-bits", str(stream_bits))
                self.assertEqual(code, 0, stderr)
                self.assertEqual(result["format"], {"kind": "pcm", "bits": bits, "channels": 2})
        capture = self.root / "EXT.WAV"
        write_pcm_capture(capture, [0] * 50 + self.samples, 32, channels=1, extensible_float=True)
        self.assertEqual(self.compare(capture)[0], 0, "an extensible float capture, one channel")
        full = self.root / "FULL24.WAV"
        write_pcm_capture(full, [0] * 50 + self.samples, 24)
        code, result, _ = self.compare(full, "--stream-bits", "16")
        self.assertEqual(code, 1, "a 16-bit stream's samples have their low 8 bits clear")

    def test_placement_and_refusals(self):
        capture = self.root / "CAP.f32.wav"
        write_f32_capture(capture, stereo_f32([0] * 300 + self.samples))
        self.assertEqual(self.compare(capture, "--capture-start", "300")[0], 0)
        self.assertEqual(self.compare(capture, "--capture-start", "301")[0], 1, "one frame off")
        code, result, _ = self.compare(capture, "--capture-start", "400")
        self.assertEqual((code, result["why"]), (1, "the capture does not hold the stream at that place"))
        write_card(self.expected, [0] * 3000)
        code, _, stderr = self.compare(capture)
        self.assertEqual(code, 2)
        self.assertIn("--capture-start", stderr)


class SplitTests(unittest.TestCase):
    """split: a battery's capture cut into one piece per USB item, by the blocks a drain saved."""

    GAP = 4800
    LEAD = 7000

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.root = Path(self.folder.name)
        self.cards = self.root / "cards"
        self.cards.mkdir()
        # A begins with 10 frames of silence, as a card may.
        self.samples = {"A.WAV": [0] * 10 + full_range(990, 1), "B.WAV": full_range(700, 2), "C.WAV": full_range(500, 3)}
        for name, samples in self.samples.items():
            write_card(self.cards / name, samples)
        playlist = self.root / "S.PL"
        playlist.write_bytes(b"A.WAV 0.1 USB label=a\nC.WAV 0.1 CODEC label=c\nB.WAV 0.1 USB label=b cut=100:20:drop\nA.WAV 0.1 USB label=d\n")
        self.expect = self.root / "S-EXPECT.json"
        with mock.patch("sys.stdout", io.StringIO()):
            self.assertEqual(mp.main(["check", str(playlist), "--cards", str(self.cards), "--expect", str(self.expect)]), 0)
        self.predicted = json.loads(self.expect.read_text())["items"]

    def tearDown(self):
        self.folder.cleanup()

    def stream_of(self, item: int) -> list[int]:
        expect = self.predicted[item]
        samples = self.samples[expect["file"]]
        if not expect["cut"]:
            return samples
        stream = cuts.apply(cuts.parse_command(expect["cut"]), len(samples))
        played = []
        for segment in stream.segments:
            played += [0] * segment.frames if segment.card_frame is None else samples[segment.card_frame:segment.card_frame + segment.frames]
        return played

    def battery(self, *, origin=3000, shift_last=0):
        """The blocks a board would report, a capture of them and the journals: USB items 0, 2 and 3, 4800 frames apart."""
        blocks, capture, at = [], [0] * self.LEAD, self.LEAD
        for seq, (item, expect) in enumerate(zip(range(4), self.predicted), 1):
            block = {"seq": seq, "run": 2, "item": item, "label": expect["label"], "file": expect["file"], "route": expect["route"],
                     "result": "done", "uac_play_stream_bits": 24}
            if expect["route"] == "codec":
                blocks.append({**block, "uac_play_sent_frames": expect["uac_play_sent_frames"], "uac_play_first_frame_index": 0})
                continue
            stream = self.stream_of(item)
            if item == 3:
                capture += [0] * shift_last
            block.update(uac_play_sent_frames=len(stream), uac_play_first_frame_index=origin + at, uac_play_missed_intervals=0,
                         uac_play_crc32_l=expect["uac_play_crc32_l"], uac_play_crc32_r=expect["uac_play_crc32_r"])
            blocks.append(block)
            capture += stream + [0] * self.GAP
            at += len(stream) + self.GAP
        drained = self.root / "BLOCKS.jsonl"
        lines = [{"schema": mp.BLOCK_SCHEMA, "playlist": None, "block": {**blocks[0], "run": 1, "file": "OLD.WAV"}}]
        lines += [{"schema": mp.BLOCK_SCHEMA, "playlist": "S.PL", "block": block} for block in blocks]
        drained.write_text("".join(json.dumps(line) + "\n" for line in lines))
        path = self.root / "CAP.f32.wav"
        write_f32_capture(path, stereo_f32(capture))

        def journal(name, opens, short, asserts):
            usb = {"format": "usb-diagnostics", "uac_stream_opens": opens, "uac_short_packets": short, "usb_stack_asserts": asserts}
            record = {"schema": mp.SESSION_SCHEMA, "record": "command", "seq": 2, "command": "USB?", "response": {"ok": True, "usb": usb}}
            (self.root / name).write_text(json.dumps({"schema": mp.SESSION_SCHEMA, "record": "identity"}) + "\n" + json.dumps(record) + "\n")
            return self.root / name

        return path, drained, journal("BEFORE.jsonl", 3, 5, 1), journal("AFTER.jsonl", 3, 5, 1), journal("REOPENED.jsonl", 4, 5, 1)

    def split(self, capture, drained, after, before=None, output="pieces"):
        argv = ["split", "--capture", str(capture), "--blocks", str(drained), "--expect", str(self.expect), "--cards", str(self.cards),
                "--usb-after", str(after), "--output", str(self.root / output)]
        if before is not None:
            argv += ["--usb-before", str(before)]
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch("sys.stdout", stdout), mock.patch("sys.stderr", stderr):
            code = mp.main(argv)
        return code, json.loads(stdout.getvalue()) if code in (0, 1) else None, stderr.getvalue()

    def test_pieces_hold_each_item_and_a_status_verify_capture_reads(self):
        capture, drained, before, after, _ = self.battery()
        code, manifest, stderr = self.split(capture, drained, after, before)
        self.assertEqual(code, 0, stderr)
        self.assertEqual((manifest["schema"], manifest["run"], manifest["anchor_item"], manifest["origin_stream_index"], manifest["ok"]),
                         (mp.SPLIT_SCHEMA, 2, 0, 3000, True), "the newest run, placed by its first item's first sound")
        self.assertEqual(manifest["stream"], {"uac_stream_opens": 3, "uac_short_packets": 0, "usb_stack_asserts": 0})
        entries = manifest["items"]
        self.assertEqual([entry["piece"] is not None for entry in entries], [True, False, True, True])
        self.assertIn("codec", entries[1]["why"])
        # A's 1000 frames, the gap, B's 680 after its DROP of 20, the gap, A again.
        starts = (self.LEAD, self.LEAD + 1000 + self.GAP, self.LEAD + 1000 + self.GAP + 680 + self.GAP)
        whole = Path(capture).read_bytes()[44:]
        for entry, item, start in zip([entries[0], entries[2], entries[3]], (0, 2, 3), starts):
            with self.subTest(item=item):
                self.assertEqual((entry["capture_start_frame"], entry["frames"], entry["onset_ok"], entry["silent_around"], entry["ok"]),
                                 (start, len(self.stream_of(item)), True, True, True))
                piece = Path(entry["piece"]).read_bytes()
                self.assertTrue(entry["piece"].endswith(".f32.wav") and piece[:4] == b"RIFF" and piece[20:22] == struct.pack("<H", 3))
                first = (start - entry["pre_frames"]) * 8
                self.assertEqual(piece[44:], whole[first:first + (entry["pre_frames"] + entry["frames"] + entry["post_frames"]) * 8])
                status = json.loads(Path(entry["status"]).read_text())
                self.assertEqual((status["output_route"], status["live_events"], status["usb_stack_asserts"], status["uac_short_packets"],
                                  status["uac_play_first_frame_index"]), (1, 0, 0, 0, 3000 + start))
                self.assertEqual(entry["verify_capture"][:6], ["--card", str(self.cards / self.predicted[item]["file"]), "--capture", entry["piece"],
                                                               "--status", entry["status"]])
        self.assertEqual(entries[2]["verify_capture"][6:], ["--cuts", "CUT:100:20:drop", "--usb", entries[2]["status"]])
        self.assertEqual([(entry["pre_frames"], entry["post_frames"]) for entry in (entries[0], entries[2], entries[3])],
                         [(self.LEAD, self.GAP // 2), (self.GAP // 2, self.GAP // 2), (self.GAP // 2, self.GAP)],
                         "all the lead-in, half of each gap, and all the last gap")

    def test_attenuated_items_name_their_compare(self):
        playlist = self.root / "T.PL"
        playlist.write_bytes(b"A.WAV 0.1 USB label=a\nB.WAV 0.1 USB label=b att=-20\n")
        expect, wavs = self.root / "T-EXPECT.json", self.root / "expected"
        with mock.patch("sys.stdout", io.StringIO()):
            self.assertEqual(mp.main(["check", str(playlist), "--cards", str(self.cards), "--expect", str(expect), "--expected-wavs", str(wavs)]), 0)
        predicted = json.loads(expect.read_text())["items"]
        streams = [self.samples["A.WAV"], [att.apply(sample, att.table()[200]) for sample in self.samples["B.WAV"]]]
        blocks, capture, at = [], [0] * self.LEAD, self.LEAD
        for seq, (expect_item, stream) in enumerate(zip(predicted, streams), 1):
            blocks.append({"seq": seq, "run": 1, "item": expect_item["item"], "label": expect_item["label"], "file": expect_item["file"], "route": "usb",
                           "result": "done", "uac_play_stream_bits": 24, "uac_play_sent_frames": len(stream), "uac_play_first_frame_index": 3000 + at})
            capture += stream + [0] * self.GAP
            at += len(stream) + self.GAP
        drained = self.root / "T-BLOCKS.jsonl"
        drained.write_text("".join(json.dumps({"schema": mp.BLOCK_SCHEMA, "playlist": "T.PL", "block": block}) + "\n" for block in blocks))
        recorded = self.root / "T.f32.wav"
        write_f32_capture(recorded, stereo_f32(capture))
        usb = {"format": "usb-diagnostics", "uac_stream_opens": 3, "uac_short_packets": 0, "usb_stack_asserts": 0}
        after = self.root / "T-AFTER.jsonl"
        after.write_text(json.dumps({"schema": mp.SESSION_SCHEMA, "record": "command", "seq": 1, "command": "USB?", "response": {"ok": True, "usb": usb}}) + "\n")
        stdout = io.StringIO()
        with mock.patch("sys.stdout", stdout):
            code = mp.main(["split", "--capture", str(recorded), "--blocks", str(drained), "--expect", str(expect), "--cards", str(self.cards),
                            "--usb-after", str(after), "--output", str(self.root / "t-pieces")])
        self.assertEqual(code, 0)
        entries = json.loads(stdout.getvalue())["items"]
        self.assertIsNone(entries[0]["compare"], "a clean item: verify_capture")
        self.assertIsNone(entries[1]["verify_capture"])
        self.assertEqual(entries[1]["compare"][:3], ["compare", "--expected", predicted[1]["expected_wav"]])
        self.assertIn("make_playlist.py compare", entries[1]["why"])
        with mock.patch("sys.stdout", io.StringIO()) as printed:
            self.assertEqual(mp.main(entries[1]["compare"]), 0, "the piece of the attenuated item is its expected stream")
        self.assertTrue(json.loads(printed.getvalue())["ok"])

    def test_a_misplaced_item_fails_its_onset(self):
        capture, drained, before, after, _ = self.battery(shift_last=48)
        code, manifest, _ = self.split(capture, drained, after, before)
        self.assertEqual(code, 1)
        self.assertEqual([entry.get("onset_ok") for entry in manifest["items"]], [True, None, True, False])
        self.assertFalse(manifest["ok"])

    def test_refusals(self):
        capture, drained, before, after, reopened = self.battery()
        code, _, stderr = self.split(capture, drained, reopened, before)
        self.assertEqual(code, 2)
        self.assertIn("opened again", stderr)
        code, manifest, stderr = self.split(capture, drained, after, output="since-boot")
        self.assertEqual((code, manifest["stream_counted_from"], manifest["stream"]["uac_short_packets"]), (0, "boot", 5),
                         "without USB? before, the counters since boot")
        loud = self.root / "LOUD.f32.wav"
        write_f32_capture(loud, stereo_f32([1] + [0] * 100))
        code, _, stderr = self.split(loud, drained, after, before, output="loud")
        self.assertEqual(code, 2)
        self.assertIn("begins with sound", stderr)
        self.assertEqual(self.split(capture, drained, after, before)[0], 0)
        self.assertEqual(self.split(capture, drained, after, before)[0], 2, "the pieces are new files")


if __name__ == "__main__":
    unittest.main()
