#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""CDC session storm against the RT1170 player: the bench row of #32 (TinyUSB EP0 assertions).

Each burst opens --sessions back-to-back CDC sessions, each one: open the port,
INFO? (the fixture's identity), USB?, close. Opening a session is what costs
the board EP0 control traffic (the port open, its line coding and DTR), so a
burst is a storm of control requests. With a capture command after `--`, the
tool starts it every --capture-every sessions (one at a time, never two), so
the stream opens and closes while the sessions run: window 3's mix, in which
84 sessions gave 4 assertions on 2325c43.

  python host/usb_storm.py --port COM8 --expected-serial SERIAL --record-dir storm `
      --bursts 12 --sessions 84 --capture-every 12 -- python capture.py --seconds 2 --force --out storm\\cap.wav

Before the first burst and after each one a session reads USB? and STATUS?,
and the tool prints per burst the change of usb_stack_asserts,
usb_assert_count, usb_ep0_reordered, usb_ep0_unordered (images with the EP0
order), uac_stream_opens, uac_short_packets, uac_missed_intervals and
uac_play_missed_intervals. Every session's requests go to that burst's journal
(<record-dir>/storm-NN.jsonl, a new file). Exit 0 when no assertion count
rose and every session succeeded; 1 when one rose or a session failed; 2 when
the tool could not run.

Stdlib and pyserial only (through live_control.py); it opens no audio device
itself: the capture command does.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import subprocess
import sys
import time

import live_control

# The USB? and STATUS? members a burst's report shows the change of; a member
# an older image does not report shows as null.
USB_DELTA_FIELDS = ("usb_stack_asserts", "usb_assert_count", "usb_ep0_reordered", "usb_ep0_unordered", "uac_stream_opens",
                    "uac_short_packets", "uac_missed_intervals")
STATUS_DELTA_FIELDS = ("uac_play_missed_intervals",)
# The counts that must not rise in a storm.
ASSERT_FIELDS = ("usb_stack_asserts", "usb_assert_count")
# How long a burst waits at its end for the capture command it started.
CAPTURE_WAIT_S = 120.0


def session(connect, journal, *, status: bool = False) -> dict:
    """One CDC session: INFO? (in connect), USB? and, with `status`, STATUS?, then close. The counters it read."""
    controller = connect(journal)
    try:
        counters = dict(controller.execute("USB?")["usb"])
        if status:
            counters.update(controller.execute("STATUS?")["status"])
        return counters
    finally:
        controller.transport.close()


def deltas(before: dict, after: dict) -> dict:
    """after - before for each reported field (both uint32 since boot), or None when either lacks it."""
    report = {}
    for name in USB_DELTA_FIELDS + STATUS_DELTA_FIELDS:
        old, new = before.get(name), after.get(name)
        report[name] = (new - old) & live_control.UINT32_MAX if type(old) is int and type(new) is int else None
    return report


class CaptureChurn:
    """The capture command, started every `every` sessions and never twice at once, so the stream opens and closes."""

    def __init__(self, command: list[str] | None, every: int, *, launch=subprocess.Popen):
        self.command = command
        self.every = every
        self.launch = launch
        self.process = None
        self.started = 0
        self.failed = 0

    def before_session(self, index: int) -> None:
        if not self.command or index % self.every:
            return
        if self.process is not None and self.process.poll() is None:
            return  # The previous capture still holds the stream.
        self._reap()
        self.process = self.launch(self.command)
        self.started += 1

    def finish(self, timeout: float = CAPTURE_WAIT_S) -> None:
        """Waits for the capture still running, if any (a capture that outlives `timeout` is killed and counts as failed)."""
        if self.process is None:
            return
        try:
            code = self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            code = None
        self.failed += 1 if code != 0 else 0
        self.process = None

    def _reap(self) -> None:
        if self.process is not None and self.process.poll() is not None:
            self.failed += 1 if self.process.returncode != 0 else 0
            self.process = None


def run_storm(connect, record_dir: Path, *, bursts: int, sessions: int, capture: CaptureChurn, pause_s: float = 2.0,
              sleep=time.sleep) -> dict:
    """The storm: a baseline, then `bursts` bursts of `sessions` sessions, each burst's change of the counters."""
    record_dir.mkdir(parents=True, exist_ok=True)
    with (record_dir / "storm-00.jsonl").open("x", encoding="utf-8", newline="\n") as journal:
        baseline = session(connect, journal, status=True)
    previous = baseline
    report = []
    for burst in range(1, bursts + 1):
        failed = []
        with (record_dir / f"storm-{burst:02d}.jsonl").open("x", encoding="utf-8", newline="\n") as journal:
            for index in range(sessions):
                capture.before_session(index)
                try:
                    session(connect, journal)
                except (live_control.ControlError, OSError) as error:
                    failed.append({"session": index + 1, "error": str(error)})
            capture.finish()
            sleep(pause_s)
            counters = session(connect, journal, status=True)
        report.append({"burst": burst, "sessions": sessions, "failed_sessions": failed, "captures_started": capture.started,
                       "delta": deltas(previous, counters)})
        previous = counters
    total = deltas(baseline, previous)
    rose = any(total[name] for name in ASSERT_FIELDS if total[name] is not None)
    failures = sum(len(item["failed_sessions"]) for item in report)
    return {"tool": "usb_storm.py", "bursts": report, "sessions": bursts * sessions, "failed_sessions": failures,
            "captures_started": capture.started, "captures_failed": capture.failed, "total_delta": total,
            "asserts_rose": rose, "pass": not rose and failures == 0}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True, help="explicit serial endpoint; never a remembered COM default")
    parser.add_argument("--expected-serial", required=True, help="expected fixture USB serial, checked against INFO in every session")
    parser.add_argument("--record-dir", type=Path, required=True, help="folder for the bursts' journals (new files)")
    parser.add_argument("--bursts", type=int, default=12)
    parser.add_argument("--sessions", type=int, default=84, help="sessions per burst")
    parser.add_argument("--capture-every", type=int, default=12, help="start the capture command every N sessions")
    parser.add_argument("--pause", type=float, default=2.0, help="seconds after a burst before its counters are read")
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("capture", nargs=argparse.REMAINDER, help="after --: the capture command that opens and closes the stream")
    args = parser.parse_args(argv)
    command = args.capture[1:] if args.capture[:1] == ["--"] else args.capture
    if min(args.bursts, args.sessions, args.capture_every) < 1 or not math.isfinite(args.pause) or args.pause < 0:
        print("usb storm: --bursts, --sessions and --capture-every must be at least 1, --pause 0 or more", file=sys.stderr)
        return 2

    def connect(journal):
        return live_control.open_session(args.port, args.expected_serial, journal=journal, timeout=args.timeout)

    try:
        result = run_storm(connect, args.record_dir, bursts=args.bursts, sessions=args.sessions,
                           capture=CaptureChurn(command or None, args.capture_every), pause_s=args.pause)
    except (live_control.ControlError, OSError, ValueError) as error:
        print(f"usb storm: {error}", file=sys.stderr)
        return 2
    print(json.dumps(result, allow_nan=False))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
