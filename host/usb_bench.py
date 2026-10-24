#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Native Windows USB bench launcher for either board; protocol and media ownership stay in the existing helpers.

Profiles contain board, serial and capture_name (null until selected on this PC).
Port discovery never opens unmatched devices. Audio names are operator-associated
with the fixture; enumeration cannot prove that association. Capture alone is not
a continuity or negotiated-24-bit acceptance test. Each operation keeps new logs.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[1]
BOARDS = {"MIMXRT1170-EVK": "RT1170", "MIMXRT1064-EVK": "RT1064"}


def read_profile(path: Path) -> dict:
    profile = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(profile, dict) or set(profile) != {"board", "serial", "capture_name"}:
        raise ValueError("profile must contain exactly board, serial and capture_name")
    if not isinstance(profile["board"], str) or profile["board"] not in BOARDS:
        raise ValueError("unsupported board in profile")
    serial = profile["serial"]
    if not isinstance(serial, str) or not serial or not serial.isascii() or not serial.isalnum():
        raise ValueError("profile serial must be the fixture's nonempty ASCII USB serial")
    name = profile["capture_name"]
    if name is not None and (not isinstance(name, str) or BOARDS[profile["board"]] not in name
                             or any(char in name for char in "\r\n\0")):
        raise ValueError("capture_name must be null or the exact physical board endpoint name")
    return profile


def select_port(ports, serial: str) -> str:
    matches = [port.device for port in ports if port.vid == 0xCAFE and port.pid in (0x4012, 0x4013)
               and (port.serial_number or "").casefold() == serial.casefold()]
    if len(matches) != 1:
        raise ValueError(f"expected one player CDC port for serial {serial}, found {matches}; connect target USB")
    return matches[0]


def capture_device(devices: list[dict], profile: dict) -> str:
    name = profile["capture_name"]
    if name is None:
        raise ValueError("capture_name is not configured; associate the physical endpoint using devices first")
    if sum(device.get("native_name") == name for device in devices) != 1:
        raise ValueError(f"expected one exact recording endpoint named {name!r}; no default fallback")
    return name


def recorder_path() -> Path:
    for candidate in (ROOT / "bin/waveform_capture.exe", ROOT / "build/host-release/host/capture/waveform_capture.exe"):
        if candidate.is_file():
            return candidate
    raise ValueError("waveform_capture.exe missing: deploy bin/ or build host-release")


def audio_devices(recorder: Path) -> list[dict]:
    result = subprocess.run([str(recorder), "--backend", "wasapi-raw", "--list-devices", "--json"],
                            capture_output=True, text=True, encoding="utf-8", check=True, timeout=30)
    return [json.loads(line) for line in result.stdout.splitlines() if line.strip()]


def logged_run(command: list[str], run: Path, label: str, *, timeout: int = 60) -> None:
    with (run / f"{label}.stdout.log").open("xb") as out, (run / f"{label}.stderr.log").open("xb") as err:
        subprocess.run(command, stdout=out, stderr=err, check=True, timeout=timeout)


def logged_bounded_capture(command: list[str], run: Path, *, timeout: int) -> None:
    """Accept the recorder's deliberate DURATION_LIMIT exit with a nonempty WAV."""
    stdout_path = run / "capture.stdout.log"
    with stdout_path.open("xb") as out, (run / "capture.stderr.log").open("xb") as err:
        completed = subprocess.run(command, stdout=out, stderr=err, check=False, timeout=timeout)

    try:
        records = [json.loads(line) for line in stdout_path.read_text(encoding="utf-8").splitlines() if line.strip()]
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValueError("capture did not write a valid JSON record stream") from error
    ready = [record for record in records if isinstance(record, dict) and record.get("record") == "READY"]
    final = [record for record in records if isinstance(record, dict) and record.get("record") == "FINAL"]
    if (len(ready) != 1 or len(final) != 1 or not isinstance(ready[0].get("capture_id"), str)
            or not ready[0]["capture_id"] or ready[0]["capture_id"] != final[0].get("capture_id")):
        raise ValueError("capture did not write matching READY and FINAL records")
    if completed.returncode not in (0, 1) or final[0].get("completion_reason") != "DURATION_LIMIT":
        raise ValueError(f"capture did not finish at its duration limit (exit {completed.returncode})")
    written = final[0].get("samples_written")
    if not isinstance(written, int) or written <= 0 or written != final[0].get("samples_delivered"):
        raise ValueError("capture did not write every delivered sample")
    if not (run / "capture.wav").is_file() or (run / "capture.wav").stat().st_size <= 44:
        raise ValueError("capture did not leave a nonempty WAV")


def control_args(profile: dict, port: str, record: Path, commands: list[str], dismounted: bool = False) -> list[str]:
    args = [sys.executable, str(ROOT / "host/live_control.py"), "--port", port,
            "--expected-serial", profile["serial"], "--expected-board", profile["board"], "--record", str(record)]
    if dismounted:
        args.append("--host-volume-dismounted")
    return args + ["command", *commands]


def perform(args, profile: dict, port: str, run: Path) -> None:
    # INFO verification precedes card writes, playback commands or opening audio.
    logged_run(control_args(profile, port, run / "identity.jsonl", ["INFO?", "STATUS?", "USB?"]), run, "identity")
    if args.action == "control":
        logged_run(control_args(profile, port, run / "control.jsonl", args.commands, args.host_volume_dismounted),
                   run, "control")
    elif args.action == "stage":
        logged_run([sys.executable, str(ROOT / "host/media_eject.py"), "stage", str(args.file.resolve()),
                    "--name", args.name, "--expected-serial", profile["serial"], "--eject"], run, "stage", timeout=600)
        # The existing helper confirms copy/read-back/lock/dismount/eject first.
        logged_run(control_args(profile, port, run / "local.jsonl", ["MEDIA LOCAL"], True), run, "local")
    elif args.action == "capture":
        recorder = recorder_path()
        name = capture_device(audio_devices(recorder), profile)
        logged_bounded_capture([str(recorder), "--backend", "wasapi-raw", "--device-name", name,
                                "--output", str(run / "capture.wav"), "--sample-rate", "48000", "--channel", args.channel,
                                "--max-seconds", str(args.seconds)], run, timeout=args.seconds + 30)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--runs-root", type=Path, default=ROOT / "build/usb-bench")
    actions = parser.add_subparsers(dest="action", required=True)
    actions.add_parser("devices", help="list serial and physical audio endpoints without opening a CDC session")
    actions.add_parser("check", help="discover the configured serial and verify INFO, STATUS and USB telemetry")
    control = actions.add_parser("control", help="send quoted WFG-LIVE/1 commands after checking the fixture")
    control.add_argument("--host-volume-dismounted", action="store_true")
    control.add_argument("commands", nargs="+")
    stage = actions.add_parser("stage", help="stage onto an already host-owned card, eject, then MEDIA LOCAL")
    stage.add_argument("file", type=Path)
    stage.add_argument("--name", required=True, help="new, unused upper-case 8.3 destination name")
    capture = actions.add_parser("capture", help="bounded native capture; launch control in another shell")
    capture.add_argument("--seconds", type=int, default=30)
    capture.add_argument("--channel", choices=("left", "right", "average"), default="left")
    args = parser.parse_args(argv)
    run = None
    try:
        from serial.tools import list_ports
        ports = list(list_ports.comports())
        if args.action == "devices":
            print(json.dumps({"ports": [{"port": p.device, "vid": p.vid, "pid": p.pid, "serial": p.serial_number,
                                          "description": p.description} for p in ports],
                              "audio": audio_devices(recorder_path())}, indent=2))
            return 0
        if args.profile is None:
            raise ValueError("--profile is required for fixture operations")
        if args.action == "capture" and not 1 <= args.seconds <= 3600:
            raise ValueError("--seconds must be 1..3600")
        profile = read_profile(args.profile)
        port = select_port(ports, profile["serial"])
        run = args.runs_root.resolve() / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:12])
        run.mkdir(parents=True, exist_ok=False)
        (run / "fixture.json").write_text(json.dumps({**profile, "port": port}, indent=2), encoding="utf-8")
        print(json.dumps({"run": str(run), "action": args.action, "board": profile["board"], "serial": profile["serial"]}), flush=True)
        perform(args, profile, port, run)
        (run / "result.json").write_text(json.dumps({"action": args.action, "completed": True,
                                                    "physical_acceptance": "not_evaluated"}), encoding="utf-8")
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"USB bench: {error}; logs: {run or 'no device operation started'}", file=sys.stderr)
        if run is not None and run.is_dir():
            (run / "error.txt").write_text(str(error), encoding="utf-8")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
