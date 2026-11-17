#!/usr/bin/env python3
"""Tests for hw1lm_wordpiece (no model or LM file needed; a stub LM records what is scored).

  python3 test_hw1lm_wordpiece.py
"""
import math
import random
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE), str(HERE.parents[0] / 'stt_train_15x5'), str(HERE.parents[1] / 'stt_train_15x5')]
sys.dont_write_bytecode = True
import hw1lm_wordpiece as wp  # noqa: E402

VOCAB = ['[PAD]', '[UNK]', '[CLS]', '[SEP]', '[MASK]', "'", 'it', 's', 'the', 'cat', 'sat', 'on', 'mat', 'h',
         '##at', '##s', '##e', 'a', '##t']


class StubLM:
    """Word ids for a few words; log10P depends only on the word (a unigram), so scores are easy to predict."""

    def __init__(self, words, log10):
        self.words = ['<s>', '</s>', '<unk>'] + list(words)
        self.word_id = {w: i for i, w in enumerate(self.words)}
        self.bos, self.eos, self.unk = 0, 1, 2
        self.log10 = log10
        self.params = dict(alpha=1.0, beta=0.0, unk_log10=-9.0, beam_width=16, char_prune=-30.0, hotword_bonus=1.0)
        self.scored = []

    def log10p(self, wid, ctx):
        self.scored.append(self.words[wid])
        return self.log10.get(self.words[wid], -1.0)


def peaked(ids, pieces, low=-12.0):
    """[T, V+1] log-probs whose argmax path is `ids` (blank = pieces.blank)."""
    lp = np.full((len(ids), pieces.blank + 1), low)
    for t, i in enumerate(ids):
        lp[t, i] = -0.01
    return lp


def test_join_matches_training_tokenizer():
    try:
        from citrinet import Tokenizer
    except ImportError:
        print('skip: training tokenizer not importable'); return
    tok, pieces = Tokenizer(VOCAB), wp.Pieces(VOCAB)
    rng = random.Random(7)
    for _ in range(2000):
        ids = [rng.randrange(len(VOCAB)) for _ in range(rng.randrange(0, 9))]
        assert pieces.join(ids) == tok.decode(ids), (ids, pieces.join(ids), tok.decode(ids))


def test_greedy_path_without_lm_weight():
    pieces = wp.Pieces(VOCAB)
    b = pieces.blank
    ids = [8, b, 9, 9, b, 10, b, 11, 8, 12, b]                 # the cat sat on the mat (repeat collapses)
    lm = StubLM(['the', 'cat', 'sat', 'on', 'mat'], {})
    assert wp.decode(peaked(ids, pieces), lm, pieces, alpha=0.0, beta=0.0) == 'the cat sat on the mat'
    ids = [12, b, 12]                                           # blank between repeats: two tokens
    assert wp.decode(peaked(ids, pieces), lm, pieces, alpha=0.0, beta=0.0) == 'mat mat'


def test_words_completed_and_apostrophe_glued():
    pieces = wp.Pieces(VOCAB)
    b = pieces.blank
    lm = StubLM(["it's", 'the', 'hat'], {"it's": -1.0, 'the': -1.0, 'hat': -1.0})
    ids = [6, 5, 7, b, 8, 13, 14]                               # it ' s | the | h ##at
    text, info = wp.decode(peaked(ids, pieces), lm, pieces, return_beams=True, alpha=1.0, beta=0.0, end_eos=False)
    assert text == "it's the hat", text
    assert {"it's", 'the', 'hat'} <= set(lm.scored), lm.scored
    assert not {'it', 's', "'", 'h'} & set(lm.scored), lm.scored
    best = info['final'][0]
    assert math.isclose(best[3], 3 * math.log(10) * -1.0), best   # three words, each alpha*ln10*(-1)


def test_lm_breaks_an_acoustic_tie():
    pieces = wp.Pieces(VOCAB)
    b = pieces.blank
    lp = np.full((3, b + 1), -12.0)
    lp[0, 9] = math.log(0.5); lp[0, 12] = math.log(0.5)         # "cat" or "mat", equally likely
    lp[1, b] = -0.01; lp[2, b] = -0.01
    lm = StubLM(['cat', 'mat'], {'cat': -0.5, 'mat': -3.0})
    assert wp.decode(lp, lm, pieces, alpha=0.5, beta=0.0, char_prune=-5.0) == 'cat'
    lm = StubLM(['cat', 'mat'], {'cat': -3.0, 'mat': -0.5})
    assert wp.decode(lp, lm, pieces, alpha=0.5, beta=0.0, char_prune=-5.0) == 'mat'


def test_hotword_and_oov():
    pieces = wp.Pieces(VOCAB)
    b = pieces.blank
    lp = np.full((3, b + 1), -12.0)
    lp[0, 9] = math.log(0.5); lp[0, 12] = math.log(0.5)
    lp[1, b] = -0.01; lp[2, b] = -0.01
    lm = StubLM(['cat'], {'cat': -2.0})                         # "mat" is OOV (-9)
    assert wp.decode(lp, lm, pieces, alpha=0.5, beta=0.0, char_prune=-5.0) == 'cat'
    big = 0.5 * math.log(10) * 7.5                               # more than the OOV penalty gap
    assert wp.decode(lp, lm, pieces, ['mat'], alpha=0.5, beta=0.0, hotword_bonus=big, char_prune=-5.0) == 'mat'


def test_specials_never_emitted():
    pieces = wp.Pieces(VOCAB)
    lp = np.full((2, pieces.blank + 1), -12.0)
    lp[0, 1] = -0.01                                            # [UNK] is the most likely token
    lp[0, 8] = -1.0
    lp[1, pieces.blank] = -0.01
    lm = StubLM(['the'], {})
    assert wp.decode(lp, lm, pieces, alpha=0.0, beta=0.0, char_prune=-5.0) == 'the'


if __name__ == '__main__':
    tests = [v for k, v in sorted(globals().items()) if k.startswith('test_')]
    for t in tests:
        t()
        print('ok', t.__name__)
    print(f'{len(tests)} passed')
