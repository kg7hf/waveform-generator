#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Gates 1-3 (Phase 5a): the chain driver pathsim_chain against the HF lane's reference.

Gate 1  pathsim_chain (pathsim's library sources + the draw-order patch + the RNG shim +
        the owned driver) is byte-identical to pathsim_ref and to the lane's shipped
        pathsim.exe (7746121) on every gate-0 case, truth export included, and reproduces
        the lane-stored 8 kHz outputs; more cases feed a stereo input and path delays of
        60 ms and 305.875 ms (the delay ring's last lag).
        Part b (check_draw_order.py): at every pathsim draw site the first draw of a pair
        reaches the imaginary part, as in the lane binary, with the host compiler and with
        arm-none-eabi-g++ for the RT1170 (GIMPLE dumps).
Gate 2  the 48 kHz chain (owned resamplers + driver) against the lane's run_pathsim.py
        chain outputs:
          (a) the golden pair gov2400L-s60001 (48 kHz output);
          (b) the conformance stimuli P24L05 / P24L60 (gen.json source hashes), from
              their clean 48 kHz inputs;
          (c) HF lane calib/appE runs (110D Appendix E durations) from their sidecars;
          (d) calib probe runs (a1-a4): the lane analyzers (analyze_snr/doppler/delay)
              run on our output and on the lane's, compared with the calib targets.
        Each case is bit-exact or quantified (max |difference| in LSB, difference
        energy re signal) and, for (d), statistically compared.
Gate 3  seeded reproducibility: the same seed twice gives identical bytes, another seed
        gives other bytes (8 kHz and 48 kHz cases).

Read-only on the external reference tree selected by --lane-root; its own
programs and analyzers run with outputs under <build>/work only, at below-normal
priority, with no Python bytecode written. Writes <build>/gates.json (every case,
command, sha256 and verdict) and <build>/gates.md. Each gate run is PASS, FAIL or SKIP
(gate 1b without arm-none-eabi-g++). Exit status 0: every gate run passed, or was skipped
with --allow-skip; 1: a gate failed or was skipped; 2: the gates could not run (a usage
error, such as --gates naming no gate 1, 2 or 3).

  python channel-sim/gates/run_gates.py --build build/main/pathsim-host [--quick | --full] [--gates 1,3] [--allow-skip]

--quick skips the 1-hour and Appendix E cases; --full adds the remaining Appendix E runs.
Guide: docs/modules/pathsim.md.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True  # no __pycache__ beside our scripts or the lane's analyzers

import argparse  # noqa: E402
import dataclasses  # noqa: E402
import datetime  # noqa: E402
import json  # noqa: E402
import os  # noqa: E402
from pathlib import Path  # noqa: E402
import struct  # noqa: E402
import wave  # noqa: E402

import numpy as np  # noqa: E402

import check_draw_order  # noqa: E402
import gate0_reference_parity as g0  # noqa: E402

SNR_TOLERANCE_DB = 0.1          # lane acceptance: delivered SNR within +/-0.1 dB of the reference
DOPPLER_TOLERANCE_REL = 0.004   # calib: Doppler 2 sigma within 0.4 %
DELAY_TOLERANCE_SAMPLES = 0.01  # calib delays 96.0 / 240.06 samples, reported to 0.01
LEVEL_TOLERANCE_DB = 0.01
LABEL_OFFSET_DB = 0.42          # calib: delivered = label + 0.42 dB (N0 x 3000 Hz, 300-3300 Hz)
PIN_DBFS = 20.0 * np.log10(0.122)

APPE_DEFAULT = ('e73_single_10Hz_2h_snr20_s1', 'e73_dual_2ms_1Hz_2h_snr20_s1', 'e74_single_1Hz_3h_snr40_s1')
APPE_FULL = ('e73_single_1Hz_2h_snr20_s1', 'e73_dual_2ms_10Hz_2h_snr20_s1')
GATES = ('1', '2', '3')  # gate 0 is its own script, gate0_reference_parity.py


@dataclasses.dataclass(frozen=True)
class CalibCase:
    name: str
    folder: str
    analysis: str  # snr | doppler | delay
    purpose: str


CALIB_CASES = (
    CalibCase('a1_gapnoise_snr18_direct_s1', 'a1', 'snr', 'AWGN only, label 18 dB, 120 s gapnoise (calib A1)'),
    CalibCase('a2_gapnoise600_2ms1hz_snr18_s1', 'a2', 'snr', 'two paths 2 ms / 1 Hz, label 18 dB, 600 s gapnoise (calib A2)'),
    CalibCase('a3_single_5Hz_600s_snr40_s1', 'a3', 'doppler', 'one path, 5 Hz spread, 600 s tone (calib A3)'),
    CalibCase('a4_delay_static2p_2ms_snr40_s1', 'a4', 'delay', 'two static paths 2 ms (calib A4: 96.05 samples)'),
    CalibCase('a4_delay_static2p_5ms_snr40_s1', 'a4', 'delay', 'two static paths 5 ms (calib A4: 240.06 samples)'),
    CalibCase('a4_delay_fading_2ms1hz_300s_snr40_s1', 'a4', 'delay', 'two fading paths 2 ms / 1 Hz, 300 s (calib A4: 96.006)'),
)


# ---------------------------------------------------------------------------------------- WAV data

def wav_data(path: Path) -> tuple[int, int, int, int]:
    """(data offset, frames, channels, bits) of a PCM WAV, by walking its chunks."""
    with open(path, 'rb') as handle:
        head = handle.read(12)
        if head[:4] != b'RIFF' or head[8:12] != b'WAVE':
            raise ValueError(f'{path}: not RIFF/WAVE')
        channels = bits = None
        while True:
            chunk = handle.read(8)
            if len(chunk) < 8:
                raise ValueError(f'{path}: no data chunk')
            cid, size = chunk[:4], struct.unpack('<I', chunk[4:])[0]
            if cid == b'fmt ':
                body = handle.read(size + (size & 1))
                _, channels, _, _, _, bits = struct.unpack_from('<HHIIHH', body)
            elif cid == b'data':
                return handle.tell(), size // (channels * bits // 8), channels, bits
            else:
                handle.seek(size + (size & 1), 1)


def pcm16_channel0(path: Path) -> np.ndarray:
    offset, frames, channels, bits = wav_data(path)
    if bits != 16:
        raise ValueError(f'{path}: not PCM16')
    return np.memmap(path, dtype='<i2', mode='r', offset=offset, shape=(frames * channels,))[::channels]


def compare_pcm16(ours: Path, reference: Path) -> dict:
    """Quantifies a difference: max |a - b| in LSB and the difference energy re the reference."""
    a, b = pcm16_channel0(ours), pcm16_channel0(reference)
    result = {'frames_ours': int(len(a)), 'frames_reference': int(len(b))}
    n = min(len(a), len(b))
    peak, diff_energy, ref_energy, differing = 0, 0.0, 0.0, 0
    step = 1 << 22
    for start in range(0, n, step):
        x = np.asarray(a[start:start + step], dtype=np.int64)
        y = np.asarray(b[start:start + step], dtype=np.int64)
        d = x - y
        peak = max(peak, int(np.max(np.abs(d))) if len(d) else 0)
        differing += int(np.count_nonzero(d))
        diff_energy += float(np.sum(d.astype(np.float64) ** 2))
        ref_energy += float(np.sum(y.astype(np.float64) ** 2))
    result.update({'max_abs_difference_lsb': peak, 'differing_samples': differing,
                   'difference_energy_db_re_reference': (float(10.0 * np.log10(diff_energy / ref_energy))
                                                         if diff_energy > 0 and ref_energy > 0 else None)})
    return result


def rms_dbfs(path: Path) -> float:
    x = pcm16_channel0(path)
    total, count = 0.0, 0
    for start in range(0, len(x), 1 << 22):
        chunk = np.asarray(x[start:start + (1 << 22)], dtype=np.float64) / 32768.0
        total += float(np.sum(chunk * chunk))
        count += len(chunk)
    return float(10.0 * np.log10(total / count)) if count and total > 0 else float('-inf')


# ---------------------------------------------------------------------------------------- helpers

def parse_gates(text: str) -> frozenset[int]:
    """--gates: a comma-separated, non-empty choice of gates 1, 2 and 3; anything else is a usage error."""
    names = [name.strip() for name in text.split(',')]
    if not all(name in GATES for name in names):
        raise argparse.ArgumentTypeError(f"{text!r}: name gates 1, 2 and 3, comma-separated "
                                         "(gate 0 runs as gate0_reference_parity.py)")
    return frozenset(int(name) for name in names)


def settle(gates: dict[str, dict], allow_skip: bool) -> int:
    """Each gate's pass and verdict (PASS, FAIL or SKIP), and the run's exit status (check_draw_order.exit_status).

    A gate that ran no case did not pass: it is FAIL (0/0), never PASS.
    """
    for verdict in gates.values():
        verdict['pass'] = not verdict.get('skipped', False) and verdict['cases'] > 0 and verdict['passed'] == verdict['cases']
        verdict['verdict'] = check_draw_order.verdict(verdict['pass'], verdict.get('skipped', False))
    return check_draw_order.exit_status([verdict['verdict'] for verdict in gates.values()], allow_skip)


def verdict_text(verdict: dict) -> str:
    if verdict['verdict'] == check_draw_order.SKIP:
        return f"{check_draw_order.SKIP} (no arm-none-eabi-g++; not a pass without --allow-skip)"
    return f"{verdict['verdict']} ({verdict['passed']}/{verdict['cases']} cases)"


def pathsim_command(sidecar: dict) -> list[str]:
    commands = [c['argv'] for c in sidecar['commands'] if Path(c['argv'][0]).name.lower() == 'pathsim.exe']
    if len(commands) != 1:
        raise ValueError('sidecar does not hold exactly one pathsim command')
    return commands[0]


def split_truth(options: list[str]) -> tuple[list[str], bool]:
    """pathsim options without '--truth-out <file>', and whether the lane wrote a truth file."""
    if '--truth-out' not in options:
        return options, False
    at = options.index('--truth-out')
    return options[:at] + options[at + 2:], True


def option_value(options: list[str] | tuple[str, ...], name: str) -> float | None:
    return float(options[options.index(name) + 1]) if name in options else None


class Runner:
    """gate0's process control (below-normal priority, toolchain PATH, command log) plus our outputs."""

    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.gate = g0.Gate(args)
        self.gate.env['PYTHONDONTWRITEBYTECODE'] = '1'
        self.work: Path = self.gate.work
        for sub in ('inputs', 'out', 'gate1', 'gate2', 'gate3', 'analysis'):
            (self.work / sub).mkdir(parents=True, exist_ok=True)

    def path(self, sub: str, name: str) -> Path:
        return self.gate.inside_work(self.work / sub / name)

    def run(self, command: list) -> float:
        return self.gate.run([str(part) for part in command])

    def chain(self, options, source: Path, output: Path, truth: Path | None = None) -> tuple[list[str], float]:
        command = [str(self.args.chain), *options]
        if truth:
            command += ['--truth-out', str(truth)]
        command += [str(source), str(output)]
        return command, self.run(command)

    def discard(self, path: Path) -> None:
        if not self.args.keep_outputs and path.is_file() and path.stat().st_size > (64 << 20):
            path.unlink()


# ---------------------------------------------------------------------------------------- gate 1

def stereo_case(runner: Runner, inputs: dict[str, Path]) -> g0.Case:
    """A stereo 8 kHz input (left: the golden input, right: it reversed): pathsim processes
    channel 0 and copies it to every output channel; the chain CLI must do the same."""
    _, mono = g0.read_wav(inputs['gov2400L-8k'])
    stereo = np.column_stack((mono, mono[::-1])).astype('<i2')
    path = runner.path('inputs', 'gov2400L-8k-stereo.wav')
    with wave.open(str(path), 'wb') as handle:
        handle.setnchannels(2)
        handle.setsampwidth(2)
        handle.setframerate(g0.FS8_HZ)
        handle.writeframes(stereo.tobytes())
    inputs['gov2400L-8k-stereo'] = path
    return g0.Case('stereo-input', 'gov2400L-8k-stereo', ('--spread', '1.0', '--delay2', '2.0', '--spread2', '1.0', '--snr', '17.6',
                                                         '--seed', '60001'),
                   'stereo PCM16 input: channel 0 processed, copied to both output channels (chain CLI only, not a gate-0 case)')


def delay_cases() -> list[g0.Case]:
    """Path delays beyond upstream's 50 ms comment: pathsim's 2448-entry delay ring serves every lag
    0 .. 2447 samples, and pathsim_chain accepts what pathsim.exe runs (ChannelSpec, delay < 306 ms)."""
    return [
        g0.Case('delay-60ms', 'gov2400L-8k', ('--spread', '1.0', '--delay2', '60', '--spread2', '1.0', '--snr', '10', '--seed', '1'),
                'two paths, 60 ms apart (beyond upstream MAXDELAY = 50 ms)'),
        g0.Case('delay-305.875ms', 'gov2400L-8k', ('--spread', '1.0', '--delay2', '305.875', '--spread2', '1.0', '--snr', '20',
                                                   '--seed', '12'),
                'two paths, 305.875 ms apart: lag 2447, the delay ring\'s last entry'),
    ]


def gate1_draw_order(runner: Runner) -> dict:
    """check_draw_order.py through the runner (below-normal priority, command log)."""
    build = runner.args.chain.parent
    arm_gxx = check_draw_order.find_arm_gxx(runner.args.arm_gxx)
    record = check_draw_order.check(build, runner.work, Path(runner.args.chain_recipe['compiler']), arm_gxx,
                                    lambda command, _cwd: runner.gate.run(command))
    for result in record['results']:
        order = ', '.join(site['first_draw_to'] for site in result['sites']) or 'no draw site'
        print(f"gate 1b {result['compiler']} {result['tree']} {result['file']}: first draw -> {order}", flush=True)
    print(f"gate 1b draw-order: {record['verdict']}{' (no arm-none-eabi-g++)' if record['skipped'] else ''}", flush=True)
    return record


def gate1(runner: Runner, inputs: dict[str, Path], cases: list) -> list[dict]:
    records = []
    programs = (('chain', runner.args.chain), ('ref', runner.args.ref), ('shipped', runner.args.shipped))
    for case in cases:
        source = inputs[case.input_id]
        record = {'gate': 1, 'case': case.name, 'purpose': case.purpose, 'input': case.input_id, 'input_path': str(source),
                  'input_sha256': g0.sha256(source), 'options': list(case.options), 'runs': {}}
        for label, program in programs:
            output = runner.path('gate1', f'{case.name}.{label}.wav')
            command = [str(program), *case.options]
            truth = runner.path('gate1', f'{case.name}.{label}.truth') if case.truth else None
            if truth:
                command += ['--truth-out', str(truth)]
            command += [str(source), str(output)]
            seconds = runner.run(command)
            run = {'argv': command, 'sha256': g0.sha256(output), 'seconds': seconds}
            if truth:
                run['truth_sha256'] = g0.sha256(truth)
            record['runs'][label] = run
        runs = record['runs']
        record['sha256'] = runs['chain']['sha256']
        record['identical'] = len({r['sha256'] for r in runs.values()}) == 1
        checks = [record['identical']]
        if case.truth:
            record['truth_identical'] = len({r['truth_sha256'] for r in runs.values()}) == 1
            checks.append(record['truth_identical'])
        if case.stored_8k:
            record['stored_8k'] = str(case.stored_8k)
            record['stored_8k_sha256'] = g0.sha256(case.stored_8k)
            record['reproduces_stored_8k'] = record['sha256'] == record['stored_8k_sha256']
            checks.append(record['reproduces_stored_8k'])
        if not record['identical']:
            record['difference_vs_ref'] = compare_pcm16(runner.path('gate1', f'{case.name}.chain.wav'),
                                                        runner.path('gate1', f'{case.name}.ref.wav'))
        record['pass'] = all(checks)
        records.append(record)
        print(f"gate 1 {case.name}: {'PASS' if record['pass'] else 'FAIL'} {record['sha256'][:16]}", flush=True)
    return records


# ---------------------------------------------------------------------------------------- gate 2

def chain_case(runner: Runner, name: str, part: str, purpose: str, source: Path, options: list[str],
               expected_sha256: str, stored: Path | None, truth_expected: str | None = None) -> dict:
    output = runner.path('gate2', f'{name}.chain-48k.wav')
    truth = runner.path('gate2', f'{name}.truth') if truth_expected else None
    command, seconds = runner.chain(options, source, output, truth)
    record = {'gate': 2, 'part': part, 'case': name, 'purpose': purpose, 'input_path': str(source),
              'input_sha256': g0.sha256(source), 'options': options, 'argv': command, 'seconds': seconds,
              'sha256': g0.sha256(output), 'reference_sha256': expected_sha256}
    if stored is not None and stored.is_file():
        record['reference_path'] = str(stored)
        record['reference_file_sha256'] = g0.sha256(stored)
    record['bit_exact'] = record['sha256'] == expected_sha256
    if truth:
        record['truth_sha256'] = g0.sha256(truth)
        record['reference_truth_sha256'] = truth_expected
        record['truth_identical'] = record['truth_sha256'] == truth_expected
        runner.discard(truth)
    if not record['bit_exact'] and stored is not None and stored.is_file():
        record['difference'] = compare_pcm16(output, stored)
    record['output_rms_dbfs'] = rms_dbfs(output)
    record['output_path'] = str(output)
    record['pass'] = record['bit_exact'] and record.get('truth_identical', True)
    print(f"gate 2{part} {name}: {'bit-exact' if record['bit_exact'] else 'DIFFERS'} {record['sha256'][:16]} ({seconds:.1f} s)",
          flush=True)
    return record


def gate2_reference_cases(runner: Runner, quick: bool, full: bool) -> list[dict]:
    lane = runner.args.lane_root
    paths = g0.lane_paths(lane)
    records = []

    golden = json.loads(paths['golden_json'].read_text(encoding='utf-8'))
    records.append(chain_case(runner, 'gov2400L-s60001', 'a', 'golden pair, 48 kHz (run_pathsim.py sidecar)',
                              Path(golden['input']['path']), list(g0.pathsim_options_from_sidecar(golden)),
                              golden['output']['sha256'], Path(golden['output']['path'])))

    p24l05 = json.loads(paths['p24l05_gen'].read_text(encoding='utf-8'))
    records.append(chain_case(runner, 'P24L05', 'b', 'conformance stimulus P24L05 from C24L05-clean-48k (gen.json)',
                              paths['p24l05_clean48'], list(g0.pathsim_options_from_gen(p24l05)),
                              p24l05['source_sha256'], Path(p24l05['source'])))

    if not quick:
        p24l60 = json.loads(paths['p24l60_gen'].read_text(encoding='utf-8'))
        run_dir = Path(p24l60['source']).parent
        record = chain_case(runner, 'P24L60', 'b', 'conformance stimulus P24L60, 1 h, from the campaign clean 48 kHz input (gen.json)',
                            run_dir / '2400l-clean-48k.wav', list(g0.pathsim_options_from_gen(p24l60)),
                            p24l60['source_sha256'], Path(p24l60['source']))
        runner.discard(Path(record['output_path']))
        records.append(record)

        appe = APPE_DEFAULT + (APPE_FULL if full else ())
        for name in appe:
            sidecar_path = lane / 'hfchannel/calib/outputs/pathsim/appE' / f'{name}.json'
            sidecar = json.loads(sidecar_path.read_text(encoding='utf-8'))
            options, _ = split_truth(pathsim_command(sidecar)[1:-2])
            record = chain_case(runner, name, 'c', f'calib/appE run ({sidecar["input"]["seconds"]:.0f} s), {sidecar_path.name}',
                                Path(sidecar['input']['path']), options, sidecar['output']['sha256'], Path(sidecar['output']['path']))
            record['reference_rms_dbfs'] = sidecar['output']['rms_dbfs']
            runner.discard(Path(record['output_path']))
            records.append(record)
    return records


def analyzer_metrics(kind: str, result: dict) -> dict:
    if kind == 'snr':
        return {'snr_db': result['snr_db'], 'snr_standard_error_db': result.get('snr_standard_error_db')}
    if kind == 'doppler':
        return {'spread_2sigma_hz': result['spread_2sigma_hz'], 'spread_2sigma_full_fit_hz': result.get('spread_2sigma_full_fit_hz'),
                'spurs': result.get('spurs'), 'table_e1': result.get('table_e1')}
    return {'n_taps': result['n_taps'], 'delays_samples': [tap['delay_samples'] for tap in result['taps']],
            'powers_rel_total': [tap['power_rel_total'] for tap in result['taps']], 'pslr_db': result.get('pslr_db')}


def compare_metrics(kind: str, ours: dict, reference: dict, options: list[str]) -> dict:
    if kind == 'snr':
        label = option_value(options, '--snr')
        delta = ours['snr_db'] - reference['snr_db']
        return {'delta_db': delta, 'tolerance_db': SNR_TOLERANCE_DB, 'within': abs(delta) <= SNR_TOLERANCE_DB,
                'label_db': label, 'delivered_minus_label_db': ours['snr_db'] - label if label is not None else None,
                'calib_label_offset_db': LABEL_OFFSET_DB}
    if kind == 'doppler':
        label = option_value(options, '--spread')
        relative = ours['spread_2sigma_hz'] / reference['spread_2sigma_hz'] - 1.0
        return {'delta_relative': relative, 'tolerance_relative': DOPPLER_TOLERANCE_REL,
                'within': abs(relative) <= DOPPLER_TOLERANCE_REL, 'label_spread_hz': label,
                'relative_to_label': ours['spread_2sigma_hz'] / label - 1.0 if label else None,
                'spurs_ours': ours['spurs'], 'spurs_reference': reference['spurs']}
    same_count = ours['n_taps'] == reference['n_taps']
    deltas = [a - b for a, b in zip(ours['delays_samples'], reference['delays_samples'])] if same_count else []
    return {'delta_samples': deltas, 'tolerance_samples': DELAY_TOLERANCE_SAMPLES,
            'within': same_count and all(abs(d) <= DELAY_TOLERANCE_SAMPLES for d in deltas)}


def run_analyzer(runner: Runner, case: CalibCase, stored_argv: list[str], wav: Path, label: str) -> dict:
    script = runner.args.lane_root / 'hfchannel/probes' / f'analyze_{case.analysis}.py'
    out = runner.path('analysis', f'{case.name}.{label}.{case.analysis}.json')
    argv = list(stored_argv)
    argv[argv.index('--in') + 1] = str(wav)
    argv[argv.index('--out') + 1] = str(out)
    command = [sys.executable, '-B', str(script), *argv]
    seconds = runner.run(command)
    return {'argv': command, 'seconds': seconds, 'result': json.loads(out.read_text(encoding='utf-8'))}


def gate2_analysis_cases(runner: Runner) -> list[dict]:
    records = []
    root = runner.args.lane_root / 'hfchannel/calib/outputs/pathsim'
    for case in CALIB_CASES:
        sidecar = json.loads((root / case.folder / f'{case.name}.json').read_text(encoding='utf-8'))
        stored_analysis = json.loads((root / case.folder / f'{case.name}_{case.analysis}.json').read_text(encoding='utf-8'))
        options, lane_truth = split_truth(pathsim_command(sidecar)[1:-2])
        reference_wav = Path(sidecar['output']['path'])
        record = chain_case(runner, case.name, 'd', case.purpose, Path(sidecar['input']['path']), options,
                            sidecar['output']['sha256'], reference_wav,
                            sidecar.get('truth_out_sha256') if lane_truth else None)
        ours = run_analyzer(runner, case, stored_analysis['argv'], Path(record['output_path']), 'chain')
        reference = run_analyzer(runner, case, stored_analysis['argv'], reference_wav, 'lane')
        ours_metrics = analyzer_metrics(case.analysis, ours['result'])
        reference_metrics = analyzer_metrics(case.analysis, reference['result'])
        stored_metrics = analyzer_metrics(case.analysis, stored_analysis)
        comparison = compare_metrics(case.analysis, ours_metrics, reference_metrics, options)
        record['analysis'] = {
            'analyzer': case.analysis, 'analyzer_version': ours['result'].get('version'),
            'ours': ours_metrics, 'reference_rerun': reference_metrics, 'reference_stored': stored_metrics,
            'rerun_equals_stored': reference_metrics == stored_metrics,
            'comparison': comparison, 'argv_ours': ours['argv'], 'argv_reference': reference['argv'],
        }
        record['level'] = {'ours_rms_dbfs': record['output_rms_dbfs'], 'reference_rms_dbfs': sidecar['output']['rms_dbfs'],
                           'within': bool(abs(record['output_rms_dbfs'] - sidecar['output']['rms_dbfs']) <= LEVEL_TOLERANCE_DB),
                           'pin_dbfs': float(PIN_DBFS)}
        record['statistically_equivalent'] = comparison['within'] and record['level']['within']
        record['pass'] = (record['bit_exact'] and record.get('truth_identical', True)) or record['statistically_equivalent']
        print(f"gate 2d {case.name}: {case.analysis} {json.dumps(comparison, default=str)[:160]}", flush=True)
        records.append(record)
    return records


# ---------------------------------------------------------------------------------------- gate 3

def gate3(runner: Runner, inputs: dict[str, Path]) -> list[dict]:
    golden = json.loads(g0.lane_paths(runner.args.lane_root)['golden_json'].read_text(encoding='utf-8'))
    cases = (
        ('8k-golden-two-path', inputs['gov2400L-8k'], ['--spread', '1.0', '--delay2', '2.0', '--spread2', '1.0', '--snr', '17.6'],
         (60001, 60001, 60002)),
        ('8k-awgn-308s', inputs['P24L05-8k'], ['--direct', '--snr', '0.0'], (11, 11, 12)),
        ('48k-lowlat-disturbed', Path(golden['input']['path']), ['--lowlat-disturbed', '--snr', '12.0'], (31, 31, 32)),
    )
    records = []
    for name, source, options, seeds in cases:
        runs = []
        for index, seed in enumerate(seeds):
            output = runner.path('gate3', f'{name}.run{index}-seed{seed}.wav')
            command, seconds = runner.chain(options + ['--seed', str(seed)], source, output)
            runs.append({'seed': seed, 'argv': command, 'seconds': seconds, 'sha256': g0.sha256(output), 'path': output})
        same = runs[0]['sha256'] == runs[1]['sha256']
        other = runs[0]['sha256'] != runs[2]['sha256']
        difference = compare_pcm16(runs[2]['path'], runs[0]['path'])
        record = {'gate': 3, 'case': name, 'input_path': str(source), 'options': options,
                  'runs': [{k: v for k, v in r.items() if k != 'path'} for r in runs],
                  'same_seed_identical': same, 'other_seed_differs': other and difference['differing_samples'] > 0,
                  'other_seed_difference': difference}
        record['pass'] = record['same_seed_identical'] and record['other_seed_differs']
        print(f"gate 3 {name}: {'PASS' if record['pass'] else 'FAIL'}", flush=True)
        records.append(record)
    return records


# ---------------------------------------------------------------------------------------- report

def markdown(report: dict) -> str:
    lines = [f"# channel-sim gates ({report['started_utc']})", '']
    for gate in ('gate1', 'gate1b', 'gate2', 'gate3'):
        if gate in report['gates']:
            lines.append(f"- {gate}: {verdict_text(report['gates'][gate])}")
    lines += ['', '| Gate | Case | sha256 (ours) | Verdict |', '|---|---|---|---|']
    for record in report['cases']:
        if record.get('case') == 'draw-order':
            orders = '; '.join(f"{r['compiler']} {r['tree']} {r['file']}: "
                               f"{', '.join(s['first_draw_to'] for s in r['sites']) or 'no site'}" for r in record['results'])
            lines.append(f"| 1b | draw-order | - | first draw -> {orders} |")
            continue
        if record['gate'] == 3:
            verdict = f"same seed identical: {record['same_seed_identical']}; other seed differs: {record['other_seed_differs']}"
            lines.append(f"| 3 | {record['case']} | `{record['runs'][0]['sha256'][:16]}` | {verdict} |")
            continue
        verdict = 'bit-exact' if record.get('identical', record.get('bit_exact')) else 'DIFFERS'
        if 'analysis' in record:
            verdict += f"; {record['analysis']['analyzer']} within tolerance: {record['analysis']['comparison']['within']}"
        label = f"{record['gate']}{record.get('part', '')}"
        lines.append(f"| {label} | {record['case']} | `{record['sha256'][:16]}` | {verdict} |")
    return '\n'.join(lines) + '\n'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', type=Path, required=True, help='channel-sim build directory (pathsim_chain.exe, pathsim_ref.exe)')
    parser.add_argument('--work', type=Path, help='work directory (default <build>/work)')
    parser.add_argument('--lane-root', type=Path, required=True, help='external reference-data root (read only; not distributed with this project)')
    parser.add_argument('--snapshot', default=g0.DEFAULT_SNAPSHOT, help='lane snapshot whose tools/ holds pathsim.exe and the resampler')
    parser.add_argument('--toolchain-bin', type=Path, help='runtime DLL directory put first on PATH (default: the recipe compiler dir)')
    parser.add_argument('--gates', type=parse_gates, default=frozenset((1, 2, 3)),
                        help='comma-separated gates to run, of 1, 2 and 3 (default 1,2,3)')
    parser.add_argument('--quick', action='store_true', help='skip the 1-hour and Appendix E cases')
    parser.add_argument('--full', action='store_true', help='add the remaining Appendix E runs')
    parser.add_argument('--keep-outputs', action='store_true', help='keep 48 kHz outputs larger than 64 MB (default: hash, then delete)')
    parser.add_argument('--arm-gxx', type=Path, help='arm-none-eabi-g++ for gate 1b (default: $ARMGCC_DIR, PATH, ~/.mcuxpressotools)')
    parser.add_argument('--allow-skip', action='store_true', help='exit 0 when gate 1b is skipped (no Arm compiler) and the rest pass')
    args = parser.parse_args()

    build = args.build.resolve()
    args.ref = build / 'pathsim_ref.exe'
    args.chain = build / 'pathsim_chain.exe'
    recipe = json.loads((build / 'pathsim_ref.recipe.json').read_text(encoding='utf-8'))
    chain_recipe = json.loads((build / 'pathsim_chain.recipe.json').read_text(encoding='utf-8'))
    args.chain_recipe = chain_recipe
    tools = args.lane_root / 'snapshots' / args.snapshot / 'tools'
    args.shipped = tools / 'pathsim.exe'
    args.resampler = tools / 'm110_wav_resample.exe'
    args.work = (args.work or build / 'work').resolve()
    args.toolchain_bin = args.toolchain_bin or Path(recipe['compiler']).parent
    for required in (args.ref, args.chain, args.shipped, args.resampler):
        if not required.is_file():
            sys.exit(f'missing: {required}')
    if g0.sha256(args.shipped) != g0.SHIPPED_PATHSIM_SHA256:
        sys.exit(f'{args.shipped} is not the lane reference {g0.SHIPPED_PATHSIM_SHA256}')
    selected = args.gates

    runner = Runner(args)
    started = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat()
    inputs, cases, provenance = g0.build_cases(runner.gate, args.quick)
    records: list[dict] = []
    gates: dict[str, dict] = {}
    if 1 in selected:
        gate1_records = gate1(runner, inputs, cases + [stereo_case(runner, inputs)] + delay_cases())
        records += gate1_records
        gates['gate1'] = {'cases': len(gate1_records), 'passed': sum(r['pass'] for r in gate1_records)}
        order = gate1_draw_order(runner)
        records.append(order)
        # Without an Arm compiler the check is SKIP, which fails the run unless --allow-skip.
        gates['gate1b'] = {'cases': 1, 'passed': int(order['pass']), 'skipped': order['skipped']}
    if 2 in selected:
        gate2_records = gate2_reference_cases(runner, args.quick, args.full) + gate2_analysis_cases(runner)
        records += gate2_records
        gates['gate2'] = {'cases': len(gate2_records), 'passed': sum(r['pass'] for r in gate2_records),
                          'bit_exact': sum(bool(r['bit_exact']) for r in gate2_records)}
    if 3 in selected:
        gate3_records = gate3(runner, inputs)
        records += gate3_records
        gates['gate3'] = {'cases': len(gate3_records), 'passed': sum(r['pass'] for r in gate3_records)}
    status = settle(gates, args.allow_skip)

    binaries = {name: {'path': str(path), 'sha256': g0.sha256(path)}
                for name, path in (('pathsim_chain', args.chain), ('pathsim_ref', args.ref), ('shipped_pathsim', args.shipped),
                                   ('lane_resampler', args.resampler))}
    runtime = {dll: g0.sha256(args.toolchain_bin / dll) if (args.toolchain_bin / dll).is_file() else None
               for dll in ('libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')}
    if not args.keep_outputs:
        for record in records:
            record.pop('output_path', None)
    report = {
        'schema': 'channel-sim.gates/1', 'started_utc': started,
        'finished_utc': datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat(),
        'tool': str(Path(__file__).resolve()), 'tool_sha256': g0.sha256(Path(__file__).resolve()),
        'argv': sys.argv, 'lane_root': str(args.lane_root), 'snapshot': args.snapshot,
        'reference_recipe': recipe, 'chain_recipe': chain_recipe, 'binaries': binaries, 'runtime_dlls': runtime,
        'toolchain_bin_prepended_to_path': str(args.toolchain_bin), 'priority': 'BELOW_NORMAL',
        'tolerances': {'snr_db': SNR_TOLERANCE_DB, 'doppler_relative': DOPPLER_TOLERANCE_REL,
                       'delay_samples': DELAY_TOLERANCE_SAMPLES, 'level_db': LEVEL_TOLERANCE_DB},
        'inputs': {name: {'path': str(path), 'sha256': g0.sha256(path)} for name, path in inputs.items()},
        'provenance': provenance, 'gates': gates, 'cases': records, 'commands': runner.gate.log,
        'pass': all(v['pass'] for v in gates.values()), 'allow_skip': args.allow_skip, 'exit_status': status,
    }
    (build / 'gates.json').write_text(json.dumps(report, indent=2, default=str) + '\n', encoding='utf-8')
    (build / 'gates.md').write_text(markdown(report), encoding='utf-8')
    for name, verdict in gates.items():
        print(f"{name}: {verdict_text(verdict)}")
    print(f"report {build / 'gates.json'}")
    return status


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        print(f'gates failed to run: {error}', file=sys.stderr)
        sys.exit(2)
