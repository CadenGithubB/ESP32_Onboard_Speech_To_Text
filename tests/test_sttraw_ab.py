#!/usr/bin/env python3
"""tools/sttraw_ab/score.py: alignment, segment-to-clip assignment and per-arm scoring on
constructed data (no recognizer, no replay binary). Also checks that the A/B playlist builder's
layout is consistent. Needs numpy (run with the export venv python).
"""
import sys
import unittest
from pathlib import Path

import numpy as np

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tools'))
from sttraw_ab import score  # noqa: E402
from sttraw_ab import make_playlist  # noqa: E402

SR = score.SR


def programme(seconds, rng):
    """Bursty noise standing in for speech: 1.5 s bursts with 1 s pauses."""
    x = np.zeros(int(seconds * SR), np.float32)
    for s in range(SR, x.size - 2 * SR, int(2.5 * SR)):
        x[s:s + int(1.5 * SR)] = rng.normal(0, 3000, int(1.5 * SR))
    return x


class Alignment(unittest.TestCase):
    def test_offset_and_drift_recovered(self):
        rng = np.random.default_rng(1)
        src = programme(200, rng)
        offset, ppm = 12345, 40.0
        t = np.arange(int(src.size * (1 + ppm * 1e-6)) + offset + 3 * SR)
        raw = np.interp((t - offset) / (1 + ppm * 1e-6), np.arange(src.size), src, left=0, right=0)
        raw = (raw * 0.05 + rng.normal(0, 10, raw.size)).astype(np.float32)
        fit = score.align(raw, src, src.size)
        self.assertLess(abs(fit['offset'] - offset), 3)
        self.assertLess(abs(fit['drift_ppm'] - ppm), 5)
        self.assertEqual(fit['kept'], fit['windows'])
        self.assertLess(fit['max_residual_ms'], 0.5)


class EnvelopeAlignment(unittest.TestCase):
    def test_lossy_regained_stream_is_followed(self):
        # A G2-like stream: lead-in, then the programme with a slow gain ride and 0.5% of it dropped in
        # 20 ms packets, so neither a waveform match nor one straight line fits.
        rng = np.random.default_rng(3)
        src = np.zeros(200 * SR, np.float32)   # irregular bursts, like speech (a periodic one is ambiguous)
        k = SR
        while k < src.size - 3 * SR:
            n = int(rng.uniform(0.4, 2.5) * SR)
            src[k:k + n] = rng.normal(0, rng.uniform(800, 4000), n)
            k += n + int(rng.uniform(0.2, 1.8) * SR)
        raw_pos, s_pos = 0, []
        lead = 3 * SR
        x = rng.normal(0, 8, lead).astype(np.float32)
        out = [x]
        raw_pos = lead
        for k in range(0, src.size, 320):
            if rng.random() < 0.005:
                continue
            blk = src[k:k + 320] * (0.5 + 0.3 * np.sin(k / src.size * 6))
            out.append(np.sign(blk) * np.abs(blk) ** 0.9 + rng.normal(0, 8, blk.size))
            s_pos.append((k, raw_pos))
            raw_pos += blk.size
        raw = np.concatenate(out).astype(np.float32)
        fit = score.align_envelope(raw, src, src.size)
        to_src = score.make_to_src(fit)
        self.assertGreaterEqual(fit['kept'], fit['windows'] - 1)
        err = [abs(to_src(r) - s) for s, r in s_pos[::200]]
        self.assertLess(max(err) / SR, 0.1)                            # within 100 ms (scoring collar 0.5 s)


def items():
    return [dict(index=0, category='silence', start_sample=0, end_sample=10 * SR, reference=''),
            dict(index=1, category='ls-test', start_sample=12 * SR, end_sample=15 * SR, reference='the cat sat'),
            dict(index=2, category='noise', start_sample=17 * SR, end_sample=27 * SR, reference=''),
            dict(index=3, category='ls-quiet', start_sample=30 * SR, end_sample=34 * SR, reference='a quiet one here')]


class Scoring(unittest.TestCase):
    def setUp(self):
        self.items = items()
        self.ident = lambda r: float(r)

    def test_assignment_uses_collar_for_speech_only(self):
        segs = [dict(start=int(11.7 * SR), end=int(11.95 * SR)),       # just before clip 1, inside its collar
                dict(start=int(16.9 * SR), end=int(17.4 * SR)),        # mostly in noise item 2
                dict(start=int(28.0 * SR), end=int(28.3 * SR)),        # between items, nothing within the collar
                dict(start=int(5 * SR), end=int(6 * SR))]              # in silence
        owner = score.assign(segs, self.items, self.ident)
        self.assertEqual([o['index'] if o else None for o in owner], [1, 2, None, 0])

    def test_phrase_opening_early_in_silence_belongs_to_its_speech(self):
        # Opens 3.7 s into the silence before clip 1 and runs 2 s into it: mostly silence by
        # time, but it is clip 1's caption. A 0.2 s graze of speech stays a silence segment.
        segs = [dict(start=int(6.3 * SR), end=int(12.5 * SR))]
        items = [dict(index=0, category='silence', start_sample=0, end_sample=10 * SR, reference=''),
                 dict(index=1, category='ls-test', start_sample=10 * SR, end_sample=13 * SR, reference='x')]
        self.assertEqual(score.assign(segs, items, self.ident)[0]['index'], 1)
        graze = [dict(start=int(6.3 * SR), end=int(10.2 * SR))]
        self.assertEqual(score.assign(graze, items, self.ident)[0]['index'], 0)

    def test_score_counts_errors_empties_and_false_captions(self):
        segs = [dict(start=12 * SR, end=15 * SR), dict(start=20 * SR, end=21 * SR),
                dict(start=2 * SR, end=3 * SR), dict(start=2 * SR, end=3 * SR)]
        texts = ['the cat sad', 'um hello there', 'um', '']
        owner = score.assign(segs, self.items, self.ident)
        r = score.score_arm(segs, texts, self.items, owner, self.ident, minutes=0.5)
        ls, quiet = r['categories']['ls-test'], r['categories']['ls-quiet']
        self.assertEqual((ls['S'], ls['D'], ls['I'], ls['empty']), (1, 0, 0, 0))
        self.assertEqual((quiet['D'], quiet['empty'], quiet['outside_s']), (4, 1, 4.0))
        self.assertEqual(r['false_captions']['noise'], [1, 2])          # filler words do not count
        self.assertEqual(r['false_captions']['silence'], [0, 0])        # "um" alone and empty text are no caption
        self.assertEqual(r['jobs_per_minute'], 8.0)
        self.assertAlmostEqual(r['total']['wer'], 100 * 5 / 7, places=2)


class Playlist(unittest.TestCase):
    def test_layout_from_constructed_pool(self):
        rng = np.random.default_rng(2)
        def clip(cat, k, label=False):
            return dict(category=cat, reference=f'{cat} {k}', label=label, night1_block=1, night1_index=k,
                        gain_db=0.0, pcm=(rng.normal(0, 1000, 3 * SR)).astype('<i2'))
        by = {c: [clip(c, k) for k in range(30)] for c in ('ls-test', 'syn-ord', 'syn-quiet', 'ami-one', 'noise')}
        by['ami-two'] = [clip('ami-two', k, True) for k in range(30)]
        import random
        audio, clips = make_playlist.build(by, random.Random(0))
        cats = [c['category'] for c in clips]
        self.assertEqual(cats.count('silence'), 3)
        for cat, n in make_playlist.PLAN.items():
            self.assertEqual(cats.count(cat), n)
        prev = 0
        for c in clips:
            self.assertGreaterEqual(c['start_sample'], prev)
            self.assertEqual(c['end_sample'] - c['start_sample'], round(c['seconds'] * SR))
            prev = c['end_sample']
        self.assertEqual(audio.size, clips[-1]['end_sample'])
        quiet = [c for c in clips if c['category'] == 'ls-quiet']
        self.assertTrue(all(c['gain_db'] == make_playlist.QUIET_DB for c in quiet))
        refs = [c['reference'] for c in clips if c['category'] in ('ls-test', 'ls-quiet')]
        self.assertEqual(len(refs), len(set(refs)))                     # quiet copies are different clips


if __name__ == '__main__':
    unittest.main()
