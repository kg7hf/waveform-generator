#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Gate 0 (Phase 5a): the reference pathsim build against the lane's shipped binary.

pathsim_ref (channel-sim, the patched pathsim's own main.cpp) and the HF lane's
shipped pathsim.exe (rev-6, sha256 77461211...) run on identical 8 kHz inputs in
the work directory. Every output must be byte-identical (sha256). Where the lane
kept the output of its own run, the reference must reproduce it too:

  gov2400L   the golden pair: trial-chan.wav is pathsim's 8 kHz output (the
             lane's trial-out-8k.wav is its byte-identical re-write); the 48 kHz
             chain output must equal gov2400L-s60001.wav.
  P24L05     8 kHz input regenerated with the snapshot's resampler like
             run_pathsim.py; the 48 kHz chain output must equal the stimulus
             source P24L05-ps-48k.wav (the 8 kHz intermediates were not kept).
  P24L60     the campaign's 1-hour trial-in.wav; the output must equal its
             trial-chan.wav (written by the same binary, from the wave2 snapshot).

Further cases cover one, two and three paths, presets, a direct path, AWGN only,
SNR above and below 0 dB (both gain branches), no AWGN, the default seed, the
maximum seed, and the --truth-out export (truth file compared too).

The script only reads the lane tree; everything it writes stays under --work.
Every child process runs at below-normal priority. Exit status 0 means parity.
Guide: docs/modules/pathsim.md.
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import wave

import numpy as np

SHIPPED_PATHSIM_SHA256 = '77461211d592912310dfff5f4ad374e51cfd8c3c2e4905d33fa58b5b6bb1187c'
DEFAULT_SNAPSHOT = 'wave3-final'
FS8_HZ = 8000
CAMPAIGN_GAIN_DB = '-12'  # run_pathsim.py / pathsim-campaign.py: the resampler's 4th argument, verbatim
BELOW_NORMAL = getattr(subprocess, 'BELOW_NORMAL_PRIORITY_CLASS', 0)
PE_TIMESTAMP_OFFSET = 8  # bytes after the 'PE\0\0' signature: COFF TimeDateStamp
PE_CHECKSUM_OFFSET = 24 + 64  # optional header CheckSum (PE32+)


# ---- helpers copied verbatim from run_pathsim.py (themselves the campaign's read_wav/write_wav),
# ---- so regenerated 8 kHz inputs are byte-for-byte the lane's.
def read_wav(path):
    with wave.open(str(path), 'rb') as w:
        rate = w.getframerate()
        raw = w.readframes(w.getnframes())
    return rate, np.frombuffer(raw, dtype='<i2')


def write_wav(path, rate, pcm):
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.astype('<i2').tobytes())


def num(value) -> str:
    """The campaign passes str(float) for every pathsim number (e.g. '2.0', '17.6')."""
    return str(float(value))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def pe_identity_digest(path: Path) -> str:
    """sha256 of a PE image with the link TimeDateStamp and the header CheckSum zeroed."""
    image = bytearray(path.read_bytes())
    pe = struct.unpack_from('<I', image, 0x3C)[0]
    if image[pe:pe + 4] != b'PE\0\0':
        raise ValueError(f'{path}: not a PE image')
    struct.pack_into('<I', image, pe + PE_TIMESTAMP_OFFSET, 0)
    struct.pack_into('<I', image, pe + PE_CHECKSUM_OFFSET, 0)
    return hashlib.sha256(image).hexdigest()


@dataclasses.dataclass(frozen=True)
class Case:
    name: str
    input_id: str
    options: tuple[str, ...]
    purpose: str
    truth: bool = False
    stored_8k: Path | None = None       # the lane's own pathsim output for this case
    stored_48k_sha256: str | None = None  # the lane's 48 kHz chain output for this case


class Gate:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.work: Path = args.work.resolve()
        self.inputs_dir = self.work / 'inputs'
        self.out_dir = self.work / 'out'
        self.env = dict(os.environ)
        self.env['PATH'] = str(args.toolchain_bin) + os.pathsep + self.env.get('PATH', '')
        self.log: list[dict] = []

    # -- process control ---------------------------------------------------
    def inside_work(self, path: Path) -> Path:
        resolved = path.resolve()
        if not resolved.is_relative_to(self.work):
            raise ValueError(f'refusing to write outside the work directory: {resolved}')
        return resolved

    def run(self, command: list[str]) -> float:
        started = time.monotonic()
        result = subprocess.run(command, capture_output=True, text=True, env=self.env, creationflags=BELOW_NORMAL)
        seconds = round(time.monotonic() - started, 3)
        self.log.append({'argv': command, 'returncode': result.returncode, 'wall_seconds': seconds,
                         'stdout': result.stdout.strip()[-2000:], 'stderr': result.stderr.strip()[-2000:]})
        if result.returncode != 0:
            raise RuntimeError(f'command failed (rc={result.returncode}): {" ".join(command)}\n{result.stdout}{result.stderr}')
        return seconds

    # -- inputs ------------------------------------------------------------
    def copy_input(self, source: Path, name: str) -> Path:
        target = self.inside_work(self.inputs_dir / name)
        if not target.is_file() or sha256(target) != sha256(source):
            shutil.copyfile(source, target)
        return target

    def resample_to_8k(self, clean48: Path, name: str) -> Path:
        """run_pathsim.py steps 1-2: resampler to 8 kHz at -12 dB, then the campaign's PCM16 re-write."""
        clean8 = self.inside_work(self.inputs_dir / f'{name}-clean-8k.wav')
        trial_in = self.inside_work(self.inputs_dir / f'{name}.wav')
        self.run([str(self.args.resampler), str(clean48), str(clean8), str(FS8_HZ), CAMPAIGN_GAIN_DB])
        rate8, signal8 = read_wav(clean8)
        if rate8 != FS8_HZ:
            raise RuntimeError(f'resampler output is {rate8} Hz, not {FS8_HZ} Hz')
        write_wav(trial_in, FS8_HZ, signal8)
        return trial_in

    def chain_to_48k(self, channel8: Path, name: str) -> Path:
        """run_pathsim.py steps 4-5: PCM16 re-write (keep_from = 0), then the resampler to 48 kHz."""
        out8 = self.inside_work(self.out_dir / f'{name}.out-8k.wav')
        out48 = self.inside_work(self.out_dir / f'{name}.out-48k.wav')
        _, channel = read_wav(channel8)
        write_wav(out8, FS8_HZ, channel[0:])
        self.run([str(self.args.resampler), str(out8), str(out48), '48000'])
        return out48


def lane_paths(lane: Path) -> dict[str, Path]:
    return {
        'golden_json': lane / 'hfchannel/pathsim/scratch/repro/gov2400L-s60001.json',
        'golden_8k': lane / 'hfchannel/pathsim/scratch/repro/gov2400L-s60001-8k',
        'p24l05_gen': lane / 'hfchannel/conformance/stimuli/P24L05.gen.json',
        'p24l05_sidecar': lane / 'hfchannel/conformance/stimuli/src/P24L05-ps-48k.json',
        'p24l05_clean48': lane / 'hfchannel/conformance/stimuli/src/C24L05-clean-48k.wav',
        'p24l60_gen': lane / 'hfchannel/conformance/stimuli/P24L60.gen.json',
    }


def pathsim_options_from_sidecar(sidecar: dict) -> tuple[str, ...]:
    """The pathsim options (argv minus program, input and output) of a run_pathsim.py sidecar."""
    commands = [c['argv'] for c in sidecar['commands'] if Path(c['argv'][0]).name.lower() == 'pathsim.exe']
    if len(commands) != 1:
        raise ValueError('sidecar does not hold exactly one pathsim command')
    options = tuple(commands[0][1:-2])
    if '--truth-out' in options:
        raise ValueError('sidecar pathsim command carries --truth-out')
    return options


def pathsim_options_from_gen(gen: dict) -> tuple[str, ...]:
    """run_pathsim.py's argv for a conformance stimulus' pathsim block (gen.json)."""
    p = gen['pathsim']
    return ('--spread', num(p['spread_hz']), '--delay2', num(p['delay2_ms']), '--spread2', num(p['spread2_hz']),
            '--snr', num(p['commanded_snr_db']), '--seed', str(int(p['seed'])))


def build_cases(gate: Gate, quick: bool) -> tuple[dict[str, Path], list[Case], dict]:
    paths = lane_paths(gate.args.lane_root)
    provenance: dict = {}
    inputs: dict[str, Path] = {}

    golden = json.loads(paths['golden_json'].read_text(encoding='utf-8'))
    golden_options = pathsim_options_from_sidecar(golden)
    inputs['gov2400L-8k'] = gate.copy_input(paths['golden_8k'] / 'trial-in.wav', 'gov2400L-8k.wav')
    provenance['golden'] = {'sidecar': str(paths['golden_json']), 'options': list(golden_options),
                            'stored_8k_pathsim_output': str(paths['golden_8k'] / 'trial-chan.wav'),
                            'stored_8k_rewrite_identical': sha256(paths['golden_8k'] / 'trial-chan.wav')
                            == sha256(paths['golden_8k'] / 'trial-out-8k.wav'),
                            'stored_48k_sha256': golden['output']['sha256']}

    cases = [
        Case('gov2400L-s60001', 'gov2400L-8k', golden_options, 'golden pair (lane run_pathsim.py sidecar argv)',
             stored_8k=paths['golden_8k'] / 'trial-chan.wav', stored_48k_sha256=golden['output']['sha256']),
    ]

    p24l05 = json.loads(paths['p24l05_gen'].read_text(encoding='utf-8'))
    p24l05_options = pathsim_options_from_gen(p24l05)
    sidecar_options = None
    if paths['p24l05_sidecar'].is_file():
        sidecar_options = pathsim_options_from_sidecar(json.loads(paths['p24l05_sidecar'].read_text(encoding='utf-8')))
    provenance['P24L05'] = {'gen_json': str(paths['p24l05_gen']), 'options': list(p24l05_options),
                            'options_equal_lane_sidecar': sidecar_options == p24l05_options if sidecar_options else None,
                            'clean_48k': str(paths['p24l05_clean48']), 'stored_48k_sha256': p24l05['source_sha256']}
    inputs['P24L05-8k'] = gate.resample_to_8k(paths['p24l05_clean48'], 'P24L05-8k')
    cases.append(Case('P24L05', 'P24L05-8k', p24l05_options, 'conformance stimulus P24L05 (gen.json pathsim block)',
                      stored_48k_sha256=p24l05['source_sha256']))

    if not quick:
        p24l60 = json.loads(paths['p24l60_gen'].read_text(encoding='utf-8'))
        p24l60_options = pathsim_options_from_gen(p24l60)
        run_dir = Path(p24l60['source']).parent
        provenance['P24L60'] = {'gen_json': str(paths['p24l60_gen']), 'options': list(p24l60_options),
                                'campaign_run': str(run_dir), 'stored_8k_pathsim_output': str(run_dir / 'trial-chan.wav')}
        inputs['P24L60-8k'] = gate.copy_input(run_dir / 'trial-in.wav', 'P24L60-8k.wav')
        cases.append(Case('P24L60', 'P24L60-8k', p24l60_options, 'conformance stimulus P24L60, 1 h (gen.json pathsim block)',
                          stored_8k=run_dir / 'trial-chan.wav'))

    two_path = ('--spread', '1.0', '--delay2', '2.0', '--spread2', '1.0')
    cases += [
        Case('one-path', 'gov2400L-8k', ('--spread', '1.0', '--snr', '20.0', '--seed', '1'), 'single fading path'),
        Case('one-path-offset', 'gov2400L-8k', ('--midlat-moderate', '--snr', '10.0', '--seed', '7'),
             'preset: one path, 1 Hz spread, 0.5 Hz offset'),
        Case('two-path-preset', 'gov2400L-8k', ('--ccir-poor', '--snr', '3.0', '--seed', '2'), 'preset: two paths, 0.5 Hz, 2 ms'),
        Case('three-path', 'gov2400L-8k', ('--spread', '0.5', '--delay2', '1.0', '--spread2', '0.5', '--delay3', '3.0',
                                           '--spread3', '2.0', '--offset3', '1.0', '--snr', '15.0', '--seed', '99'),
             'three paths, offset on the third'),
        Case('direct', 'gov2400L-8k', ('--direct',), 'direct path, no AWGN'),
        Case('awgn-direct', 'gov2400L-8k', ('--direct', '--snr', '30.0', '--seed', '3'), 'AWGN only'),
        Case('awgn10-preset', 'gov2400L-8k', ('--awgn10', '--seed', '4'), 'preset: AWGN only, 10 dB'),
        Case('high-snr', 'gov2400L-8k', two_path + ('--snr', '60.0', '--seed', '5'), 'two paths, high SNR'),
        Case('low-snr', 'gov2400L-8k', two_path + ('--snr', '-5.0', '--seed', '6'), 'two paths, SNR < 0 dB (other gain branch)'),
        Case('no-awgn', 'gov2400L-8k', two_path + ('--seed', '8'), 'two paths, no AWGN'),
        Case('default-seed', 'gov2400L-8k', two_path + ('--snr', '17.6'), 'two paths, no --seed (seed 1)'),
        Case('flutter-max-seed', 'gov2400L-8k', ('--highlat-disturbed', '--snr', '10.0', '--seed', '4294967295'),
             'preset: 30 Hz spread, 7 ms; maximum seed'),
        Case('truth-export', 'gov2400L-8k', golden_options, 'golden argv plus --truth-out (truth file compared)', truth=True),
        Case('long-awgn-0dB', 'P24L05-8k', ('--direct', '--snr', '0.0', '--seed', '11'), '308 s, AWGN only at the 0 dB boundary'),
        Case('long-lowlat-disturbed', 'P24L05-8k', ('--lowlat-disturbed', '--snr', '12.0', '--seed', '31'),
             '308 s, preset: 10 Hz spread, 6 ms'),
    ]
    return inputs, cases, provenance


def run_case(gate: Gate, case: Case, inputs: dict[str, Path]) -> dict:
    source = inputs[case.input_id]
    record: dict = {'case': case.name, 'purpose': case.purpose, 'input': case.input_id, 'input_sha256': sha256(source),
                    'options': list(case.options)}
    outputs = {}
    for label, program in (('ref', gate.args.ref), ('shipped', gate.args.shipped)):
        output = gate.inside_work(gate.out_dir / f'{case.name}.{label}.wav')
        command = [str(program), *case.options]
        truth = None
        if case.truth:
            truth = gate.inside_work(gate.out_dir / f'{case.name}.{label}.truth')
            command += ['--truth-out', str(truth)]
        command += [str(source), str(output)]
        seconds = gate.run(command)
        outputs[label] = output
        record[f'{label}_sha256'] = sha256(output)
        record[f'{label}_seconds'] = seconds
        if truth:
            record[f'{label}_truth_sha256'] = sha256(truth)
    record['identical'] = record['ref_sha256'] == record['shipped_sha256']
    if case.truth:
        record['truth_identical'] = record['ref_truth_sha256'] == record['shipped_truth_sha256']
    if case.stored_8k:
        record['stored_8k_sha256'] = sha256(case.stored_8k)
        record['reproduces_stored_8k'] = record['ref_sha256'] == record['stored_8k_sha256']
    if case.stored_48k_sha256:
        out48 = gate.chain_to_48k(outputs['ref'], f'{case.name}.ref')
        record['ref_chain_48k_sha256'] = sha256(out48)
        record['stored_48k_sha256'] = case.stored_48k_sha256
        record['reproduces_stored_48k'] = record['ref_chain_48k_sha256'] == case.stored_48k_sha256
    checks = [record['identical'], record.get('truth_identical', True), record.get('reproduces_stored_8k', True),
              record.get('reproduces_stored_48k', True)]
    record['pass'] = all(checks)
    return record


def markdown(records: list[dict]) -> str:
    lines = ['| Case | pathsim options | Input | sha256 pathsim_ref | sha256 shipped 7746121 | Identical | Lane-stored output |',
             '|---|---|---|---|---|---|---|']
    for r in records:
        stored = []
        if 'reproduces_stored_8k' in r:
            stored.append('8 kHz ' + ('reproduced' if r['reproduces_stored_8k'] else 'DIFFERS'))
        if 'reproduces_stored_48k' in r:
            stored.append('48 kHz chain ' + ('reproduced' if r['reproduces_stored_48k'] else 'DIFFERS'))
        if 'truth_identical' in r:
            stored.append('truth ' + ('identical' if r['truth_identical'] else 'DIFFERS'))
        lines.append(f"| {r['case']} | `{' '.join(r['options'])}` | {r['input']} | `{r['ref_sha256']}` | `{r['shipped_sha256']}` "
                     f"| {'yes' if r['identical'] else 'NO'} | {'; '.join(stored) or '-'} |")
    return '\n'.join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', type=Path, required=True, help='channel-sim build directory (holds pathsim_ref.exe)')
    parser.add_argument('--work', type=Path, help='work directory (default <build>/work)')
    parser.add_argument('--lane-root', type=Path, required=True, help='external reference-data root (read only; not distributed with this project)')
    parser.add_argument('--snapshot', default=DEFAULT_SNAPSHOT, help='lane snapshot whose tools/ holds pathsim.exe and the resampler')
    parser.add_argument('--toolchain-bin', type=Path, help='runtime DLL directory put first on PATH (default: the recipe compiler dir)')
    parser.add_argument('--quick', action='store_true', help='skip the 1-hour P24L60 case')
    args = parser.parse_args()

    build = args.build.resolve()
    args.ref = build / 'pathsim_ref.exe'
    recipe = json.loads((build / 'pathsim_ref.recipe.json').read_text(encoding='utf-8'))
    tools = args.lane_root / 'snapshots' / args.snapshot / 'tools'
    args.shipped = tools / 'pathsim.exe'
    args.resampler = tools / 'm110_wav_resample.exe'
    args.work = args.work or build / 'work'
    args.toolchain_bin = args.toolchain_bin or Path(recipe['compiler']).parent
    for required in (args.ref, args.shipped, args.resampler):
        if not required.is_file():
            sys.exit(f'missing: {required}')
    shipped_sha256 = sha256(args.shipped)
    if shipped_sha256 != SHIPPED_PATHSIM_SHA256:
        sys.exit(f'{args.shipped} has sha256 {shipped_sha256}, not the lane reference {SHIPPED_PATHSIM_SHA256}')

    gate = Gate(args)
    gate.inputs_dir.mkdir(parents=True, exist_ok=True)
    gate.out_dir.mkdir(parents=True, exist_ok=True)
    started = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat()
    inputs, cases, provenance = build_cases(gate, args.quick)
    records = [run_case(gate, case, inputs) for case in cases]

    runtime = {dll: {'toolchain': sha256(args.toolchain_bin / dll) if (args.toolchain_bin / dll).is_file() else None,
                     'beside_ref': sha256(build / dll) if (build / dll).is_file() else None}
               for dll in ('libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')}
    binaries = {
        'pathsim_ref': {'path': str(args.ref), 'sha256': sha256(args.ref), 'pe_identity_sha256': pe_identity_digest(args.ref)},
        'shipped': {'path': str(args.shipped), 'sha256': shipped_sha256, 'pe_identity_sha256': pe_identity_digest(args.shipped)},
        'resampler': {'path': str(args.resampler), 'sha256': sha256(args.resampler)},
    }
    binaries['identical_except_pe_timestamp_and_checksum'] = (
        binaries['pathsim_ref']['pe_identity_sha256'] == binaries['shipped']['pe_identity_sha256'])
    passed = all(r['pass'] for r in records)
    report = {
        'gate': 'channel-sim gate 0: pathsim_ref vs shipped pathsim.exe 7746121',
        'started_utc': started, 'tool': str(Path(__file__).resolve()), 'tool_sha256': sha256(Path(__file__).resolve()),
        'recipe': recipe, 'binaries': binaries, 'runtime_dlls': runtime, 'toolchain_bin_prepended_to_path': str(args.toolchain_bin),
        'inputs': {name: {'path': str(path), 'sha256': sha256(path)} for name, path in inputs.items()},
        'provenance': provenance, 'cases': records, 'commands': gate.log, 'pass': passed,
    }
    (gate.work / 'gate0-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    table = markdown(records)
    (gate.work / 'gate0-report.md').write_text(table + '\n', encoding='utf-8')
    print(table)
    print(f"binary identical to the shipped one except the PE timestamp/checksum: "
          f"{binaries['identical_except_pe_timestamp_and_checksum']}")
    print(f"gate 0: {'PASS' if passed else 'FAIL'} ({sum(r['pass'] for r in records)}/{len(records)} cases); "
          f"report {gate.work / 'gate0-report.json'}")
    return 0 if passed else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        print(f'gate 0 failed to run: {error}', file=sys.stderr)
        sys.exit(2)
