# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""host/media_eject.py against a fake Windows: no volume, disk, port or device is opened."""

import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "host"))
import live_control as live
import make_playlist as mp
import media_eject as me

BOARD = {"vendor": "WFG", "product": "RT1170 SD LOADER", "revision": "0001", "serial": "", "removable_media": True}
READ_ONLY = {**BOARD, "product": "RT1170 SD READ"}
OTHER = {"vendor": "Samsung", "product": "SSD 980 PRO", "revision": "1", "serial": "S1", "removable_media": False}
SHARING_VIOLATION = 32


class FakeWindows:
    """Drives, volumes and the volume requests the tool sends, with the board's medium on E: (a folder)."""

    def __init__(self, root: Path, *, disks=None):
        self.folder = root
        self.disks = disks if disks is not None else {"C": OTHER, "E": BOARD}
        self.mounted = {letter: True for letter in self.disks}
        self.denied = set()        # access modes CreateFileW refuses with error 5
        self.ioctl_errors = {}     # code -> list of errors to return, one per call (0 = success)
        self.requests = []
        self.opened = []
        self.closed = []
        self.corrupt = False
        self.on_eject = None

    def is_elevated(self):
        return False

    def root(self, letter):
        return self.folder / letter

    def letters(self):
        return sorted(self.disks)

    def volume_label(self, letter):
        return "WFGCARD" if self.mounted.get(letter) else None

    def disk_identity(self, letter):
        return self.disks.get(letter)

    def open_volume(self, letter, access, flags=0):
        if access in self.denied:
            raise OSError(me.ERROR_ACCESS_DENIED, "Access is denied")
        handle = 100 + len(self.opened)
        self.opened.append((letter, access, handle))
        return handle

    def ioctl(self, handle, code, payload=b"", output_bytes=0):
        self.requests.append((code, payload))
        errors = self.ioctl_errors.get(code, [])
        error = errors.pop(0) if errors else 0
        if error == 0 and code == me.IOCTL_STORAGE_EJECT_MEDIA:
            letter = next(entry[0] for entry in self.opened if entry[2] == handle)
            self.mounted[letter] = False
            if self.on_eject is not None:
                self.on_eject()
        return error == 0, error, b""

    def close(self, handle):
        self.closed.append(handle)

    def read_unbuffered(self, path):
        data = Path(path).read_bytes()
        if self.corrupt:
            data = data[:-1] + bytes([data[-1] ^ 1])
        return hashlib.sha256(data).hexdigest(), len(data), 0.5


class EjectTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.api = FakeWindows(Path(self.folder.name))
        for letter in self.api.disks:
            (Path(self.folder.name) / letter).mkdir()

    def tearDown(self):
        self.folder.cleanup()

    def test_find_marks_only_the_boards_drive(self):
        found = me.drives(self.api)
        self.assertEqual([(drive["letter"], drive["board"]) for drive in found], [("C", False), ("E", True)])

    def test_the_board_needs_its_vendor_and_its_product(self):
        self.assertTrue(me.is_board(BOARD))
        self.assertTrue(me.is_board(READ_ONLY))
        for identity in ({**BOARD, "product": "OTHER DISK"}, {**OTHER, "product": "RT1170 SD LOADER"},
                         {**OTHER, "product": "RT1170 SD READ"}, {**BOARD, "vendor": ""}, None):
            with self.subTest(identity=identity):
                self.assertFalse(me.is_board(identity))
        api = FakeWindows(Path(self.folder.name), disks={"E": {**BOARD, "product": "OTHER DISK"},
                                                         "F": {**OTHER, "product": "RT1170 SD LOADER"}})
        self.assertEqual([drive["board"] for drive in me.drives(api)], [False, False])
        with self.assertRaisesRegex(me.StagingError, "exactly one board drive"):
            me.media_eject(api, sleep=lambda seconds: None)
        self.assertEqual(api.requests, [], "WFG with another product, or that product from another vendor, is not the board")

    def test_the_eject_sends_only_the_media_requests_in_order(self):
        steps = me.media_eject(self.api, sleep=lambda seconds: None)
        self.assertEqual([code for code, _ in self.api.requests],
                         [me.FSCTL_LOCK_VOLUME, me.FSCTL_DISMOUNT_VOLUME, me.IOCTL_STORAGE_MEDIA_REMOVAL, me.IOCTL_STORAGE_EJECT_MEDIA])
        self.assertEqual(self.api.requests[2][1], b"\0", "PreventMediaRemoval FALSE: removal allowed")
        self.assertEqual((steps["drive"], steps["access"], steps["ejected"]), ("E", "read-write", True))
        self.assertEqual(self.api.closed, [self.api.opened[0][2]], "the handle is closed, which ends the lock")
        self.assertEqual(self.api.opened[0][:2], ("E", me.GENERIC_READ | me.GENERIC_WRITE))

    def test_only_the_boards_drive_is_ever_ejected(self):
        for letter, disks, message in (("C", None, "not the board's drive"),
                                       (None, {"E": BOARD, "F": BOARD}, "exactly one board drive"),
                                       (None, {"C": OTHER}, "exactly one board drive")):
            with self.subTest(letter=letter, disks=disks):
                api = FakeWindows(Path(self.folder.name), disks=disks) if disks else self.api
                with self.assertRaisesRegex(me.StagingError, message) as caught:
                    me.media_eject(api, letter, sleep=lambda seconds: None)
                self.assertEqual(caught.exception.exit_code, me.EXIT_REFUSED)
                self.assertEqual(api.requests, [], "no request reaches any volume")
        self.api.mounted["E"] = False
        with self.assertRaisesRegex(me.StagingError, "no mounted volume"):
            me.media_eject(self.api, sleep=lambda seconds: None)

    def test_serial_selects_one_of_two_boards_and_rejects_wrong_letter(self):
        self.api.disks["E"] = {**BOARD, "serial": "1064"}
        self.api.disks["F"] = {**BOARD, "serial": "1170"}
        self.api.mounted["F"] = True
        self.assertEqual(me.board_drive(self.api, expected_serial="1064")["letter"], "E")
        for letter, serial in (("F", "1064"), (None, "missing")):
            with self.assertRaises(me.StagingError):
                me.media_eject(self.api, letter, expected_serial=serial, sleep=lambda seconds: None)
        self.assertEqual(self.api.requests, [])
        result = me.media_eject(self.api, expected_serial="1064", sleep=lambda seconds: None)
        self.assertEqual(result["disk"]["serial"], "1064")
        self.assertFalse(result["process_elevated"])
        self.assertTrue(self.api.mounted["F"])

    def test_volume_open_denial_does_not_claim_a_dismount(self):
        self.api.denied = {me.GENERIC_READ | me.GENERIC_WRITE}
        steps = me.media_eject(self.api, sleep=lambda seconds: None)
        self.assertEqual(steps["access"], "read", "a read-only handle is tried after a refused read-write one")
        self.api.mounted["E"] = True
        self.api.denied = {me.GENERIC_READ | me.GENERIC_WRITE, me.GENERIC_READ}
        with self.assertRaises(me.StagingError) as caught:
            me.media_eject(self.api, sleep=lambda seconds: None)
        self.assertEqual(caught.exception.exit_code, me.EXIT_ELEVATION)
        self.assertIn("Windows denied both access modes", str(caught.exception))
        self.assertIn("do not send MEDIA LOCAL", str(caught.exception))
        self.assertNotIn("reset", str(caught.exception))

    def test_read_only_handle_lock_denial_is_retried_without_guessing_elevation(self):
        self.api.denied = {me.GENERIC_READ | me.GENERIC_WRITE}
        self.api.ioctl_errors = {me.FSCTL_LOCK_VOLUME: [me.ERROR_ACCESS_DENIED] * 2}
        sleeps = []
        result = me.media_eject(self.api, sleep=sleeps.append)
        self.assertEqual(result["lock_attempts"], 3)
        self.assertEqual(sleeps, [me.LOCK_INTERVAL_S] * 2)
        self.assertFalse(result["process_elevated"])

    def test_read_write_handle_does_not_prove_elevation_and_retries_a_busy_lock(self):
        # A FAT32 removable volume can grant this handle to an ordinary user.
        sleeps = []
        self.api.ioctl_errors = {me.FSCTL_LOCK_VOLUME: [me.ERROR_ACCESS_DENIED] * 2}
        steps = me.media_eject(self.api, sleep=sleeps.append)
        self.assertEqual((steps["access"], steps["lock_attempts"], steps["ejected"]), ("read-write", 3, True))
        self.assertEqual(sleeps, [me.LOCK_INTERVAL_S] * 2)
        self.api.mounted["E"] = True
        self.api.requests.clear()
        self.api.ioctl_errors = {me.FSCTL_LOCK_VOLUME: [me.ERROR_ACCESS_DENIED] * me.LOCK_ATTEMPTS}
        with self.assertRaises(me.StagingError) as caught:
            me.media_eject(self.api, sleep=lambda seconds: None)
        self.assertEqual(caught.exception.exit_code, me.EXIT_LOCKED, "a lock that never comes is reported locked, not elevation")
        self.assertIn("Windows error 5", str(caught.exception))
        self.assertIn("file open", str(caught.exception))
        self.assertEqual([code for code, _ in self.api.requests], [me.FSCTL_LOCK_VOLUME] * me.LOCK_ATTEMPTS, "a bounded retry")
        self.assertEqual(len(self.api.closed), len(self.api.opened))

    def test_error_5_after_the_lock_reports_a_failed_step_for_either_handle(self):
        for denied, exit_code in ((set(), me.EXIT_REQUEST), ({me.GENERIC_READ | me.GENERIC_WRITE}, me.EXIT_REQUEST)):
            with self.subTest(read_write=not denied):
                self.api.mounted["E"] = True
                self.api.denied = denied
                self.api.ioctl_errors = {me.FSCTL_DISMOUNT_VOLUME: [me.ERROR_ACCESS_DENIED]}
                with self.assertRaises(me.StagingError) as caught:
                    me.media_eject(self.api, sleep=lambda seconds: None)
                self.assertEqual(caught.exception.exit_code, exit_code)

    def test_a_busy_volume_is_retried_then_refused(self):
        sleeps = []
        self.api.ioctl_errors = {me.FSCTL_LOCK_VOLUME: [SHARING_VIOLATION] * 3}
        steps = me.media_eject(self.api, sleep=sleeps.append)
        self.assertEqual((steps["lock_attempts"], sleeps), (4, [me.LOCK_INTERVAL_S] * 3))
        self.api.mounted["E"] = True
        self.api.ioctl_errors = {me.FSCTL_LOCK_VOLUME: [SHARING_VIOLATION] * me.LOCK_ATTEMPTS}
        self.api.requests.clear()
        with self.assertRaises(me.StagingError) as caught:
            me.media_eject(self.api, sleep=lambda seconds: None)
        self.assertEqual(caught.exception.exit_code, me.EXIT_LOCKED)
        self.assertEqual({code for code, _ in self.api.requests}, {me.FSCTL_LOCK_VOLUME}, "nothing is dismounted or ejected")
        self.assertEqual(len(self.api.closed), len(self.api.opened), "every handle is closed")

    def test_a_failed_request_stops_the_eject(self):
        for code in (me.FSCTL_DISMOUNT_VOLUME, me.IOCTL_STORAGE_MEDIA_REMOVAL, me.IOCTL_STORAGE_EJECT_MEDIA):
            with self.subTest(code=hex(code)):
                self.api.mounted["E"] = True
                self.api.requests.clear()
                self.api.ioctl_errors = {code: [1]}
                with self.assertRaises(me.StagingError) as caught:
                    me.media_eject(self.api, sleep=lambda seconds: None)
                self.assertEqual(caught.exception.exit_code, me.EXIT_REQUEST)
                self.assertEqual(self.api.requests[-1][0], code, "no request is sent after the one that failed")
        self.assertEqual(len(self.api.closed), len(self.api.opened))


class StageTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        root = Path(self.folder.name)
        self.api = FakeWindows(root / "drives")
        for letter in self.api.disks:
            (root / "drives" / letter).mkdir(parents=True)
        self.card = root / "fsrand.wav"
        self.card.write_bytes(bytes(range(256)) * 4096)

    def tearDown(self):
        self.folder.cleanup()

    def test_wrong_serial_refuses_before_creating_or_writing_files(self):
        self.api.disks["E"] = {**BOARD, "serial": "1064"}
        with self.assertRaises(me.StagingError):
            me.stage(self.api, self.card, letter="E", expected_serial="1170", eject=True)
        self.assertFalse((self.api.root("E") / "WG").exists())
        self.assertEqual(self.api.requests, [])

    def test_stage_copies_reads_back_past_the_cache_and_ejects(self):
        result = me.stage(self.api, self.card, eject=True, sleep=lambda seconds: None)
        staged = self.api.root("E") / "WG" / "FSRAND.WAV"
        self.assertEqual(staged.read_bytes(), self.card.read_bytes())
        self.assertTrue(result["match"])
        self.assertEqual((result["bytes"], result["sha256"]), (len(self.card.read_bytes()), hashlib.sha256(self.card.read_bytes()).hexdigest()))
        self.assertTrue(result["eject"]["ejected"])
        self.assertFalse(self.api.mounted["E"])

    def test_stage_without_eject_never_ejects(self):
        result = me.stage(self.api, self.card, sleep=lambda seconds: None)
        self.assertTrue(result["match"])
        self.assertNotIn("eject", result)
        self.assertEqual((self.api.requests, self.api.opened), ([], []), "no volume handle is opened, no request sent")
        self.assertTrue(self.api.mounted["E"], "the medium stays with the host")
        for name, ejected in (("CLI0001.WAV", False), ("CLI0002.WAV", True)):
            extra = ["--name", name]
            if ejected:
                extra.append("--eject")
            with self.subTest(extra=extra):
                self.api.mounted["E"] = True
                self.api.requests.clear()
                stdout = io.StringIO()
                with mock.patch.object(me, "Windows", lambda: self.api), mock.patch.object(me.sys, "platform", "win32"), \
                        mock.patch("sys.stdout", stdout):
                    self.assertEqual(me.main(["stage", str(self.card), *extra]), me.EXIT_OK)
                self.assertEqual("eject" in json.loads(stdout.getvalue()), ejected)
                self.assertEqual(me.IOCTL_STORAGE_EJECT_MEDIA in [code for code, _ in self.api.requests], ejected)
                self.assertEqual(self.api.mounted["E"], not ejected)

    def test_stage_refuses_existing_destination_without_changing_it(self):
        target = self.api.root("E") / "WG" / "FSRAND.WAV"
        target.parent.mkdir()
        original = b"existing card artifact must survive"
        target.write_bytes(original)

        with mock.patch.object(self.api, "read_unbuffered", side_effect=AssertionError("must not read a refused target")):
            with self.assertRaisesRegex(me.StagingError, "refusing to overwrite existing staged file") as caught:
                me.stage(self.api, self.card, eject=True, sleep=lambda seconds: None)

        self.assertEqual(caught.exception.exit_code, me.EXIT_REFUSED)
        self.assertEqual(target.read_bytes(), original)
        self.assertEqual(self.api.requests, [], "a refused overwrite does not eject the card")

    def test_stage_takes_every_name_playlist_takes(self):
        for name in ("LADDER", "BATTERY.TXT", "W5ROWS.PL", "FSRAND.SCN", "STAGE16.BIN", "A_1.X"):
            with self.subTest(name=name):
                result = me.stage(self.api, self.card, name=name, sleep=lambda seconds: None)
                self.assertTrue(result["match"])
                self.assertEqual((self.api.root("E") / "WG" / name).read_bytes(), self.card.read_bytes())
        for name in ("LADDER", "BATTERY.TXT", "A.WAVE", "lower.pl", "A.", "TOOLONGNAME", "A B.PL", "A.WAV.EXE"):
            with self.subTest(name=name):
                self.assertEqual(bool(me.NAME_RULE.fullmatch(name)), mp.is_playlist_name(name), "the staging rule is PLAYLIST's")

    def test_stage_refuses_what_the_player_would(self):
        for kwargs, message in (({"name": "fs-rand.wav"}, "8.3"), ({"name": "TOOLONGNAME.WAV"}, "8.3"), ({"name": "A.WAVE"}, "8.3"),
                                ({"name": "A."}, "8.3"), ({"name": ".PL"}, "8.3"), ({"name": "A.WAV.EXE"}, "8.3"),
                                ({"name": "A.WAV "}, "8.3")):
            with self.subTest(kwargs=kwargs), self.assertRaisesRegex(me.StagingError, message):
                me.stage(self.api, self.card, **kwargs)
        self.api.disks["E"] = READ_ONLY
        with self.assertRaisesRegex(me.StagingError, "read-only image"):
            me.stage(self.api, self.card)

    def test_a_read_back_that_differs_fails_and_never_ejects(self):
        self.api.corrupt = True
        with self.assertRaises(me.StagingError) as caught:
            me.stage(self.api, self.card, eject=True)
        self.assertEqual(caught.exception.exit_code, me.EXIT_CHECK)
        self.assertTrue(self.api.mounted["E"], "the medium stays with the host")


class BoardSession:
    """The board's side of a cycle: MEDIA HOST mounts the medium, the eject latches msc_ejected, MEDIA LOCAL takes it."""

    def __init__(self, api):
        self.api = api
        self.media_state = 3
        self.msc_ejected = 0
        self.asserts = 0
        self.refuse_local = False
        self.unmount_after_host = False
        self.hold = 0
        self.latches = True
        api.on_eject = self.eject
        api.mounted["E"] = False

    def eject(self):
        self.msc_ejected = 1 if self.latches else 0

    def handle(self, sequence, text):
        def reply(field, data, ok=True):
            return json.dumps({"seq": sequence, "protocol": live.PROTOCOL, "ok": ok, "state": 1, "run_id": None, field: data},
                              separators=(",", ":")).encode() + b"\n"

        if text == "INFO?":
            return reply("info", {"kind": "waveform-player", "board": "MIMXRT1170-EVK", "sample_rate_hz": 48000, "usb_serial": "SERIAL"})
        if text == "USB?":
            return reply("usb", {"format": "usb-diagnostics", "usb_stack_asserts": self.asserts})
        if text == "STATUS?":
            return reply("status", {"media_state": self.media_state, "msc_ejected": self.msc_ejected, "msc_host_hold": self.hold})
        if text == "MEDIA HOST":
            self.media_state, self.msc_ejected = 2, 0
            self.api.mounted["E"] = not self.unmount_after_host
            return reply("result", {"accepted": True})
        if text == "MEDIA LOCAL":
            if self.refuse_local:
                return reply("error", {"code": "HOST_LOCKED", "message": "HOST_LOCKED"}, ok=False)
            self.media_state = 3
            return reply("result", {"accepted": True})
        return reply("result", {"accepted": True})


class Transport:
    def __init__(self, board):
        self.board, self.output, self.pending, self.closed = board, bytearray(), bytearray(), False

    def write(self, data):
        self.pending.extend(data)
        line, _, rest = self.pending.partition(b"\n")
        self.pending = bytearray(rest)
        sequence, text = line.decode().split(" ", 1)
        self.output.extend(self.board.handle(int(sequence), text))
        return len(data)

    def read(self, count):
        chunk = bytes(self.output[:count])
        del self.output[:count]
        return chunk

    def close(self):
        self.closed = True


class CycleTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        root = Path(self.folder.name)
        self.api = FakeWindows(root / "drives")
        for letter in self.api.disks:
            (root / "drives" / letter).mkdir(parents=True)
        self.card = root / "CARD16.WAV"
        self.card.write_bytes(b"\x01\x02\x03" * 100000)
        self.board = BoardSession(self.api)
        self.sessions = []
        self.ports = ["COM8", "COM7"]

    def tearDown(self):
        self.folder.cleanup()

    def connect(self, journal, dismounted):
        transport = Transport(self.board)
        controller = live.Controller(transport, "SERIAL", journal=journal, volume_dismounted=dismounted)
        controller.identify()
        self.sessions.append((transport, dismounted))
        return controller

    def run_cycles(self, cycles):
        clock = [0.0]

        def tick():
            clock[0] += 0.1
            return clock[0]

        self.runs = getattr(self, "runs", 0) + 1
        record_dir = Path(self.folder.name) / ("rec" if self.runs == 1 else f"rec{self.runs}")  # each run's journals are new files
        return me.run_cycles(self.api, self.connect, lambda: list(self.ports), "COM8", self.card, record_dir, cycles,
                             sleep=lambda seconds: None, clock=tick)

    def remove_staged_cycle(self, cycle=1):
        # These subcases model separate runs after the host explicitly removes
        # its prior retained file; normal reruns must hit the collision refusal.
        (self.api.root("E") / "WG" / me.cycle_stage_name(self.card, cycle)).unlink(missing_ok=True)

    def test_cycles_hand_the_card_over_without_removing_the_device(self):
        result = self.run_cycles(3)
        self.assertTrue(result["pass"], result)
        self.assertEqual(result["completed"], 3)
        self.assertTrue(all(all(item["checks"].values()) for item in result["results"]))
        names = [item["staged_name"] for item in result["results"]]
        self.assertEqual(names, ["C0000001.WAV", "C0000002.WAV", "C0000003.WAV"])
        self.assertEqual(len(set(names)), 3)
        self.assertTrue(all((self.api.root("E") / "WG" / name).read_bytes() == self.card.read_bytes() for name in names))
        self.assertEqual(self.board.media_state, 3)
        self.assertEqual([dismounted for _, dismounted in self.sessions], [False, True] * 3,
                         "MEDIA LOCAL is asserted only after the dismount and eject")
        self.assertTrue(all(transport.closed for transport, _ in self.sessions))
        journals = sorted(path.name for path in (Path(self.folder.name) / "rec").iterdir())
        self.assertEqual(journals, ["cycle-01.jsonl", "cycle-02.jsonl", "cycle-03.jsonl"])

    def test_cycles_bind_storage_to_the_requested_cdc_serial(self):
        self.api.disks["E"] = {**BOARD, "serial": "SERIAL"}
        self.api.disks["F"] = {**BOARD, "serial": "OTHER"}
        self.api.mounted["F"] = True
        result = me.run_cycles(self.api, self.connect, lambda: list(self.ports), "COM8", self.card,
                               Path(self.folder.name) / "serial-run", 1, expected_serial="SERIAL", sleep=lambda _: None)
        self.assertTrue(result["pass"], result)
        self.assertTrue(self.api.mounted["F"])
        self.assertFalse(result["results"][0]["process_elevated"])

    def test_unconfirmed_eject_never_requests_local_ownership(self):
        self.board.latches = False
        result = self.run_cycles(3)
        self.assertFalse(result["pass"])
        self.assertEqual(result["completed"], 1)
        self.assertEqual(self.board.media_state, 2)
        journal = (Path(self.folder.name) / "rec" / "cycle-01.jsonl").read_text()
        self.assertNotIn("MEDIA LOCAL", [json.loads(line).get("command") for line in journal.splitlines()])

    def test_a_later_run_refuses_its_existing_cycle_name_without_changing_it(self):
        first = self.run_cycles(2)
        self.assertTrue(first["pass"], first)
        target = self.api.root("E") / "WG" / "C0000001.WAV"
        original = target.read_bytes()
        second = self.run_cycles(1)
        self.assertFalse(second["pass"])
        self.assertEqual(second["completed"], 1)
        self.assertIn("refusing to overwrite existing staged file", second["results"][0]["error"])
        self.assertEqual(second["results"][0]["exit_code"], me.EXIT_REFUSED)
        self.assertEqual(target.read_bytes(), original)

    def test_cycle_names_preserve_extension_and_refuse_unrepresentable_ranges(self):
        self.assertEqual(me.cycle_stage_name(Path("fixture.bin"), 12), "C0000012.BIN")
        self.assertEqual(me.cycle_stage_name(Path("fixture"), 12), "C0000012")
        with self.assertRaisesRegex(me.StagingError, "cycle number"):
            me.cycle_stage_name(Path("fixture.wav"), me.MAX_CYCLE_NAME + 1)
        with self.assertRaisesRegex(me.StagingError, "extension"):
            me.cycle_stage_name(Path("fixture.wave"), 1)

    def test_a_vanished_port_or_a_new_assert_fails_the_cycle(self):
        original = self.api.on_eject

        def eject_and_lose_the_port():
            original()
            self.ports.remove("COM8")

        self.api.on_eject = eject_and_lose_the_port
        result = self.run_cycles(1)
        self.assertFalse(result["pass"])
        self.assertFalse(result["results"][0]["checks"]["port_listed"])
        self.ports.append("COM8")
        self.remove_staged_cycle()
        self.api.on_eject = lambda: (original(), setattr(self.board, "asserts", self.board.asserts + 2))
        result = self.run_cycles(1)
        self.assertEqual(result["results"][0]["usb_stack_asserts_delta"], 2)
        self.assertFalse(result["results"][0]["checks"]["no_new_asserts"])

    def test_the_assert_count_is_compared_across_its_32_bit_wrap(self):
        original = self.api.on_eject
        self.board.asserts = 0xFFFFFFFF
        result = self.run_cycles(1)
        self.assertEqual(result["results"][0]["usb_stack_asserts_delta"], 0)
        self.assertTrue(result["pass"], "a counter at its maximum with no new assert passes")
        self.remove_staged_cycle()
        self.api.on_eject = lambda: (original(), setattr(self.board, "asserts", (self.board.asserts + 2) & 0xFFFFFFFF))
        result = self.run_cycles(1)
        self.assertEqual(self.board.asserts, 1, "the board's uint32 counter wrapped")
        self.assertEqual(result["results"][0]["usb_stack_asserts_delta"], 2, "two asserts across the wrap")
        self.assertFalse(result["results"][0]["checks"]["no_new_asserts"])

    def test_the_board_must_show_the_eject_and_no_hold(self):
        self.board.latches = False
        result = self.run_cycles(1)
        self.assertEqual(result["results"][0]["checks"]["msc_ejected_1"], False, "an eject the board did not latch fails the cycle")
        self.assertFalse(result["pass"])
        self.remove_staged_cycle()
        self.board.latches = True
        self.board.hold = 1
        result = self.run_cycles(1)
        self.assertEqual(result["results"][0]["checks"]["msc_host_hold_0"], False, "a host hold left after the eject fails it")
        self.board.hold = 0
        self.board.media_state = 3

    def test_a_refused_media_local_or_a_drive_that_never_comes_stops_the_run(self):
        self.board.refuse_local = True
        result = self.run_cycles(3)
        self.assertEqual((result["pass"], result["completed"]), (False, 1))
        self.assertIn("HOST_LOCKED", result["results"][0]["error"])
        self.board.refuse_local = False
        self.board.unmount_after_host = True
        result = self.run_cycles(2)
        self.assertEqual((result["pass"], result["completed"]), (False, 1))
        self.assertIn("no mounted volume", result["results"][0]["error"], "the drive is there, its medium never mounted")


class Win32Tests(unittest.TestCase):
    """The parts of the real Win32 layer that need no drive: the descriptor's layout, and an unbuffered read of a local file."""

    def test_the_device_descriptor_is_read_as_windows_lays_it_out(self):
        strings = b"WFG\0RT1170 SD LOADER\x000001\0\0"
        header = 36 + 4  # STORAGE_DEVICE_DESCRIPTOR up to RawPropertiesLength, then RawDeviceProperties[1] padded
        vendor = header
        product = vendor + len(b"WFG\0")
        revision = product + len(b"RT1170 SD LOADER\0")
        raw = bytearray(header) + strings
        raw[0:36] = (1).to_bytes(4, "little") + len(raw).to_bytes(4, "little") + bytes([0, 0, 1, 0]) + vendor.to_bytes(4, "little") + \
            product.to_bytes(4, "little") + revision.to_bytes(4, "little") + bytes(4) + (7).to_bytes(4, "little") + bytes(4)
        identity = me.parse_device_descriptor(bytes(raw))
        self.assertEqual((identity["vendor"], identity["product"], identity["revision"], identity["removable_media"]),
                         ("WFG", "RT1170 SD LOADER", "0001", True))
        self.assertTrue(me.is_board(identity))
        self.assertIsNone(me.parse_device_descriptor(b"\0" * 35))

    @unittest.skipUnless(sys.platform == "win32", "Win32 only")
    def test_an_unbuffered_read_of_a_local_file(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "readback.bin"
            data = bytes(range(251)) * 70000
            path.write_bytes(data)
            digest, size, seconds = me.Windows().read_unbuffered(path)
        self.assertEqual((digest, size), (hashlib.sha256(data).hexdigest(), len(data)))
        self.assertGreaterEqual(seconds, 0.0)


class CommandLineTests(unittest.TestCase):
    def test_find_through_the_command_line(self):
        with tempfile.TemporaryDirectory() as folder:
            api = FakeWindows(Path(folder))
            stdout = io.StringIO()
            with mock.patch.object(me, "Windows", lambda: api), mock.patch.object(me.sys, "platform", "win32"), \
                    mock.patch("sys.stdout", stdout):
                self.assertEqual(me.main(["find"]), me.EXIT_OK)
            self.assertEqual([drive["letter"] for drive in json.loads(stdout.getvalue())["drives"] if drive["board"]], ["E"])
            stderr = io.StringIO()
            with mock.patch.object(me, "Windows", lambda: api), mock.patch.object(me.sys, "platform", "win32"), \
                    mock.patch("sys.stderr", stderr):
                self.assertEqual(me.main(["eject", "--drive", "C"]), me.EXIT_REFUSED)
            self.assertIn("not the board's drive", stderr.getvalue())
        with mock.patch.object(me.sys, "platform", "linux"), mock.patch("sys.stderr", io.StringIO()):
            self.assertEqual(me.main(["find"]), me.EXIT_REFUSED)


if __name__ == "__main__":
    unittest.main()
