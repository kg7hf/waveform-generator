#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Card staging for the RT1170 and RT1064 players without Explorer's Eject: a media-only eject of the board's SD volume.

Never use Explorer's Eject verb (or "Safely Remove") on the board's drive. In
some states (window 3: right after a board reset, while the drive showed no
medium) it removes the whole composite USB device, and the COM port and the
capture endpoint vanish with it until the board is reset (#33). This tool
sends only the storage-media requests, so the CDC and UAC2 functions stay up:

  find                    the PC's drives, the board's marked (no elevation needed)
  eject [--drive X]       lock and dismount the volume, allow medium removal,
                          eject the medium (START STOP UNIT, LoEj=1): the board
                          latches msc_ejected and the drive stays away
  stage FILE [--name N.WAV] [--drive X] [--eject]
                          copy FILE into \\WG\\ on the board's drive, flush it,
                          read it back past the Windows cache and compare
                          SHA-256; refuses an existing destination; with
                          --eject, eject the medium after the read-back
  cycle --port P --expected-serial S --record-dir D --file F [--cycles N]
                          the staging cycle, N times: MEDIA HOST, wait for the
                          drive, stage each copy as C0000001.WAV, C0000002.WAV,
                          etc. (source extension preserved), eject, STATUS? (msc_ejected 1,
                          msc_host_hold 0), MEDIA LOCAL, STATUS? (media_state
                          3), USB?, and the COM port still listed

The board's drive is the one whose disk reports the player's SCSI identity:
vendor "WFG", product "RT1170 SD LOADER" (the read-only image: "RT1170 SD
READ"). The tool refuses any other drive, and refuses to guess when the PC
shows more than one.

Privileges. Run from an ordinary shell first. The RT1064 FAT32 volume passed
copy, uncached read-back, lock, dismount and media-only eject with a non-elevated
Windows process. A read-write handle does not prove administrator status.
Windows policy and filesystem/device permissions can still refuse access.
The helper never launches UAC, resets the board or forces MEDIA LOCAL on a
failed handoff. Close open card files before retrying a refused lock.
Use --expected-serial to bind the storage volume to the selected fixture;
cycle uses its required serial for both CDC and storage selection.

Exit: 0 done; 1 a check failed (hash, STATUS, port); 2 refused (no single
board drive, bad arguments); 3 both volume-open modes were denied; 4 the volume stays
locked (a program has a file open on it); 5 a dismount, removal or eject
request failed. Stdlib and ctypes; `cycle` also needs pyserial (live_control.py).
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import string
import struct
import sys
import time

BOARD_VENDOR = "WFG"
BOARD_PRODUCTS = ("RT1170 SD LOADER", "RT1170 SD READ")
READ_ONLY_PRODUCT = "RT1170 SD READ"
STAGE_DIRECTORY = "WG"
# The names the player opens in \WG\: an upper-case 8.3 name, its extension
# optional, as PLAYLIST takes them (a card's .WAV and a scenario's .SCN keep
# to it too).
NAME_RULE = re.compile(r"[A-Z0-9_]{1,8}(?:\.[A-Z0-9_]{1,3})?")
CHUNK = 8 * 1024 * 1024
# Win32 (winioctl.h, winnt.h, winerror.h).
GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
FILE_SHARE_READ, FILE_SHARE_WRITE = 0x1, 0x2
OPEN_EXISTING = 3
FILE_FLAG_NO_BUFFERING, FILE_FLAG_SEQUENTIAL_SCAN = 0x20000000, 0x08000000
FSCTL_LOCK_VOLUME, FSCTL_DISMOUNT_VOLUME = 0x00090018, 0x00090020
IOCTL_STORAGE_MEDIA_REMOVAL, IOCTL_STORAGE_EJECT_MEDIA = 0x002D4804, 0x002D4808
IOCTL_STORAGE_QUERY_PROPERTY = 0x002D1400
ERROR_ACCESS_DENIED = 5
DRIVE_REMOVABLE, DRIVE_FIXED = 2, 3
# Lock attempts while a program closes its last file on the volume.
LOCK_ATTEMPTS = 20
LOCK_INTERVAL_S = 0.5
# How long a cycle waits for the drive after MEDIA HOST, and for the volume's
# letter to go after the eject.
MOUNT_TIMEOUT_S = 30.0
POLL_S = 0.5
# STATUS media_state values (rt1170/platform/waveform_msc.h).
MEDIA_HOST, MEDIA_LOCAL = 2, 3

# Legacy exit 3 reports denied volume access; it does not prove the process lacks elevation.
EXIT_OK, EXIT_CHECK, EXIT_REFUSED, EXIT_ELEVATION, EXIT_LOCKED, EXIT_REQUEST = 0, 1, 2, 3, 4, 5
MAX_CYCLE_NAME = 9_999_999


class StagingError(Exception):
    """A staging step failed; `exit_code` says how."""

    def __init__(self, message: str, exit_code: int):
        super().__init__(message)
        self.exit_code = exit_code


def utc() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


class Windows:
    """The Win32 calls the tool makes, through ctypes, created only when used (on Windows)."""

    def __init__(self):
        import ctypes
        from ctypes import wintypes

        self.ctypes = ctypes
        self.wintypes = wintypes
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                                         wintypes.DWORD, wintypes.HANDLE]
        kernel32.CreateFileW.restype = wintypes.HANDLE
        kernel32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD, wintypes.LPVOID,
                                             wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
        kernel32.DeviceIoControl.restype = wintypes.BOOL
        kernel32.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
        kernel32.ReadFile.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL
        kernel32.GetLogicalDrives.restype = wintypes.DWORD
        kernel32.GetDriveTypeW.argtypes = [wintypes.LPCWSTR]
        kernel32.GetDriveTypeW.restype = wintypes.UINT
        kernel32.GetVolumeInformationW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                                                   ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD), wintypes.LPWSTR,
                                                   wintypes.DWORD]
        kernel32.GetVolumeInformationW.restype = wintypes.BOOL
        kernel32.SetThreadErrorMode.argtypes = [wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
        kernel32.SetThreadErrorMode.restype = wintypes.BOOL
        kernel32.VirtualAlloc.argtypes = [wintypes.LPVOID, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD]
        kernel32.VirtualAlloc.restype = wintypes.LPVOID
        kernel32.VirtualFree.argtypes = [wintypes.LPVOID, ctypes.c_size_t, wintypes.DWORD]
        kernel32.VirtualFree.restype = wintypes.BOOL
        self.kernel32 = kernel32
        self.invalid = wintypes.HANDLE(-1).value

    def is_elevated(self) -> bool:
        """Report the current token, independently of which volume access succeeds."""
        shell32 = self.ctypes.WinDLL("shell32", use_last_error=True)
        shell32.IsUserAnAdmin.argtypes = []
        shell32.IsUserAnAdmin.restype = self.wintypes.BOOL
        return bool(shell32.IsUserAnAdmin())

    @staticmethod
    def root(letter: str) -> Path:
        return Path(f"{letter}:\\")

    def letters(self) -> list[str]:
        mask = self.kernel32.GetLogicalDrives()
        return [letter for index, letter in enumerate(string.ascii_uppercase) if mask & (1 << index)
                and self.kernel32.GetDriveTypeW(f"{letter}:\\") in (DRIVE_REMOVABLE, DRIVE_FIXED)]

    def volume_label(self, letter: str) -> str | None:
        """The mounted volume's label, or None when no volume is mounted (no medium, or ejected)."""
        wintypes = self.wintypes
        label, fs_name = self.ctypes.create_unicode_buffer(261), self.ctypes.create_unicode_buffer(261)
        serial, max_component, flags, old_mode = wintypes.DWORD(), wintypes.DWORD(), wintypes.DWORD(), wintypes.DWORD()
        self.kernel32.SetThreadErrorMode(0x8001, self.ctypes.byref(old_mode))  # no popup for an empty drive
        try:
            ok = self.kernel32.GetVolumeInformationW(f"{letter}:\\", label, 261, self.ctypes.byref(serial), self.ctypes.byref(max_component),
                                                     self.ctypes.byref(flags), fs_name, 261)
        finally:
            self.kernel32.SetThreadErrorMode(old_mode.value, None)
        return label.value if ok else None

    def open_volume(self, letter: str, access: int, flags: int = 0) -> int:
        handle = self.kernel32.CreateFileW(f"\\\\.\\{letter}:", access, FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, flags, None)
        if handle in (None, self.invalid):
            raise OSError(self.ctypes.get_last_error(), f"CreateFileW(\\\\.\\{letter}:) failed")
        return handle

    def ioctl(self, handle: int, code: int, payload: bytes = b"", output_bytes: int = 0) -> tuple[bool, int, bytes]:
        returned = self.wintypes.DWORD(0)
        source = self.ctypes.create_string_buffer(payload, len(payload)) if payload else None
        out = self.ctypes.create_string_buffer(output_bytes) if output_bytes else None
        ok = self.kernel32.DeviceIoControl(handle, code, source, len(payload), out, output_bytes, self.ctypes.byref(returned), None)
        error = 0 if ok else self.ctypes.get_last_error()
        return bool(ok), error, out.raw[:returned.value] if out is not None else b""

    def close(self, handle: int) -> None:
        self.kernel32.CloseHandle(handle)

    def disk_identity(self, letter: str) -> dict | None:
        """The SCSI identity of the disk under a letter (IOCTL_STORAGE_QUERY_PROPERTY on a handle with no access)."""
        try:
            handle = self.open_volume(letter, 0)
        except OSError:
            return None
        try:
            ok, _, raw = self.ioctl(handle, IOCTL_STORAGE_QUERY_PROPERTY, bytes(12), 1024)
        finally:
            self.close(handle)
        return parse_device_descriptor(raw) if ok else None

    def read_unbuffered(self, path: Path) -> tuple[str, int, float]:
        """SHA-256 and size of a file read past the Windows cache (FILE_FLAG_NO_BUFFERING), and the seconds it took."""
        handle = self.kernel32.CreateFileW(str(path), GENERIC_READ, FILE_SHARE_READ, None, OPEN_EXISTING,
                                           FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, None)
        if handle in (None, self.invalid):
            raise OSError(self.ctypes.get_last_error(), f"CreateFileW({path}, FILE_FLAG_NO_BUFFERING) failed")
        buffer = self.kernel32.VirtualAlloc(None, CHUNK, 0x3000, 0x04)  # MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
        digest, total, started = hashlib.sha256(), 0, time.perf_counter()
        try:
            while True:
                got = self.wintypes.DWORD()
                if not self.kernel32.ReadFile(handle, buffer, CHUNK, self.ctypes.byref(got), None):
                    raise OSError(self.ctypes.get_last_error(), f"ReadFile({path}) at offset {total} failed")
                if got.value == 0:
                    break
                digest.update((self.ctypes.c_ubyte * got.value).from_address(buffer))
                total += got.value
        finally:
            self.kernel32.VirtualFree(buffer, 0, 0x8000)  # MEM_RELEASE
            self.close(handle)
        return digest.hexdigest(), total, time.perf_counter() - started


def _c_string(raw: bytes, offset: int) -> str:
    if offset == 0 or offset >= len(raw):
        return ""
    end = raw.find(b"\0", offset)
    return raw[offset:end if end >= 0 else len(raw)].decode("ascii", "replace").strip()


def parse_device_descriptor(raw: bytes) -> dict | None:
    """STORAGE_DEVICE_DESCRIPTOR: vendor, product, revision, serial and whether the medium is removable."""
    if len(raw) < 36:
        return None
    (_version, _size, _type, _modifier, removable, _queueing, vendor, product, revision, serial, _bus, _raw) = struct.unpack_from(
        "<IIBBBBIIIIII", raw, 0)
    return {"vendor": _c_string(raw, vendor), "product": _c_string(raw, product), "revision": _c_string(raw, revision),
            "serial": _c_string(raw, serial), "removable_media": bool(removable)}


def is_board(identity: dict | None) -> bool:
    return identity is not None and identity.get("vendor") == BOARD_VENDOR and identity.get("product") in BOARD_PRODUCTS


def drives(api) -> list[dict]:
    """Every removable or fixed drive with its disk identity and volume label (None: no volume mounted)."""
    found = []
    for letter in api.letters():
        identity = api.disk_identity(letter)
        found.append({"letter": letter, "disk": identity, "volume_label": api.volume_label(letter), "board": is_board(identity)})
    return found


def board_drive(api, letter: str | None = None, *, mounted: bool = True, expected_serial: str | None = None) -> dict:
    """Select one WFG volume; supplied letter and USB serial must both match."""
    board = [drive for drive in drives(api) if drive["board"]]
    if expected_serial is not None:
        board = [drive for drive in board if drive["disk"].get("serial", "").casefold() == expected_serial.casefold()]
    if letter is not None:
        board = [drive for drive in board if drive["letter"] == letter.rstrip(":").upper()]
        if not board:
            raise StagingError(f"{letter} is not the board's drive (no disk {BOARD_VENDOR} {BOARD_PRODUCTS[0]} there): refused", EXIT_REFUSED)
    if len(board) != 1:
        raise StagingError(f"expected exactly one board drive, found {len(board)}: {[drive['letter'] for drive in board]} "
                           "(is the card the host's? MEDIA HOST)", EXIT_REFUSED)
    if mounted and board[0]["volume_label"] is None:
        raise StagingError(f"{board[0]['letter']}: has no mounted volume (no medium, or already ejected): nothing to eject or stage",
                           EXIT_REFUSED)
    return board[0]


def _require(step: str, ok: bool, error: int) -> None:
    if not ok:
        raise StagingError(f"{step} failed: Windows error {error}; no media handoff was confirmed", EXIT_REQUEST)


def _lock(api, handle: int, attempts: int, interval: float, sleep) -> tuple[bool, int, int]:
    """Bounded lock retries: access denied can mean an open file, regardless of elevation."""
    error = 0
    for attempt in range(1, attempts + 1):
        ok, error, _ = api.ioctl(handle, FSCTL_LOCK_VOLUME)
        if ok:
            return ok, error, attempt
        if attempt < attempts:
            sleep(interval)
    return False, error, attempts


def media_eject(api, letter: str | None = None, *, lock_attempts: int = LOCK_ATTEMPTS, lock_interval: float = LOCK_INTERVAL_S,
                expected_serial: str | None = None, sleep=time.sleep) -> dict:
    """Lock, dismount, allow removal, eject the medium of the board's drive only. The steps and which access worked."""
    drive = board_drive(api, letter, expected_serial=expected_serial)
    handle, access = None, None
    for mode, name in ((GENERIC_READ | GENERIC_WRITE, "read-write"), (GENERIC_READ, "read")):
        try:
            handle, access = api.open_volume(drive["letter"], mode), name
            break
        except OSError as error:
            if error.errno != ERROR_ACCESS_DENIED:
                raise StagingError(f"opening \\\\.\\{drive['letter']}: failed: {error}", EXIT_REQUEST) from error
    if handle is None:
        raise StagingError(f"opening the {drive['letter']}: volume: Windows denied both access modes (error 5). "
                           "Check device policy and permissions; elevation may be required on this system. "
                           "No dismount occurred: do not send MEDIA LOCAL", EXIT_ELEVATION)
    steps = {"drive": drive["letter"], "disk": drive["disk"], "access": access, "process_elevated": api.is_elevated(), "started_utc": utc()}
    try:
        ok, error, steps["lock_attempts"] = _lock(api, handle, lock_attempts, lock_interval, sleep)
        if not ok:
            raise StagingError(f"the volume stays locked after {lock_attempts} attempts (Windows error {error}): a program has a file "
                               "open on it or Windows policy denies locking; no dismount occurred", EXIT_LOCKED)
        _require("FSCTL_DISMOUNT_VOLUME", *api.ioctl(handle, FSCTL_DISMOUNT_VOLUME)[:2])
        _require("IOCTL_STORAGE_MEDIA_REMOVAL (allow)", *api.ioctl(handle, IOCTL_STORAGE_MEDIA_REMOVAL, b"\0")[:2])
        _require("IOCTL_STORAGE_EJECT_MEDIA", *api.ioctl(handle, IOCTL_STORAGE_EJECT_MEDIA)[:2])
    finally:
        api.close(handle)
    steps.update({"locked": True, "dismounted": True, "removal_allowed": True, "ejected": True, "ended_utc": utc()})
    return steps


def stage(api, source: Path, *, name: str | None = None, letter: str | None = None, eject: bool = False,
          expected_serial: str | None = None, sleep=time.sleep) -> dict:
    """Copy `source` to \\WG\\<name> on the board's drive, flush it, read it back past the cache and compare SHA-256."""
    name = name or source.name.upper()
    if not NAME_RULE.fullmatch(name):
        raise StagingError(f"{name}: the player takes an upper-case 8.3 name (letters, digits, underscore) in \\WG\\", EXIT_REFUSED)
    drive = board_drive(api, letter, expected_serial=expected_serial)
    if drive["disk"]["product"] == READ_ONLY_PRODUCT:
        raise StagingError("the board runs the read-only image (RT1170 SD READ): it refuses every write", EXIT_REFUSED)
    target_dir = api.root(drive["letter"]) / STAGE_DIRECTORY
    target = target_dir / name
    expected = hashlib.sha256()
    started = time.perf_counter()
    target_dir.mkdir(exist_ok=True)
    with source.open("rb") as reader:
        try:
            # Exclusive creation makes an existing WAV, SCN, or playlist an
            # explicit refusal and closes the check/create race. Never truncate
            # card data merely because the requested 8.3 name is already used.
            writer_context = target.open("xb")
        except FileExistsError as error:
            raise StagingError(f"{target}: refusing to overwrite existing staged file", EXIT_REFUSED) from error

        with writer_context as writer:
            while block := reader.read(CHUNK):
                expected.update(block)
                writer.write(block)
            writer.flush()
            os.fsync(writer.fileno())
    written_s = time.perf_counter() - started
    size = source.stat().st_size
    read_hash, read_size, read_s = api.read_unbuffered(target)
    result = {"drive": drive["letter"], "file": str(target), "bytes": size, "sha256": expected.hexdigest(), "read_back_sha256": read_hash,
              "read_back_bytes": read_size, "write_s": round(written_s, 3), "read_s": round(read_s, 3),
              "write_MBps": round(size / written_s / 1e6, 3) if written_s > 0 else None,
              "read_MBps": round(read_size / read_s / 1e6, 3) if read_s > 0 else None}
    result["match"] = read_hash == result["sha256"] and read_size == size
    if not result["match"]:
        raise StagingError(f"{target}: the read-back differs from the source (SHA-256 {read_hash}, {read_size} bytes)", EXIT_CHECK)
    if eject:
        result["eject"] = media_eject(api, drive["letter"], expected_serial=drive["disk"]["serial"], sleep=sleep)
    return result


def wait_for_drive(api, *, expected_serial: str | None = None, timeout: float = MOUNT_TIMEOUT_S, poll: float = POLL_S, clock=time.monotonic, sleep=time.sleep) -> dict:
    """The board's drive once its volume is mounted, after MEDIA HOST."""
    deadline = clock() + timeout
    while True:
        try:
            return board_drive(api, expected_serial=expected_serial)
        except StagingError:
            if clock() >= deadline:
                raise
        sleep(poll)


@dataclass
class CycleCheck:
    """One staging cycle's facts and whether each holds."""

    cycle: int
    facts: dict

    @property
    def passed(self) -> bool:
        return all(self.facts["checks"].values())


def run_cycle(api, connect, ports, port: str, source: Path, journal, cycle: int, *, expected_serial: str | None = None,
              sleep=time.sleep, clock=time.monotonic) -> CycleCheck:
    """MEDIA HOST, the drive mounts, stage and read back, eject, STATUS?, MEDIA LOCAL, STATUS?, USB?, the port still listed."""
    staged_name = cycle_stage_name(source, cycle)
    started = clock()
    controller = connect(journal, False)
    try:
        before = controller.execute("USB?")["usb"]
        controller.execute("MEDIA HOST")
    finally:
        controller.transport.close()
    drive = wait_for_drive(api, expected_serial=expected_serial, clock=clock, sleep=sleep)
    staged = stage(api, source, name=staged_name, letter=drive["letter"], expected_serial=drive["disk"]["serial"],
                   eject=True, sleep=sleep)
    controller = connect(journal, True)
    try:
        ejected = controller.execute("STATUS?")["status"]
        local = ejected
        if ejected.get("msc_ejected") == 1 and ejected.get("msc_host_hold") == 0:
            controller.execute("MEDIA LOCAL")
            local = controller.execute("STATUS?")["status"]
        after = controller.execute("USB?")["usb"]
    finally:
        controller.transport.close()
    listed = port in ports()
    asserts = (after.get("usb_stack_asserts", 0) - before.get("usb_stack_asserts", 0)) & 0xFFFFFFFF
    checks = {"read_back_match": staged["match"], "msc_ejected_1": ejected.get("msc_ejected") == 1,
              "msc_host_hold_0": ejected.get("msc_host_hold") == 0, "media_state_3": local.get("media_state") == MEDIA_LOCAL,
              "port_listed": listed, "no_new_asserts": asserts == 0}
    return CycleCheck(cycle, {"cycle": cycle, "staged_name": staged_name, "seconds": round(clock() - started, 3), "bytes": staged["bytes"],
                              "write_MBps": staged["write_MBps"], "read_MBps": staged["read_MBps"], "eject_access": staged["eject"]["access"],
                              "process_elevated": staged["eject"]["process_elevated"], "usb_stack_asserts_delta": asserts, "checks": checks})


def cycle_stage_name(source: Path, cycle: int) -> str:
    """A distinct deterministic 8.3 destination for one cycle's retained card copy."""
    if cycle < 1 or cycle > MAX_CYCLE_NAME:
        raise StagingError(f"cycle number must be in 1..{MAX_CYCLE_NAME} to form a unique 8.3 name", EXIT_REFUSED)
    name = f"C{cycle:07d}{source.suffix.upper()}"
    if not NAME_RULE.fullmatch(name):
        raise StagingError(f"{source.name}: cycle staging requires a source extension that fits an upper-case 8.3 name", EXIT_REFUSED)
    return name


def run_cycles(api, connect, ports, port: str, source: Path, record_dir: Path, cycles: int, *, sleep=time.sleep,
               clock=time.monotonic, expected_serial: str | None = None) -> dict:
    if cycles < 1 or cycles > MAX_CYCLE_NAME:
        raise StagingError(f"--cycles must be in 1..{MAX_CYCLE_NAME}", EXIT_REFUSED)
    cycle_stage_name(source, cycles)
    record_dir.mkdir(parents=True, exist_ok=True)
    results = []
    for cycle in range(1, cycles + 1):
        with (record_dir / f"cycle-{cycle:02d}.jsonl").open("x", encoding="utf-8", newline="\n") as journal:
            try:
                results.append(run_cycle(api, connect, ports, port, source, journal, cycle,
                                         expected_serial=expected_serial, sleep=sleep, clock=clock))
                if not results[-1].passed:
                    break
            except (StagingError, OSError, RuntimeError) as error:
                # A refused MEDIA HOST or MEDIA LOCAL, a lost session (live_control's
                # errors are RuntimeErrors), a drive that never came: the run stops here.
                code = error.exit_code if isinstance(error, StagingError) else EXIT_CHECK
                results.append(CycleCheck(cycle, {"cycle": cycle, "staged_name": cycle_stage_name(source, cycle),
                                                  "error": str(error), "exit_code": code, "checks": {"ran": False}}))
                break
    passed = len(results) == cycles and all(result.passed for result in results)
    return {"tool": "media_eject.py cycle", "file": str(source), "cycles": cycles, "completed": len(results),
            "pass": passed, "results": [result.facts for result in results]}


def _listed_ports() -> list[str]:
    from serial.tools import list_ports

    return [entry.device for entry in list_ports.comports()]


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    actions = parser.add_subparsers(dest="action", required=True)
    actions.add_parser("find", help="list the drives and mark the board's")
    eject = actions.add_parser("eject", help="media-only eject of the board's medium")
    eject.add_argument("--drive", help="the board's drive letter (default: the only board drive)")
    eject.add_argument("--expected-serial", help="require this storage USB serial, even when --drive is given")
    stage_parser = actions.add_parser("stage", help="copy a file into \\WG\\, read it back past the cache, compare")
    stage_parser.add_argument("file", type=Path)
    stage_parser.add_argument("--name", help="its 8.3 name on the card (default: the file's own, upper case)")
    stage_parser.add_argument("--drive")
    stage_parser.add_argument("--expected-serial", help="require this storage USB serial, even when --drive is given")
    stage_parser.add_argument("--eject", action="store_true", help="media-only eject once the read-back matches")
    cycle = actions.add_parser("cycle", help="the staging cycle of #33, N times, with its checks")
    cycle.add_argument("--port", required=True)
    cycle.add_argument("--expected-serial", required=True)
    cycle.add_argument("--expected-board", choices=("MIMXRT1170-EVK", "MIMXRT1064-EVK"), default="MIMXRT1170-EVK")
    cycle.add_argument("--record-dir", type=Path, required=True, help="folder for each cycle's journal (new files)")
    cycle.add_argument("--file", type=Path, required=True)
    cycle.add_argument("--cycles", type=int, default=20)
    cycle.add_argument("--timeout", type=float, default=3.0)
    args = parser.parse_args(argv)
    if sys.platform != "win32":
        print("media eject: Windows only (it sends Win32 volume requests)", file=sys.stderr)
        return EXIT_REFUSED
    try:
        api = Windows()
        if args.action == "find":
            result = {"drives": drives(api)}
        elif args.action == "eject":
            result = media_eject(api, args.drive, expected_serial=args.expected_serial)
        elif args.action == "stage":
            result = stage(api, args.file, name=args.name, letter=args.drive, eject=args.eject, expected_serial=args.expected_serial)
        else:
            if args.cycles < 1 or args.cycles > MAX_CYCLE_NAME or not math.isfinite(args.timeout) or args.timeout <= 0:
                raise StagingError(f"--cycles must be in 1..{MAX_CYCLE_NAME} and --timeout positive", EXIT_REFUSED)
            sys.path.insert(0, str(Path(__file__).resolve().parent))
            import live_control

            def connect(journal, dismounted):
                return live_control.open_session(args.port, args.expected_serial, journal=journal, timeout=args.timeout,
                                                 volume_dismounted=dismounted, expected_board=args.expected_board)

            result = run_cycles(api, connect, _listed_ports, args.port, args.file, args.record_dir, args.cycles,
                                expected_serial=args.expected_serial)
            print(json.dumps(result, allow_nan=False))
            return EXIT_OK if result["pass"] else EXIT_CHECK
    except StagingError as error:
        print(f"media eject: {error}", file=sys.stderr)
        return error.exit_code
    except OSError as error:
        print(f"media eject: {error}", file=sys.stderr)
        return EXIT_REQUEST
    print(json.dumps(result, allow_nan=False))
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
