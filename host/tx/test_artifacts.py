#!/usr/bin/env python3
"""Native generator artifact checks, independent of a parent checkout."""
import argparse
import hashlib
import json
import subprocess
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from m110_score import expected_payload_from_sidecar
from signal_lab.render import load_source_sidecar


def digest(data):
    return hashlib.sha256(data).hexdigest()


def verify(output, payload):
    sidecar = json.loads(output.with_suffix('.JSON').read_text(encoding='utf-8'))
    stored = output.with_suffix('.BIN').read_bytes()
    assert stored == payload
    assert sidecar['payload']['sha256'] == digest(payload)
    assert sidecar['payload']['bytes'] == len(payload)
    resolved, _ = expected_payload_from_sidecar(output)
    assert Path(resolved).read_bytes() == payload
    assert Path(load_source_sidecar(output)['payload']['path']).read_bytes() == payload
    data = output.read_bytes()
    assert sidecar['wav']['sha256'] == digest(data)
    pcm = sidecar['wav']['pcm']
    assert pcm['sha256'] == digest(data[pcm['data_offset_bytes']:])
    assert sidecar['generator']['source_manifest_sha256'] not in ('', 'unidentified-source')
    if output.suffix.lower() == '.wav':
        with wave.open(str(output), 'rb') as wav:
            assert (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) == (48000, 1, 2)
            assert wav.getnframes() == sidecar['wav']['samples']
        assert data[-96000:] == bytes(96000)
    return data


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tx', required=True, type=Path)
    parser.add_argument('--scratch', required=True, type=Path)
    args = parser.parse_args()
    scratch = args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    tx = str(args.tx.resolve())
    first = scratch / 'TEXT.WAV'
    subprocess.run([tx, '600', 'short', str(first), 'hello', 'world'], check=True)
    original = verify(first, b'hello world')
    # Reusing the retained payload must not truncate it, and both entry paths agree.
    subprocess.run([tx, '600', 'short', str(first), '--file', str(first.with_suffix('.BIN'))], check=True)
    assert verify(first, b'hello world') == original
    raw = scratch / 'RAW.PCM'
    payload = bytes(range(256)) * 3
    source = scratch / 'INPUT.BIN'
    source.write_bytes(payload)
    subprocess.run([tx, '1200', 'long', str(raw), '--file', str(source)], check=True)
    verify(raw, payload)
    assert source.read_bytes() == payload
    # Output/input alias rejection must preserve an existing input byte for byte.
    result = subprocess.run([tx, '600', 'short', str(first), '--file', str(first)], capture_output=True)
    assert result.returncode != 0 and first.read_bytes() == original
    # A retrieved board artifact uses the encoder-agnostic schema. Only the
    # M110 scoring adapter translates its profile; the renderer preserves it.
    board = scratch / 'BOARD.WAV'
    board.with_suffix('.BIN').write_bytes(payload)
    metadata = {'schema': 'waveform-artifact/1', 'encoder': 'M110B', 'profile': '600:long',
                'payload': {'path': 'BOARD.BIN', 'bytes': len(payload), 'sha256': digest(payload)}}
    board.with_suffix('.JSON').write_text(json.dumps(metadata), encoding='utf-8')
    resolved, normalized = expected_payload_from_sidecar(board)
    assert Path(resolved).read_bytes() == payload and normalized['mode'] == '600L'
    assert normalized['kind'] == 'reference_perfect'
    source_reference = load_source_sidecar(board)
    assert source_reference['encoder'] == 'M110B' and source_reference['profile'] == '600:long'
    impaired = scratch / 'IMPAIRED.WAV'
    impaired.with_suffix('.json').write_text(json.dumps({'source_reference': source_reference}), encoding='utf-8')
    resolved, normalized = expected_payload_from_sidecar(impaired)
    assert Path(resolved).read_bytes() == payload and normalized['source_reference']['mode'] == '600L'
    metadata['encoder'] = 'TEST'
    board.with_suffix('.JSON').write_text(json.dumps(metadata), encoding='utf-8')
    assert 'mode' not in expected_payload_from_sidecar(board)[1]
    print('native artifact tests: text/file parity, retained payload, SHA256, PCM hash, raw/WAV and input preservation passed')


if __name__ == '__main__':
    main()
