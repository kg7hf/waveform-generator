# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

import io
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
import dd008
import tx_upload as upload


SERIAL = "0123456789ABCDEF"


class FakeMux:
    def __init__(self):
        self.sent = []
        self.pending = []
        self.mode = "600L"
        self.output = "GEN.WAV"
        self.state = 0
        self.payload = bytearray()
        self.input_name = None
        self.bad_count = False
        self.identity_serial = SERIAL

    def respond(self, text):
        wire = (text + "\n").encode("ascii")
        # Exercise response fragmentation independently of line boundaries.
        self.pending.extend((dd008.ROLE_CONTROL, wire[index:index + 7]) for index in range(0, len(wire), 7))

    def send(self, role, payload):
        self.sent.append((role, payload))
        if role == dd008.ROLE_DATA:
            self.payload.extend(payload)
            self.state = 1
            self.respond(f"DATA:{len(self.payload) + int(self.bad_count)}")
            return
        command = payload.decode("ascii").strip()
        if command == "CMD:MODEM INFO:?":
            self.respond("MODEM INFO:VERSION=0.2.0;BOARD=MIMXRT1170-EVK;KIND=waveform-generator;"
                         f"SERIAL={self.identity_serial};PROTOCOL=DD008/1;MAX_UPLOAD=1048576;BUILD_ID=wfg-test")
        elif command == "CMD:STATUS:?":
            complete = self.state == 3
            self.respond(f"STATUS:WAVEFORM FILE:STATE={self.state};FILE={self.output};PAYLOAD={len(self.payload)};"
                         f"FRAMES={123456 if complete else 0};TOTAL={123456 if complete else 0};SHA256=" +
                         ("b" * 64 if complete else "") + ";ERROR=none")
        elif command == "CMD:RESET MDM":
            self.payload.clear()
            self.state = 0
            self.respond("OK")
        elif command == "CMD:DATA RATE:?":
            self.respond(self.mode)
        elif command.startswith("CMD:DATA RATE:"):
            self.mode = command.removeprefix("CMD:DATA RATE:")
            self.respond("OK")
        elif command.startswith("CMD:WAV FILE:"):
            self.output = command.removeprefix("CMD:WAV FILE:")
            self.respond("OK")
        elif command == "CMD:SENDBUFFER":
            self.state = 3
            self.respond("STATUS:TX:GENERATING")
        elif command.startswith("CMD:TX FILE:"):
            self.input_name, self.output = command.removeprefix("CMD:TX FILE:").split(":")
            self.payload = bytearray(b"payload read from SD")
            self.state = 3
            self.respond("STATUS:TX:GENERATING")
        else:
            raise AssertionError(command)

    def poll(self, timeout):
        return [self.pending.pop(0)] if self.pending else []


class UploadTests(unittest.TestCase):
    def client(self):
        mux = FakeMux()
        journal = io.StringIO()
        client = upload.UploadClient(mux, SERIAL, journal=journal)
        client.identify()
        return client, mux, journal

    def test_identity_is_required_and_wrong_serial_blocks_mutation(self):
        mux = FakeMux()
        client = upload.UploadClient(mux, SERIAL)
        with self.assertRaises(upload.UploadError):
            client.generate(output="A.WAV", mode="600L", payload=b"a")
        self.assertEqual(mux.sent, [])
        mux.identity_serial = "WRONG"
        with self.assertRaises(upload.UploadError):
            client.identify()
        with self.assertRaises(upload.UploadError):
            client.exchange("CMD:RESET MDM", "OK")
        self.assertEqual(len(mux.sent), 1)

    def test_upload_acknowledges_each_chunk_before_next_and_retains_hash_record(self):
        client, mux, journal = self.client()
        payload = bytes(range(256)) * 2 + b"tail"
        result = client.generate(output="TEST001.WAV", mode="1200S", payload=payload)
        self.assertEqual(mux.payload, payload)
        self.assertEqual([len(data) for role, data in mux.sent if role == dd008.ROLE_DATA], [256, 256, 4])
        self.assertEqual(result["FILE"], "TEST001.WAV")
        self.assertEqual(result["PAYLOAD"], "516")
        records = [json.loads(line) for line in journal.getvalue().splitlines()]
        self.assertEqual([row["record"] for row in records].count("data"), 3)
        self.assertEqual(records[-1]["record"], "complete")
        self.assertTrue(any(row.get("text") == "DATA:256" for row in records))

    def test_sd_payload_uses_file_command_without_upload(self):
        client, mux, _ = self.client()
        result = client.generate(output="OUT.WAV", mode="4800U", sd_input="INPUT.BIN")
        self.assertEqual(mux.input_name, "INPUT.BIN")
        self.assertEqual(result["FILE"], "OUT.WAV")
        self.assertFalse(any(role == dd008.ROLE_DATA for role, _ in mux.sent))
        self.assertIn((dd008.ROLE_CONTROL, b"CMD:TX FILE:INPUT.BIN:OUT.WAV\n"), mux.sent)

    def test_invalid_requests_do_not_mutate_device(self):
        client, mux, _ = self.client()
        for changes in ({"output": "../A.WAV"}, {"mode": "4800L"}, {"payload": b""},
                        {"payload": bytes(upload.MAX_UPLOAD + 1)}, {"sd_input": "BAD.BIN"}):
            with self.subTest(changes=changes):
                args = {"output": "A.WAV", "mode": "600L", "payload": b"data"}
                args.update(changes)
                with self.assertRaises(upload.UploadError):
                    client.generate(**args)
                self.assertEqual(len(mux.sent), 1)

    def test_bad_chunk_ack_is_uncertain_and_not_retried(self):
        client, mux, journal = self.client()
        mux.bad_count = True
        with self.assertRaisesRegex(upload.UploadError, "byte count"):
            client.generate(output="A.WAV", mode="600L", payload=b"x" * 1000)
        self.assertEqual(sum(role == dd008.ROLE_DATA for role, _ in mux.sent), 1)
        with self.assertRaises(upload.UploadError):
            client.exchange("CMD:SENDBUFFER", "STATUS:TX:GENERATING")
        self.assertTrue(any(json.loads(line)["record"] == "uncertain" for line in journal.getvalue().splitlines()))

    def test_existing_partial_upload_needs_explicit_reset(self):
        client, mux, _ = self.client()
        mux.state = 1
        with self.assertRaisesRegex(upload.UploadError, "partial upload"):
            client.generate(output="A.WAV", mode="600L", payload=b"abc")
        self.assertFalse(any(data.startswith(b"CMD:DATA RATE:") for _, data in mux.sent))
        client.generate(output="A.WAV", mode="600L", payload=b"abc", reset=True)
        self.assertEqual(mux.payload, b"abc")

    def test_duplicate_status_fields_rejected(self):
        with self.assertRaises(upload.UploadError):
            upload.fields("STATUS:A=1;A=2", "STATUS:")


class CodecTests(unittest.TestCase):
    def test_binary_roundtrip_fragmented_cobs_and_crc(self):
        payload = bytes(range(256))
        wire = dd008.encode_frame(dd008.STREAM_DATA, dd008.BINARY_DATA, 123, payload)
        decoder = dd008.StreamDecoder()
        frames = []
        for byte in wire:
            frames.extend(decoder.feed(bytes([byte])))
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].payload, payload)
        damaged = bytearray(dd008.cobs_decode(wire[:-1]))
        damaged[20] ^= 1
        with self.assertRaises(dd008.MuxError):
            dd008.decode_frame(dd008.cobs_encode(damaged))


if __name__ == "__main__":
    unittest.main()
