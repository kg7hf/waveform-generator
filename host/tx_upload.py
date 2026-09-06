"""Create an M110 WAV on the board using DD008 control/data channels.

First dismount the board's host filesystem and transfer it to MEDIA LOCAL using
the existing controller, then close that connection. This utility explicitly
selects its port and verifies the board serial before changing configuration.
Examples (new output names preserve previously completed artifacts):

  python host/tx_upload.py --port COM8 --expected-serial 0123456789ABCDEF \
      --payload message.bin --output TEST001.WAV --mode 600L --record upload.jsonl
  python host/tx_upload.py --port COM8 --expected-serial 0123456789ABCDEF \
      --sd-input MESSAGE.BIN --output TEST002.WAV --mode 1200S --record tx.jsonl

The board retains the payload, WAV, and JSON manifest. Generation is separate
from playback; LOAD/PLAY selects the resulting WAV afterward. A command timeout
is uncertain and is never retried automatically. --reset explicitly abandons a
previous partial upload or generation before starting this request.
"""
from __future__ import annotations

import argparse
import dataclasses
import datetime
import hashlib
import json
import re
import sys
import time
from pathlib import Path

try:
    from . import dd008
except ImportError:
    import dd008

MODES = ("75S", "75L", "150S", "150L", "300S", "300L", "600S", "600L",
         "1200S", "1200L", "2400S", "2400L", "4800U")
MAX_UPLOAD = 1048576


class UploadError(RuntimeError):
    pass


def filename(value: str, extension: str) -> str:
    if not isinstance(value, str) or re.fullmatch(r"[A-Z0-9_]{1,8}\." + extension, value) is None:
        raise UploadError(f"expected an uppercase 8.3 {extension} filename")
    return value


def fields(line: str, prefix: str) -> dict[str, str]:
    if not line.startswith(prefix):
        raise UploadError(f"unexpected response: {line}")
    result = {}
    for field in line[len(prefix):].split(";"):
        name, separator, value = field.partition("=")
        if not separator or not name or name in result:
            raise UploadError("malformed or duplicate response field")
        result[name] = value
    return result


@dataclasses.dataclass
class RawChunk:
    payload: bytes


class SerialTransport:
    def __init__(self, port: str, expected_serial: str, timeout: float):
        try:
            import serial
            from serial.tools import list_ports
        except ImportError as error:
            raise UploadError("physical USB control requires the explicitly installed pyserial==3.5 dependency") from error
        matches = [item for item in list_ports.comports() if item.device.casefold() == port.casefold()]
        if len(matches) != 1 or matches[0].serial_number != expected_serial:
            raise UploadError("explicit port does not have the expected USB serial")
        self._serial = serial.Serial(port=port, baudrate=115200, timeout=0.05, write_timeout=timeout)
        self._timeout = timeout
        self.description = port

    def poll(self, timeout_seconds):
        self._serial.timeout = max(0.0, timeout_seconds)
        payload = self._serial.read(min(max(self._serial.in_waiting, 1), 4096))
        return [RawChunk(payload)] if payload else []

    def send(self, role, payload):
        if role != dd008.ROLE_USB:
            raise UploadError("raw transport accepts only USB bytes")
        deadline = time.monotonic() + self._timeout
        offset = 0
        while offset < len(payload):
            count = self._serial.write(payload[offset:])
            if not count or time.monotonic() >= deadline:
                raise UploadError("USB write is uncertain; reconnect and inspect status")
            offset += count

    def close(self):
        self._serial.close()


class UploadClient:
    def __init__(self, mux, expected_serial: str, *, timeout=10.0, journal=None, clock=time.monotonic):
        self.mux = mux
        self.expected_serial = expected_serial
        self.timeout = timeout
        self.journal = journal
        self.clock = clock
        self.identity = None
        self.buffer = bytearray()
        self.lines = []
        self.trusted = True

    def record(self, **entry):
        if self.journal is not None:
            self.journal.write(json.dumps({"schema": "wfg-tx.session/1", "utc": datetime.datetime.now(
                datetime.timezone.utc).isoformat(), **entry}, allow_nan=False) + "\n")
            self.journal.flush()

    def _line(self, deadline):
        while not self.lines:
            remaining = deadline - self.clock()
            if remaining <= 0:
                raise UploadError("acknowledgment timeout; command outcome is uncertain")
            for role, payload in self.mux.poll(min(0.05, remaining)):
                if role != dd008.ROLE_CONTROL:
                    raise UploadError("unexpected device binary data")
                for byte in payload:
                    if byte == 10:
                        self.lines.append(self.buffer.decode("ascii", "strict").removesuffix("\r"))
                        self.buffer.clear()
                        if len(self.lines) > 16:
                            raise UploadError("unsolicited response queue exceeds its bound")
                    else:
                        if len(self.buffer) >= 512 or byte < 32 or byte > 126:
                            raise UploadError("invalid or oversized control response")
                        self.buffer.append(byte)
        line = self.lines.pop(0)
        self.record(record="response", text=line)
        return line

    def exchange(self, command: str, prefix: str):
        if not self.trusted:
            raise UploadError("connection is uncertain; reconnect and inspect status")
        if self.identity is None and command != "CMD:MODEM INFO:?":
            raise UploadError("verify board identity before sending commands")
        wire = (command + "\n").encode("ascii")
        if len(wire) > 256 or "\n" in command or "\r" in command:
            raise UploadError("invalid command framing")
        self.record(record="command", text=command)
        try:
            self.mux.send(dd008.ROLE_CONTROL, wire)
            return self.wait(prefix)
        except Exception as error:
            self.trusted = False
            self.record(record="uncertain", error=str(error))
            raise

    def wait(self, prefix: str):
        deadline = self.clock() + self.timeout
        while True:
            line = self._line(deadline)
            if line.startswith("ERROR:"):
                raise UploadError(f"device rejected request: {line[6:]}")
            if line.startswith(prefix):
                return line
            if not line.startswith("STATUS:WAVEFORM FILE:"):
                raise UploadError(f"unexpected acknowledgment: {line}")

    def identify(self):
        info = fields(self.exchange("CMD:MODEM INFO:?", "MODEM INFO:"), "MODEM INFO:")
        if (info.get("KIND") != "waveform-generator" or info.get("BOARD") != "MIMXRT1170-EVK" or
                info.get("SERIAL") != self.expected_serial or info.get("PROTOCOL") != "DD008/1" or
                info.get("MAX_UPLOAD") != str(MAX_UPLOAD) or
                re.fullmatch(r"[0-9a-f]{64}", info.get("SOURCE_MANIFEST", "")) is None):
            self.trusted = False
            raise UploadError("board identity or artifact protocol capabilities mismatch")
        self.identity = info
        self.record(record="identity", info=info)
        return info

    def snapshot(self):
        result = fields(self.exchange("CMD:STATUS:?", "STATUS:WAVEFORM FILE:"), "STATUS:WAVEFORM FILE:")
        for key in ("STATE", "PAYLOAD", "FRAMES", "TOTAL"):
            value = result.get(key, "")
            if re.fullmatch(r"0|[1-9][0-9]{0,19}", value) is None or int(value) > (1 << 64) - 1:
                raise UploadError("invalid artifact status counter")
        if int(result["STATE"]) > 4:
            raise UploadError("unknown artifact state")
        filename(result.get("FILE"), "WAV")
        return result

    def generate(self, *, output: str, mode: str, payload: bytes | None = None,
                 sd_input: str | None = None, reset=False, generation_timeout=1800.0, sleep=time.sleep):
        filename(output, "WAV")
        if mode not in MODES or (payload is None) == (sd_input is None):
            raise UploadError("select one payload source and a supported M110 mode")
        if payload is not None and not 0 < len(payload) <= MAX_UPLOAD:
            raise UploadError("payload must contain 1 through 1048576 bytes")
        if sd_input is not None:
            filename(sd_input, "BIN")
        if reset:
            self.exchange("CMD:RESET MDM", "OK")
        before = self.snapshot()
        if before["STATE"] in ("1", "2"):
            raise UploadError("a partial upload or generation exists; inspect it or explicitly use --reset")
        self.exchange("CMD:DATA RATE:" + mode, "OK")
        if self.exchange("CMD:DATA RATE:?", mode) != mode:
            raise UploadError("device mode readback mismatch")
        if payload is not None:
            self.exchange("CMD:WAV FILE:" + output, "OK")
            self.record(record="source", payload_bytes=len(payload), payload_sha256=hashlib.sha256(payload).hexdigest())
            for offset in range(0, len(payload), 256):
                chunk = payload[offset:offset + 256]
                self.record(record="data", offset=offset, size=len(chunk), sha256=hashlib.sha256(chunk).hexdigest())
                try:
                    self.mux.send(dd008.ROLE_DATA, chunk)
                    if self.wait("DATA:") != f"DATA:{offset + len(chunk)}":
                        raise UploadError("upload acknowledgment byte count mismatch")
                except Exception as error:
                    self.trusted = False
                    self.record(record="uncertain", error=str(error))
                    raise
            self.exchange("CMD:SENDBUFFER", "STATUS:TX:GENERATING")
        else:
            self.record(record="source", sd_input=sd_input)
            self.exchange(f"CMD:TX FILE:{sd_input}:{output}", "STATUS:TX:GENERATING")
        deadline = self.clock() + generation_timeout
        while self.clock() < deadline:
            snapshot = self.snapshot()
            if snapshot["FILE"] != output:
                raise UploadError("artifact status selected a different output")
            if snapshot["STATE"] == "3":
                if (re.fullmatch(r"[0-9a-f]{64}", snapshot.get("SHA256", "")) is None or
                        snapshot["FRAMES"] != snapshot["TOTAL"] or int(snapshot["FRAMES"]) == 0 or
                        (payload is not None and int(snapshot["PAYLOAD"]) != len(payload))):
                    raise UploadError("completed artifact metadata is inconsistent")
                self.record(record="complete", status=snapshot)
                return snapshot
            if snapshot["STATE"] != "2":
                raise UploadError(f"generation did not complete: {snapshot}")
            sleep(0.1)
        raise UploadError("generation wait timed out; board job may still be running")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True)
    parser.add_argument("--expected-serial", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--mode", choices=MODES, default="600L")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--payload", type=Path)
    source.add_argument("--sd-input")
    parser.add_argument("--record", required=True, type=Path)
    parser.add_argument("--reset", action="store_true")
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--generation-timeout", type=float, default=1800.0)
    args = parser.parse_args(argv)
    mux = None
    try:
        filename(args.output, "WAV")
        if args.sd_input:
            filename(args.sd_input, "BIN")
        if not 0 < args.timeout <= 60 or not 0 < args.generation_timeout <= 86400:
            raise UploadError("timeouts must be finite, positive and bounded")
        if args.payload and not 0 < args.payload.stat().st_size <= MAX_UPLOAD:
            raise UploadError("payload must contain 1 through 1048576 bytes")
        payload = args.payload.read_bytes() if args.payload else None
        with args.record.open("x", encoding="utf-8", newline="\n") as journal:
            raw = SerialTransport(args.port, args.expected_serial, args.timeout)
            mux = dd008.CdcMuxSerialSession(raw, args.timeout)
            client = UploadClient(mux, args.expected_serial, timeout=args.timeout, journal=journal)
            client.identify()
            result = client.generate(output=args.output, mode=args.mode, payload=payload, sd_input=args.sd_input,
                                     reset=args.reset, generation_timeout=args.generation_timeout)
            print(json.dumps(result, indent=2))
        return 0
    except (OSError, ValueError, UnicodeError, UploadError, dd008.MuxError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    finally:
        if mux is not None:
            mux.close()


if __name__ == "__main__":
    raise SystemExit(main())
