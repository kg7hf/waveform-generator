# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""Known answers for criterion 5's equivalent-SNR offset (lane call 3), its scorer (Q11) and the
threshold set's reuse of c5 renders (on fake simulators).

  python -B -m unittest discover -s signal-lab/validation -p "test_*.py" -v
"""

import argparse
import contextlib
import datetime
import io
import json
import math
import os
import sys
import tempfile
import unittest
import wave
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np  # noqa: E402

import run_watterson_validation as rv  # noqa: E402
import wv_analysis as wa  # noqa: E402

BITS = 10 ** 6
FAKE_FRAMES = 480                 # 10 ms: every fake WAV
FAKE_TONE_HZ = 1000.0
PCM24_FULL_SCALE = (1 << 23) - 1


def curve(*pairs):
    """(SNR dB, bit errors) pairs over BITS bits each."""
    return [(snr, errors, BITS) for snr, errors in pairs]


class CrossingTests(unittest.TestCase):
    def test_bracketed_crossing_and_offset_by_hand(self):
        # Stage: 2e-3 at 8 dB, 5e-4 at 10 dB, a factor 2 either side of 1e-3 in log: 9.0 dB.
        stage = curve((8, 2000), (10, 500), (12, 10))
        # pathsim: 4e-3 at 8 dB, 5e-4 at 10 dB: log10(4) / log10(8) = 2/3 of the way, 9 1/3 dB.
        pathsim = curve((8, 4000), (10, 500), (12, 5))
        a = wa.ber_crossing(stage)
        self.assertAlmostEqual(a['snr_db'], 9.0, places=12)
        self.assertEqual(a['bracket'], [8.0, 10.0])
        self.assertAlmostEqual(wa.ber_crossing(pathsim)['snr_db'], 8.0 + 4.0 / 3.0, places=12)
        result = wa.snr_offset(stage, pathsim, 0.5)
        self.assertAlmostEqual(result['offset_db'], -1.0 / 3.0, places=12)
        self.assertEqual(result['status'], 'pass')
        # pathsim 8e-3, 2e-3, 1.25e-4: between 10 and 12 dB, log10(2) / log10(16) = 1/4: 10.5 dB.
        late = curve((8, 8000), (10, 2000), (12, 125))
        self.assertAlmostEqual(wa.ber_crossing(late)['snr_db'], 10.5, places=12)
        result = wa.snr_offset(stage, late, 1.0)
        self.assertAlmostEqual(result['offset_db'], -1.5, places=12)
        self.assertEqual(result['status'], 'fail')

    def test_point_at_the_target_is_the_crossing(self):
        self.assertEqual(wa.ber_crossing(curve((6, 5000), (8, 1000), (10, 20)))['snr_db'], 8.0)
        # The last point at the target, the one before it above: the curve reaches it falling.
        self.assertEqual(wa.ber_crossing(curve((6, 4000), (8, 1000)))['snr_db'], 8.0)

    def test_point_at_the_target_on_a_rising_curve_is_no_crossing(self):
        crossing = wa.ber_crossing(curve((6, 500), (8, 1000), (10, 2000)))
        self.assertEqual((crossing['snr_db'], crossing['outside']), (None, 'rising'))
        self.assertEqual(wa.snr_offset(curve((6, 500), (8, 1000), (10, 2000)), curve((6, 4000), (8, 1000), (10, 20)), 1.0)['reason'],
                         'n/a: the BER does not fall through 0.001 with rising SNR')

    def test_crossing_outside_names_the_next_rung(self):
        below = curve((10, 500), (12, 10), (14, 0))
        result = wa.snr_offset(below, below, 1.0)
        self.assertEqual(result['status'], 'n/a')
        self.assertIsNone(result['offset_db'])
        self.assertEqual(result['reason'], 'n/a: crossing outside, add a rung at 8 dB')
        self.assertEqual(result['crossings']['stage']['outside'], 'below')
        above = curve((0, 50000), (2, 20000), (4, 5000))
        self.assertEqual(wa.snr_offset(above, above, 1.0)['reason'], 'n/a: crossing outside, add a rung at 6 dB')
        # One simulator bracketed, the other not: still no verdict.
        bracketed = curve((10, 2000), (12, 500), (14, 0))
        result = wa.snr_offset(bracketed, below, 1.0)
        self.assertEqual((result['status'], result['add_rungs_db']), ('n/a', [8.0]))
        self.assertAlmostEqual(result['crossings']['stage']['snr_db'], 11.0, places=12)

    def test_zero_error_point_is_placed_at_one_over_bits(self):
        crossing = wa.ber_crossing(curve((8, 2000), (10, 0), (12, 0)))
        expected = 8.0 + 2.0 * math.log10(2.0) / (math.log10(2.0) + 3.0)  # log10(1/1e6) = -6
        self.assertAlmostEqual(crossing['snr_db'], expected, places=12)
        self.assertAlmostEqual(crossing['snr_db'], 8.1823855, places=6)  # 8 + 2 x 0.30103 / 3.30103
        self.assertTrue(crossing['from_zero_error_point'])
        zero = crossing['points'][1]
        self.assertTrue(zero['zero_error'])
        self.assertEqual(zero['ber'], 0.0)
        self.assertAlmostEqual(zero['u95'], 3e-6)  # reported, not interpolated
        # A point estimate, not a bound: with the point at 3/bits the crossing is later.
        with_u95 = 8.0 + 2.0 * math.log10(2.0) / (math.log10(2.0) + 3.0 - math.log10(3.0))
        self.assertAlmostEqual(crossing['snr_db_u95'], with_u95, places=12)
        self.assertAlmostEqual(crossing['snr_db_u95'] - crossing['snr_db'], 0.0308154, places=6)
        reason = wa.snr_offset(curve((8, 2000), (10, 0)), curve((8, 2000), (10, 0)), 1.0)['reason']
        self.assertIn("stage's crossing uses a zero-error point placed at 1/bits, a point estimate (at its 95 % bound 3/bits "
                      'the crossing is 0.03 dB later)', reason)
        self.assertNotIn('upper bound', reason)
        self.assertIsNone(wa.ber_crossing(curve((8, 2000), (10, 500)))['snr_db_u95'])  # no zero-error point used
        # Every point error-free lies below 1e-3: the crossing is below the lowest point.
        self.assertEqual(wa.ber_crossing(curve((10, 0), (12, 0), (14, 0)))['outside'], 'below')

    def test_equal_curves_give_zero(self):
        points = curve((8, 3100), (10, 420), (12, 7))
        result = wa.snr_offset(points, list(points), 0.5)
        self.assertEqual(result['offset_db'], 0.0)
        self.assertEqual(result['status'], 'pass')


class ThresholdSetTests(unittest.TestCase):
    def test_default_set(self):
        rungs = rv.parse_threshold_set(rv.C5_THRESHOLD_SET)
        self.assertEqual(len(rungs), 12)
        self.assertIn(('600L', 'C06L05', 'MPP', 0.0), rungs)
        self.assertIn(('2400L', 'C24L05', 'AWGN', 6.0), rungs)
        jobs = rv.c5_ladder_jobs(rungs, lambda rung: rv.C5_EXTENDED_SEEDS)
        self.assertEqual(len(jobs), 288)
        self.assertEqual(jobs[0]['label'], 'c5-C24L05-MPP-8dB-s1-stage')
        self.assertIn('c5-C06L05-MPP-0dB-s12-pathsim', {j['label'] for j in jobs})

    def test_bad_set_is_refused(self):
        for text in ('C24L05-MPP', 'C99L05-MPP:4', 'C24L05-FOO:4', 'C24L05-MPP:x', ''):
            with self.assertRaises(ValueError):
                rv.parse_threshold_set(text)


class ScorerTests(unittest.TestCase):
    """score_manifest on a manifest of fake decodes (decode_existing.py's trials.jsonl)."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.renders = []

    def add(self, geometry, target, seeds, stage_errors, pathsim_errors, decoded=True, pathsim_seeds=None):
        """One rung: per-seed bit errors (a number for every seed, or a list) over 1000 payload bytes;
        pathsim has the stage's seeds unless pathsim_seeds names others."""
        for simulator, errors, sim_seeds in (('stage', stage_errors, seeds), ('pathsim', pathsim_errors, pathsim_seeds or seeds)):
            for index, seed in enumerate(sim_seeds):
                count = errors[index] if isinstance(errors, list) else errors
                rung = rv.c5_rung_name('C24L05', geometry, target)
                label = f'c5-{rung}-s{seed}-{simulator}'
                out = self.root / label
                if decoded:
                    out.mkdir()
                    trial = {'bit_errors': count, 'missing_bytes': 0, 'exact': count == 0}
                    (out / 'trials.jsonl').write_bytes((json.dumps(trial) + '\n').encode())
                self.renders.append({'kind': 'ladder', 'label': label, 'rung': rung, 'mode': '2400L', 'card': 'C24L05',
                                     'geometry': geometry, 'target_db': target, 'seed': seed, 'simulator': simulator,
                                     'payload': {'bytes': 1000}, 'decode': {'out_dir': str(out)}})

    def score(self):
        return rv.score_manifest({'renders': self.renders})

    def test_threshold_ladder(self):
        seeds = range(1, 13)
        self.add('MPP', 8.0, seeds, 24, 24)    # 3e-3 per seed on both
        self.add('MPP', 10.0, seeds, 4, 4)     # 5e-4
        self.add('MPP', 12.0, seeds, 0, 0)     # error-free on both: excluded from the rank-sum
        score = self.score()
        ladder, = score['ladders']
        self.assertEqual(ladder['ladder'], '2400L MPP (C24L05)')
        expected = 8.0 + 2.0 * (math.log10(3e-3) + 3.0) / (math.log10(3e-3) - math.log10(5e-4))
        self.assertAlmostEqual(ladder['crossings']['stage']['snr_db'], expected, places=12)
        self.assertEqual((ladder['status'], ladder['offset_db'], ladder['bound_db']), ('pass', 0.0, 1.0))
        statuses = {r['rung']: r['status'] for r in score['rungs']}
        self.assertEqual(statuses, {'C24L05-MPP-8dB': 'pass', 'C24L05-MPP-10dB': 'pass', 'C24L05-MPP-12dB': 'excluded'})
        self.assertEqual(score['status'], 'pass')
        self.assertIn('Equivalent-SNR offset', '\n'.join(rv.score_table(score)))

    def test_rejecting_rung_fails_and_crossing_outside_is_inconclusive(self):
        seeds = range(1, 13)
        self.add('AWGN', 6.0, seeds, 6, 2)    # every stage seed above every pathsim seed: p = 2 / C(24, 12)
        self.add('AWGN', 8.0, seeds, 0, 0)
        self.add('AWGN', 10.0, seeds, 0, 0)
        score = self.score()
        rung = next(r for r in score['rungs'] if r['rung'] == 'C24L05-AWGN-6dB')
        self.assertAlmostEqual(rung['rank_sum_p'], 2.0 / math.comb(24, 12), places=15)
        self.assertEqual(rung['status'], 'fail')
        ladder, = score['ladders']
        self.assertEqual((ladder['status'], ladder['bound_db']), ('n/a', 0.5))
        self.assertEqual(ladder['reason'], 'n/a: crossing outside, add a rung at 4 dB')
        self.assertEqual(score['status'], 'fail')

    def test_ladder_beyond_its_bound_fails_through_score_c5(self):
        """A manifest on disk whose only failure is a ladder's offset: --score-c5 says FAIL and exits 1."""
        seeds = range(1, 13)
        # pathsim 3e-3, 5e-4, error-free; the stage reaches the same BERs 2 dB later. One stage seed
        # carries each extra error, so every rung's rank-sum is p = 1 and passes: the offset alone fails.
        self.add('MPP', 8.0, seeds, 24, 24)
        self.add('MPP', 10.0, seeds, [4] * 11 + [244], 4)
        self.add('MPP', 12.0, seeds, [0] * 11 + [48], 0)
        path = self.root / 'c5t' / 'manifest.json'
        rv.write_json(path, {'renders': self.renders})
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            code = rv.score_c5(path)
        self.assertEqual(code, 1)
        self.assertIn('criterion 5: FAIL', printed.getvalue())
        score = rv.read_json(path.with_name('score.json'))
        self.assertEqual(score['status'], 'fail')
        self.assertEqual({r['rung']: (r['status'], r['rank_sum_p']) for r in score['rungs']},
                         {rung: ('pass', 1.0) for rung in ('C24L05-MPP-8dB', 'C24L05-MPP-10dB', 'C24L05-MPP-12dB')})
        ladder, = score['ladders']
        self.assertEqual(ladder['status'], 'fail')
        self.assertAlmostEqual(ladder['offset_db'], 2.0, places=12)
        self.assertEqual(ladder['reason'], 'offset +2.00 dB, beyond +/-1 dB')
        # Criterion 5 fails on it as the threshold set's score, and as c5's beside a passing threshold set.
        passing = {'status': 'pass', 'rungs': [], 'ladders': [], 'd62': []}
        self.assertEqual(rv.c5_status(passing, {'score': score}), 'fail')
        self.assertEqual(rv.c5_status(score, {'score': passing}), 'fail')

    def test_four_seeds_are_informational_and_spacing_is_required(self):
        self.add('MPP', 10.0, range(1, 5), [0, 40, 0, 2], [5, 0, 0, 0])
        self.add('MPP', 14.0, range(1, 5), 0, 0)
        self.add('MPP', 18.0, range(1, 5), 0, 0)
        score = self.score()
        rung = next(r for r in score['rungs'] if r['rung'] == 'C24L05-MPP-10dB')
        self.assertEqual(rung['status'], 'informational')
        self.assertIsNotNone(rung['rank_sum_p'])
        ladder, = score['ladders']
        self.assertEqual(ladder['status'], 'n/a')
        self.assertTrue(ladder['reason'].startswith('n/a: the curve needs >= 3 SNR points 2 dB apart'))
        self.assertIn('add a rung at 8 dB', ladder['reason'])
        self.assertEqual(score['status'], 'inconclusive')

    def test_rank_sum_level_is_005_over_8(self):
        """Q11's level per rung is 0.05 / 8: p about 0.02 passes and p about 0.005 fails. The p values
        are exact counts of the C(24, 12) assignments (checked against a separate counting oracle)."""
        seeds = range(1, 13)
        self.add('AWGN', 6.0, seeds, list(range(5, 17)), list(range(1, 13)))  # each stage seed 4 errors above pathsim's
        self.add('AWGN', 8.0, seeds, list(range(6, 18)), list(range(1, 13)))  # 5 above
        rungs = {r['rung']: r for r in self.score()['rungs']}
        near, far = rungs['C24L05-AWGN-6dB'], rungs['C24L05-AWGN-8dB']
        self.assertAlmostEqual(near['rank_sum_p'], 53150 / math.comb(24, 12), places=15)
        self.assertTrue(rv.C5_ALPHA < near['rank_sum_p'] < 0.05)
        self.assertEqual((near['status'], near['reason']), ('pass', 'rank-sum p = 0.01965 >= 0.00625'))
        self.assertAlmostEqual(far['rank_sum_p'], 12892 / math.comb(24, 12), places=15)
        self.assertEqual((far['status'], far['reason']), ('fail', 'rank-sum p = 0.004767 < 0.00625'))

    def test_one_simulator_error_free_is_not_excluded(self):
        """Only a rung where both simulators are error-free on every seed is excluded from the rank-sum."""
        seeds = range(1, 13)
        self.add('MPP', 10.0, seeds, 0, 3)                # the stage error-free, pathsim 3 on every seed
        self.add('MPP', 12.0, seeds, [1] + [0] * 11, 0)  # pathsim error-free, one stage error
        self.add('MPP', 14.0, seeds, 0, 0)
        rungs = {r['rung']: r for r in self.score()['rungs']}
        self.assertEqual(rungs['C24L05-MPP-10dB']['status'], 'fail')
        self.assertAlmostEqual(rungs['C24L05-MPP-10dB']['rank_sum_p'], 2.0 / math.comb(24, 12), places=15)
        self.assertEqual((rungs['C24L05-MPP-12dB']['status'], rungs['C24L05-MPP-12dB']['rank_sum_p']), ('pass', 1.0))
        self.assertEqual((rungs['C24L05-MPP-14dB']['status'], rungs['C24L05-MPP-14dB']['rank_sum_p']), ('excluded', None))

    def test_unequal_seed_counts_give_no_rank_sum(self):
        self.add('MPP', 10.0, range(1, 13), 4, 4, pathsim_seeds=range(1, 12))
        rung, = self.score()['rungs']
        self.assertEqual((rung['status'], rung['rank_sum_p'], rung['seeds_per_simulator']), ('informational', None, 11))
        self.assertEqual(rung['reason'], 'unequal seed counts (12 and 11): no rank-sum verdict')
        self.assertEqual((rung['stage']['seeds'], rung['pathsim']['seeds']), (12, 11))

    def test_two_point_ladder_is_not_qualified(self):
        self.add('MPP', 8.0, range(1, 5), 24, 24)  # 3e-3 and 5e-4: the crossing is bracketed
        self.add('MPP', 10.0, range(1, 5), 4, 4)
        score = self.score()
        ladder, = score['ladders']
        self.assertEqual((ladder['status'], ladder['offset_db'], ladder['offset_db_unqualified']), ('n/a', None, 0.0))
        self.assertEqual(ladder['reason'], 'n/a: the curve needs >= 3 SNR points 2 dB apart, each pooling >= 4 seeds (has 8, 10 dB, '
                                           '4 seeds at the fewest); read anyway: offset +0.00 dB, within +/-1 dB')
        self.assertEqual(score['status'], 'inconclusive')

    def test_three_seeds_do_not_qualify_a_ladder_and_four_do(self):
        """A ladder's points pool >= 4 seeds each, judged on its fewest: 3 seeds on one point is n/a."""
        for target, errors in ((8.0, 24), (10.0, 4), (12.0, 0)):
            self.add('MPP', target, range(1, 4) if target == 10.0 else range(1, 5), errors, errors)
            self.add('AWGN', target - 2.0, range(1, 5), errors, errors)
        ladders = {item['geometry']: item for item in self.score()['ladders']}
        mpp, awgn = ladders['MPP'], ladders['AWGN']
        self.assertEqual((mpp['status'], mpp['offset_db'], mpp['offset_db_unqualified']), ('n/a', None, 0.0))
        self.assertIn('(has 8, 10, 12 dB, 3 seeds at the fewest)', mpp['reason'])
        self.assertEqual((awgn['status'], awgn['offset_db'], awgn['bound_db']), ('pass', 0.0, 0.5))

    def test_missing_decode(self):
        self.add('MPP', 8.0, range(1, 13), 24, 24)
        self.add('MPP', 10.0, range(1, 13), 4, 4, decoded=False)
        self.add('MPP', 12.0, range(1, 13), 0, 0)
        score = self.score()
        self.assertEqual(score['status'], 'missing')
        self.assertEqual(len(score['missing']), 24)
        self.assertEqual(score['ladders'][0]['status'], 'missing')

    def test_zero_error_crossing_is_footnoted_as_a_point_estimate(self):
        self.add('MPP', 8.0, range(1, 13), 24, 24)
        self.add('MPP', 10.0, range(1, 13), 0, 0)
        self.add('MPP', 12.0, range(1, 13), 0, 0)
        score = self.score()
        ladder, = score['ladders']
        self.assertEqual((ladder['status'], ladder['offset_db']), ('pass', 0.0))
        self.assertTrue(ladder['crossings']['stage']['from_zero_error_point'])
        table = '\n'.join(rv.score_table(score))
        self.assertIn('* uses a zero-error point placed at 1/bits: a point estimate, not a bound', table)
        self.assertIn('2400L MPP (C24L05) stage', table)
        self.assertNotIn('upper bound', table)
        self.assertIn('point estimate', '\n'.join(rv.score_markdown(score)))

    def test_full_pass_reads_the_threshold_set(self):
        """c5_threshold_score: <work>/c5t/manifest.json scored; c5t/score.json only once every render is decoded."""
        self.add('MPP', 8.0, range(1, 13), 24, 24)
        self.add('MPP', 10.0, range(1, 13), 4, 4)
        self.add('MPP', 12.0, range(1, 13), 0, 0)
        work = self.root / 'work'
        runner = FakeRunner(work)
        self.assertIsNone(rv.c5_threshold_score(runner))
        rv.write_json(work / 'c5t' / 'manifest.json', {'renders': self.renders})
        threshold = rv.c5_threshold_score(runner)
        self.assertEqual((threshold['renders'], threshold['decoded'], threshold['score']['status']), (72, 72, 'pass'))
        self.assertEqual(rv.read_json(work / 'c5t' / 'score.json')['status'], 'pass')
        (work / 'c5t' / 'score.json').unlink()
        (Path(self.renders[0]['decode']['out_dir']) / 'trials.jsonl').unlink()
        threshold = rv.c5_threshold_score(runner)
        self.assertEqual((threshold['decoded'], threshold['score']['status']), (71, 'missing'))
        self.assertFalse((work / 'c5t' / 'score.json').exists())


class FakeRunner:
    """The two things c5_threshold_score uses of a Runner."""

    def __init__(self, work: Path) -> None:
        self.work = work

    def inside(self, *parts: str) -> Path:
        path = self.work.joinpath(*parts)
        path.parent.mkdir(parents=True, exist_ok=True)
        return path


class StatusTests(unittest.TestCase):
    """c5_status: criterion 5 from c5's score and the threshold set's."""

    @staticmethod
    def score(status, rungs=(), ladders=(), d62=()):
        return {'status': status, 'rungs': [{'status': s} for s in rungs], 'ladders': [{'status': s} for s in ladders],
                'd62': [{'status': s} for s in d62]}

    def test_c5_alone_and_with_the_threshold_set(self):
        c5 = self.score('inconclusive', rungs=('excluded', 'pass'), ladders=('n/a', 'n/a'), d62=('pass',))
        self.assertEqual(rv.c5_status(None, None), 'missing')
        self.assertEqual(rv.c5_status(c5, None), 'inconclusive')
        for theirs in ('missing', 'pass', 'inconclusive', 'fail'):
            self.assertEqual(rv.c5_status(c5, {'score': self.score(theirs)}), theirs)
        self.assertEqual(rv.c5_status(None, {'score': self.score('pass')}), 'missing')
        for failing in (self.score('fail', rungs=('fail',)), self.score('fail', d62=('fail',)), self.score('fail', ladders=('fail',))):
            self.assertEqual(rv.c5_status(failing, {'score': self.score('pass')}), 'fail')
        self.assertEqual(rv.c5_status(c5 | {'rungs': [{'status': 'fail'}]}, {'score': self.score('missing')}), 'missing')


class BinaryEvidenceTests(unittest.TestCase):
    """c5_binary_evidence: which c5 renders today's binary made."""

    def test_recorded_hash_or_a_binary_older_than_the_record(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'tool.exe'
            path.write_bytes(b'x')
            built = datetime.datetime(2026, 10, 9, 7, 57, 40, tzinfo=datetime.timezone.utc).timestamp()
            os.utime(path, (built, built))
            binary = {'path': str(path), 'sha256': 'a' * 64}
            self.assertEqual(rv.c5_binary_evidence({'binary_sha256': 'a' * 64}, binary), 'binary sha256 recorded with the render and equal')
            self.assertIsNone(rv.c5_binary_evidence({'binary_sha256': 'b' * 64, 'measured_by': {'utc': '2026-10-09T09:49:06+00:00'}}, binary))
            self.assertIn('is older than the record (2026-10-09T09:49:06+00:00)',
                          rv.c5_binary_evidence({'measured_by': {'utc': '2026-10-09T09:49:06+00:00'}}, binary))
            self.assertIsNone(rv.c5_binary_evidence({'measured_by': {'utc': '2026-10-09T07:57:40+00:00'}}, binary))  # same second
            self.assertIsNone(rv.c5_binary_evidence({'measured_by': {'utc': '2026-10-09T07:00:00+00:00'}}, binary))  # rebuilt since
            self.assertIsNone(rv.c5_binary_evidence({}, binary))


def write_wav(path: Path, amplitude: float) -> None:
    """A mono PCM24 48 kHz WAV of FAKE_FRAMES frames of a tone, with no zero sample."""
    phase = 2.0 * np.pi * FAKE_TONE_HZ * (np.arange(FAKE_FRAMES) + 0.25) / rv.FS
    codes = np.round(amplitude * PCM24_FULL_SCALE * np.sin(phase)).astype('<i4')
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), 'wb') as handle:
        handle.setnchannels(1)
        handle.setsampwidth(3)
        handle.setframerate(rv.FS)
        handle.writeframes(codes.view(np.uint8).reshape(-1, 4)[:, :3].tobytes())


class FakeTools(rv.Runner):
    """A Runner whose commands fake the driver, pathsim_chain and the lane's analyzer: each writes the
    files the real one writes (a short PCM24 render, the driver's sidecar, the analysis JSON), beside a
    lane tree with one card, so c5_render and c5_threshold_prepare run without a build or the lane."""

    CARD = 'C24L05'
    PAYLOAD_BYTES = 1000

    def __init__(self, root: Path) -> None:
        build = root / 'build'
        build.mkdir()
        driver, chain = build / 'wfg_watterson_render.exe', build / 'pathsim_chain.exe'
        driver.write_bytes(b'driver, build A')
        chain.write_bytes(b'pathsim_chain, build A')
        lane = root / 'lane'
        self.stim = lane / 'hfchannel/conformance/stimuli'
        clean = self.stim / 'src' / f'{self.CARD}-clean-48k.wav'
        write_wav(clean, 0.25)
        (self.stim / f'{self.CARD}.BIN').write_bytes(bytes(self.PAYLOAD_BYTES))
        rv.write_json(self.stim / f'{self.CARD}.timeline.json', {'payload_bytes': self.PAYLOAD_BYTES})
        (root / 'work').mkdir()
        super().__init__(argparse.Namespace(work=root / 'work', toolchain_bin=None, lane_root=lane, driver=driver, chain=chain,
                                            gate_script=None, resume=False, keep_outputs=True, quick=False,
                                            c5_threshold='C24L05-MPP:8', c5_reuse=None))
        self.level = {'trimmed_rms': rv.trimmed_rms(clean), 'sha256': rv.sha256(clean)}
        self.binaries = {name: {'path': str(path), 'sha256': rv.sha256(path)} for name, path in (('driver', driver), ('pathsim_chain', chain))}

    def run(self, command: list) -> tuple[float, str]:
        argv = [str(part) for part in command]
        self.log.append({'argv': argv})
        if argv[0] == str(self.args.driver):
            write_wav(Path(argv[argv.index('--output') + 1]), 0.125)
            rv.write_json(Path(argv[argv.index('--sidecar') + 1]),
                          {'output_digest': '0x1', 'clipped_samples': 0, 'watterson': {'parameter_digest': '0x2'}})
        elif argv[0] == str(self.args.chain):
            write_wav(Path(argv[-1]), 0.125)
        else:
            rv.write_json(Path(argv[argv.index('--out') + 1]), {'snr_db': 8.0, 'snr_standard_error_db': 0.01})
        return 0.0, ''


class FakeProbes:
    """The one probe c5_render asks for, the companion gapnoise (the fake analyzer reads nothing)."""

    def __init__(self, root: Path) -> None:
        self.companion = rv.Probe('gapnoise_305s', root / 'probe_gapnoise_305s.wav',
                                  {'probe': 'gapnoise', 'body': {'start_sample': 0, 'samples': FAKE_FRAMES}})

    def get(self, name: str) -> rv.Probe:
        return self.companion


class ReuseTests(unittest.TestCase):
    """The threshold set's reuse of c5 renders (c5_reuse), its mixed-binary guard (c5_other_builds),
    and their wiring in c5_threshold_prepare, on fake simulators (FakeTools)."""

    STAGE, PATHSIM = 'c5-C24L05-MPP-8dB-s1-stage', 'c5-C24L05-MPP-8dB-s1-pathsim'

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.runner = FakeTools(self.root)
        self.probes = FakeProbes(self.root)
        rungs = rv.parse_threshold_set(self.runner.args.c5_threshold)
        self.jobs = {job['label']: job for job in rv.c5_ladder_jobs(rungs, lambda rung: (1, 2))}
        self.source = self.runner.work / 'c5' / 'manifest.json'

    @staticmethod
    def quietly(call, *args):
        with contextlib.redirect_stdout(io.StringIO()):
            return call(*args)

    def c5_renders(self) -> dict[str, dict]:
        """c5's renders of seeds 1 and 2, made as c5_prepare makes them, and c5's manifest."""
        records = {}
        for label, job in self.jobs.items():
            record = rv.c5_render(self.runner, self.probes, **job, card_level=self.runner.level, stim=self.runner.stim)
            records[label] = self.quietly(rv.row_store, self.runner, label, record)
        rv.write_json(self.source, {'renders': list(records.values())})
        return records

    def binary(self, row: dict) -> dict:
        return self.runner.binaries[rv.C5_TOOLS[row['simulator']]]

    def reuse(self, label: str, candidate: dict | None, binary: dict | None = None, level: dict | None = None) -> dict | None:
        job = self.jobs[label]
        return rv.c5_reuse(self.runner, job, candidate, binary or self.binary(job), level or self.runner.level, self.runner.stim,
                           self.source)

    def test_c5_render_records_its_binary(self):
        for label, record in self.c5_renders().items():
            self.assertEqual(record['binary_sha256'], self.binary(record)['sha256'], label)

    def test_reuse_decision(self):
        records = self.c5_renders()
        for label, candidate in records.items():
            row = self.reuse(label, candidate)
            self.assertEqual(row['output'], candidate['output'], label)
            self.assertEqual(row['reused_from']['binary_sha256'], self.binary(candidate)['sha256'], label)
            self.assertIn('binary sha256 recorded with the render and equal', row['reused_from']['checked'])
            self.assertNotIn('reused_from', candidate)  # a copy: c5's record is unchanged
        self.assertIsNone(self.reuse(self.STAGE, None))
        for label in (self.STAGE, self.PATHSIM):  # another build made it
            self.assertIsNone(self.reuse(label, records[label], binary=self.binary(records[label]) | {'sha256': 'f' * 64}))
        self.assertIsNone(self.reuse(self.STAGE, records['c5-C24L05-MPP-8dB-s2-stage']))      # another seed's scenario
        self.assertIsNone(self.reuse(self.PATHSIM, records['c5-C24L05-MPP-8dB-s2-pathsim']))  # another seed's argv
        self.assertIsNone(self.reuse(self.STAGE, records[self.STAGE], level=self.runner.level | {'sha256': '0' * 64}))  # another card
        write_wav(Path(records[self.PATHSIM]['output']['path']), 0.5)  # the output changed since c5
        self.assertIsNone(self.reuse(self.PATHSIM, records[self.PATHSIM]))

    def test_other_builds(self):
        rows = list(self.c5_renders().values())
        binaries = self.runner.binaries
        self.assertEqual(rv.c5_other_builds(rows, binaries), [])
        today = binaries['pathsim_chain']['sha256']
        self.assertEqual(rv.c5_other_builds(rows + [rows[1] | {'binary_sha256': 'e' * 64}], binaries), ['e' * 16])
        self.assertEqual(rv.c5_other_builds(rows + [{k: v for k, v in rows[1].items() if k != 'binary_sha256'}], binaries), ['None'])
        reused = rows[1] | {'binary_sha256': 'c' * 64, 'reused_from': {'binary_sha256': today}}  # checked at reuse: that one counts
        self.assertEqual(rv.c5_other_builds([reused], binaries), [])
        self.assertEqual(rv.c5_other_builds([reused | {'reused_from': {'binary_sha256': 'd' * 64}}], binaries), ['d' * 16])
        # Referenced by 3b4aeb1's runner (no hash kept): today's binary where it is older than the record.
        old = {k: v for k, v in rows[1].items() if k != 'binary_sha256'} | {'reused_from': {'label': rows[1]['label']}}
        self.assertEqual(rv.c5_other_builds([old | {'measured_by': {'utc': '2099-01-01T00:00:00+00:00'}}], binaries), [])
        self.assertEqual(rv.c5_other_builds([old | {'measured_by': {'utc': '2000-01-01T00:00:00+00:00'}}], binaries), ['None'])

    def test_threshold_prepare_wiring(self):
        """c5_threshold_prepare references only c5 renders of today's build, records every row's binary,
        and a --resume pass stops on a row another build made."""
        records = self.c5_renders()
        records['c5-C24L05-MPP-8dB-s2-stage']['binary_sha256'] = 'f' * 64  # c5 made this one with another build
        rv.write_json(self.source, {'renders': list(records.values())})
        block = self.quietly(rv.c5_threshold_prepare, self.runner, self.probes)
        self.assertEqual((block['renders'], block['reused'], block['rendered']), (24, 3, 21))
        manifest = rv.read_json(self.runner.work / 'c5t' / 'manifest.json')
        self.assertEqual(manifest['reused']['labels'], [self.STAGE, self.PATHSIM, 'c5-C24L05-MPP-8dB-s2-pathsim'])
        for row in manifest['renders']:
            made_by = row['reused_from']['binary_sha256'] if 'reused_from' in row else row['binary_sha256']
            self.assertEqual(made_by, self.binary(row)['sha256'], row['label'])
        self.runner.args.resume = True
        block = self.quietly(rv.c5_threshold_prepare, self.runner, self.probes)
        self.assertEqual((block['renders'], block['reused'], block['rendered']), (24, 3, 21))
        rows = self.runner.work / 'c5t' / 'rows'
        rendered = rows / 'c5-C24L05-MPP-8dB-s5-stage.json'
        kept = rendered.read_bytes()
        rv.write_json(rendered, rv.read_json(rendered) | {'binary_sha256': 'e' * 64})
        with self.assertRaisesRegex(RuntimeError, r'rows made by other or unknown binaries \(eeeeeeeeeeeeeeee\)'):
            self.quietly(rv.c5_threshold_prepare, self.runner, self.probes)
        rendered.write_bytes(kept)
        referenced = rows / f'{self.PATHSIM}.json'
        row = rv.read_json(referenced)
        row['reused_from']['binary_sha256'] = 'd' * 64
        rv.write_json(referenced, row)
        with self.assertRaisesRegex(RuntimeError, r'rows made by other or unknown binaries \(dddddddddddddddd\)'):
            self.quietly(rv.c5_threshold_prepare, self.runner, self.probes)


if __name__ == '__main__':
    unittest.main()
