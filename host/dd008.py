# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

"""Dependency-free DD-008 host codec/session used by diagnostics and tests."""

from __future__ import annotations

import dataclasses
import struct
import time
from typing import Protocol


VERSION = 1
MAX_PAYLOAD = 1024
MANDATORY_CAPABILITIES = 0x00000003

HELLO = 0x01
HELLO_ACK = 0x02
CHANNEL_OPEN = 0x10
CHANNEL_CLOSE = 0x11
STREAM_DATA = 0x12
RESET = 0x13
ERROR = 0x14

LINK = 0x00
CONTROL_STATUS = 0x01
BINARY_DATA = 0x02

ROLE_USB = "USB"
ROLE_CONTROL = "CONTROL"
ROLE_DATA = "DATA"


class MuxError(RuntimeError):
    """A malformed frame, handshake failure, or session-ordering failure."""


class RawChunk(Protocol):
    payload: bytes


class RawSerialSession(Protocol):
    @property
    def description(self) -> str: ...

    def poll(self, timeout_seconds: float) -> list[RawChunk]: ...

    def send(self, role: str, payload: bytes) -> None: ...

    def close(self) -> None: ...


@dataclasses.dataclass(frozen=True)
class Frame:
    frame_type: int
    channel: int
    sequence: int
    payload: bytes


def crc32c(payload: bytes) -> int:
    crc = 0xFFFFFFFF

    for value in payload:
        crc ^= value

        for _ in range(8):
            mask = -(crc & 1) & 0xFFFFFFFF
            crc = ((crc >> 1) ^ (0x82F63B78 & mask)) & 0xFFFFFFFF

    return crc ^ 0xFFFFFFFF


def cobs_encode(payload: bytes) -> bytes:
    output = bytearray((0,))
    code_index = 0
    code = 1

    for value in payload:
        if value == 0:
            output[code_index] = code
            code_index = len(output)
            output.append(0)
            code = 1
        else:
            output.append(value)
            code += 1

            if code == 0xFF:
                output[code_index] = code
                code_index = len(output)
                output.append(0)
                code = 1

    output[code_index] = code
    return bytes(output)


def cobs_decode(packet: bytes) -> bytes:
    output = bytearray()
    offset = 0

    while offset < len(packet):
        code = packet[offset]
        offset += 1

        if code == 0:
            raise MuxError("zero code in COBS packet")

        count = code - 1

        if count > len(packet) - offset:
            raise MuxError("truncated COBS packet")

        output.extend(packet[offset : offset + count])
        offset += count

        if code != 0xFF and offset < len(packet):
            output.append(0)

    return bytes(output)


def encode_frame(frame_type: int, channel: int, sequence: int, payload: bytes) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise MuxError("DD-008 payload exceeds 1024 bytes")

    header = struct.pack(
        "<2sBBBBHI", b"M1", VERSION, frame_type, channel, 0, len(payload), sequence
    )
    decoded = header + payload
    decoded += struct.pack("<I", crc32c(decoded))
    return cobs_encode(decoded) + b"\x00"


def decode_frame(packet: bytes) -> Frame:
    decoded = cobs_decode(packet)

    if len(decoded) < 16:
        raise MuxError("DD-008 frame is too short")

    magic, version, frame_type, channel, flags, length, sequence = struct.unpack(
        "<2sBBBBHI", decoded[:12]
    )

    if magic != b"M1":
        raise MuxError("DD-008 magic mismatch")
    if version != VERSION:
        raise MuxError(f"unsupported DD-008 version {version}")
    if flags != 0:
        raise MuxError("unsupported DD-008 flags")
    if length > MAX_PAYLOAD or len(decoded) != 12 + length + 4:
        raise MuxError("DD-008 length mismatch")

    expected_crc = struct.unpack("<I", decoded[-4:])[0]

    if crc32c(decoded[:-4]) != expected_crc:
        raise MuxError("DD-008 CRC-32C mismatch")

    return Frame(frame_type, channel, sequence, decoded[12:-4])


class StreamDecoder:
    def __init__(self) -> None:
        self._packet = bytearray()
        self._discarding = False

    def reset(self) -> None:
        self._packet.clear()
        self._discarding = False

    def feed(self, payload: bytes) -> list[Frame]:
        frames: list[Frame] = []

        for value in payload:
            if value == 0:
                if self._discarding:
                    self._discarding = False
                    self._packet.clear()
                    continue

                if self._packet:
                    frames.append(decode_frame(bytes(self._packet)))
                    self._packet.clear()

                continue

            if self._discarding:
                continue

            if len(self._packet) == 1045:
                self._packet.clear()
                self._discarding = True
                continue

            self._packet.append(value)

        return frames


class CdcMuxSerialSession:
    """Two DD-006 roles carried over one raw USB CDC serial session."""

    def __init__(self, raw: RawSerialSession, timeout_seconds: float = 3.0) -> None:
        self._raw = raw
        self._decoder = StreamDecoder()
        self._next_transmit_sequence = 1
        self._expected_receive_sequence = 1
        self._open_channels: set[int] = set()
        self._negotiated_payload = 0

        try:
            self._handshake(timeout_seconds)
            self._open(CONTROL_STATUS)
            self._open(BINARY_DATA)
        except BaseException:
            self.close()
            raise

    @property
    def description(self) -> str:
        return f"{self._raw.description}, DD-008 control/data multiplex"

    def _write_frame(self, frame_type: int, channel: int, payload: bytes = b"") -> None:
        wire = encode_frame(frame_type, channel, self._next_transmit_sequence, payload)
        self._raw.send(ROLE_USB, wire)
        self._next_transmit_sequence = (self._next_transmit_sequence + 1) & 0xFFFFFFFF

    def _handshake(self, timeout_seconds: float) -> None:
        # A CDC driver may retain bytes written by the previous process after
        # its handle closes. Drain only bytes that are already pending before
        # issuing this session's HELLO, then let the device's HELLO reset define
        # the new epoch.
        for _ in range(16):
            if not self._raw.poll(0.01):
                break

        hello_payload = struct.pack("<BBHI", VERSION, VERSION, MAX_PAYLOAD, MANDATORY_CAPABILITIES)
        self._raw.send(ROLE_USB, b"\x00" + encode_frame(HELLO, LINK, 0, hello_payload))
        deadline = time.monotonic() + timeout_seconds

        while time.monotonic() < deadline:
            for chunk in self._raw.poll(min(0.05, max(0.0, deadline - time.monotonic()))):
                for frame in self._decoder.feed(chunk.payload):
                    if frame.frame_type != HELLO_ACK or frame.channel != LINK or frame.sequence != 0:
                        # Bytes already committed to a host serial queue can
                        # outlive the prior process/session. The leading empty
                        # packet plus HELLO establishes the new device epoch;
                        # discard only pre-ACK frames and begin sequence checks
                        # after the authoritative ACK arrives.
                        continue
                    if len(frame.payload) != 8:
                        raise MuxError("malformed DD-008 HELLO_ACK")

                    version, status, maximum, capabilities = struct.unpack("<BBHI", frame.payload)

                    if (
                        version != VERSION
                        or status != 0
                        or not 0 < maximum <= MAX_PAYLOAD
                        or capabilities & MANDATORY_CAPABILITIES != MANDATORY_CAPABILITIES
                    ):
                        raise MuxError("device rejected the DD-008 handshake")

                    self._negotiated_payload = maximum
                    return

        raise MuxError("timed out waiting for DD-008 HELLO_ACK")

    def _open(self, channel: int) -> None:
        self._write_frame(CHANNEL_OPEN, channel)
        self._open_channels.add(channel)

    def poll(self, timeout_seconds: float) -> list[tuple[str, bytes]]:
        received: list[tuple[str, bytes]] = []

        for chunk in self._raw.poll(timeout_seconds):
            for frame in self._decoder.feed(chunk.payload):
                if frame.sequence != self._expected_receive_sequence:
                    raise MuxError(
                        "DD-008 receive sequence mismatch: "
                        f"expected {self._expected_receive_sequence}, got {frame.sequence}"
                    )

                self._expected_receive_sequence = (
                    self._expected_receive_sequence + 1
                ) & 0xFFFFFFFF

                if frame.frame_type == ERROR:
                    code = struct.unpack("<H", frame.payload[:2])[0]
                    raise MuxError(f"device reported DD-008 error {code}")
                if frame.frame_type == RESET:
                    raise MuxError("device reset the DD-008 session")
                if frame.frame_type != STREAM_DATA or frame.channel not in self._open_channels:
                    raise MuxError("unexpected DD-008 device frame")

                role = ROLE_CONTROL if frame.channel == CONTROL_STATUS else ROLE_DATA
                received.append((role, frame.payload))

        return received

    def send(self, role: str, payload: bytes) -> None:
        channel = CONTROL_STATUS if role == ROLE_CONTROL else BINARY_DATA if role == ROLE_DATA else None

        if channel is None or channel not in self._open_channels:
            raise MuxError(f"invalid or closed DD-008 role {role!r}")

        for offset in range(0, len(payload), self._negotiated_payload):
            self._write_frame(STREAM_DATA, channel, payload[offset : offset + self._negotiated_payload])

    def close(self) -> None:
        raw = getattr(self, "_raw", None)

        if raw is not None:
            try:
                for channel in tuple(self._open_channels):
                    self._write_frame(CHANNEL_CLOSE, channel)
            except BaseException:
                pass

            self._open_channels.clear()
            self._raw = None
            raw.close()
