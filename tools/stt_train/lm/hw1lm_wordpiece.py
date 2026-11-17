"""CTC prefix beam search over WordPiece tokens with the HW1LM1 word LM (reference decoder for Citrinet).

The same search as hw1lm.decode (FORMAT.md), with tokens in place of characters:
  * A prefix is a tuple of token ids; blank / repeat collapse is per token, exactly as per character.
  * A word ends where the next word starts: a word-start piece (no "##") after a non-empty word completes
    the word and adds alpha * ln10 * log10P(word | context) + beta (+ hotword_bonus) to the beam's LM score,
    as a space does in the character decoder. Continuation pieces ("##x") extend the current word.
  * The apostrophe is its own token (BERT pre-tokenization: "it's" -> it ' s). It joins the word before it
    and the piece after it, as the tokenizer's decode does, so "it's" is scored as one LM word.
  * Special tokens ([PAD] [UNK] [CLS] [SEP] [MASK]) are never emitted (the greedy decode drops them too).
  * char_prune applies per token: a token is a candidate in a frame when its log-probability >= char_prune.
  * At the end the open word is completed and </s> scored (end_eos), as in hw1lm.decode.
The returned text is the tokenizer's own decode of the best token prefix.
"""
from __future__ import annotations

import math
import re

import numpy as np

from hw1lm import NEG_INF, PARAM_NAMES, _lse, log_softmax, start_context

SPECIAL = re.compile(r'^\[[A-Z]+\]$')


class Pieces:
    """Token kinds and texts for a BERT-style WordPiece vocabulary (vocab.txt lines, blank = len(vocab))."""

    def __init__(self, vocab):
        self.vocab = list(vocab)
        self.blank = len(self.vocab)
        self.kind, self.text = [], []
        for t in self.vocab:
            if SPECIAL.match(t):
                self.kind.append('special'); self.text.append('')
            elif t == "'":
                self.kind.append('apos'); self.text.append("'")
            elif t.startswith('##'):
                self.kind.append('cont'); self.text.append(t[2:])
            else:
                self.kind.append('start'); self.text.append(t)
        self.emit = np.array([k != 'special' for k in self.kind] + [False])   # candidates (blank handled apart)

    def join(self, ids):
        """The tokenizer's decode: ' ' before word starts, '##' stripped, apostrophes glued both ways."""
        text = ''
        for i in ids:
            k = self.kind[i]
            if k == 'special':
                continue
            text += self.text[i] if k == 'cont' else ' ' + self.text[i]
        text = re.sub(r"\s*'\s*", "'", text)
        return re.sub(r'\s+', ' ', text).strip()


def decode(log_probs, lm, pieces, custom_words=None, return_beams=False, context=None, end_eos=True, **overrides):
    """log_probs: [T, len(vocab) + 1] natural-log probabilities (blank last). Returns the best text, or
    (text, info) with return_beams=True. Parameters as hw1lm.decode (char_prune = per-token prune)."""
    p = dict(lm.params)
    for k, v in overrides.items():
        if k not in PARAM_NAMES:
            raise TypeError(f'unknown decoder parameter {k}')
        if v is not None:
            p[k] = v
    alpha_ln = float(p['alpha']) * math.log(10)
    beta = float(p['beta'])
    unk_log10 = float(p['unk_log10'])
    width = int(p['beam_width'])
    prune = float(p['char_prune'])
    bonus = float(p['hotword_bonus'])
    custom = frozenset(custom_words or ())
    word_id, unk = lm.word_id, lm.unk
    specials = (lm.bos, lm.eos, lm.unk)
    kind, text = pieces.kind, pieces.text
    blank = pieces.blank
    cache = {}

    def complete(word, ctx):
        key = (word, ctx)
        hit = cache.get(key)
        if hit is None:
            wid = word_id.get(word)
            if wid is None or wid in specials:
                l10, wid = unk_log10, unk
            else:
                l10 = lm.log10p(wid, ctx)
            hit = cache[key] = (alpha_ln * l10 + beta + (bonus if word in custom else 0.0), (ctx[1], wid))
        return hit

    def extend(prefix, state, c):
        """State after appending token c to prefix: (lm, ctx, word, glue)."""
        lms, ctx, word, glue = state
        k = kind[c]
        if k == 'cont':
            return lms, ctx, word + text[c], False
        if k == 'apos':
            return lms, ctx, word + "'", True
        if glue or not word:            # the piece after an apostrophe, or the first word
            return lms, ctx, word + text[c], False
        delta, nctx = complete(word, ctx)
        return lms + delta, nctx, text[c], False

    lp = np.asarray(log_probs, dtype=np.float64)
    if lp.ndim != 2 or lp.shape[1] != blank + 1:
        raise ValueError(f'log_probs must be [T, {blank + 1}]')
    rows = lp.tolist()
    allowed = [np.nonzero(r)[0].tolist() for r in ((lp >= prune) & pieces.emit)]
    # beam: token tuple -> [p_b, p_nb, (lm, ctx, word, glue)]
    beams = {(): [0.0, NEG_INF, (0.0, start_context(lm, context), '', False)]}
    for row, cands in zip(rows, allowed):
        nxt = {}
        lp_blank = row[blank]
        for prefix, (pb, pnb, state) in beams.items():
            tot = _lse(pb, pnb)
            e = nxt.get(prefix)
            if e is None:
                e = nxt[prefix] = [NEG_INF, NEG_INF, state]
            e[0] = _lse(e[0], tot + lp_blank)
            last = prefix[-1] if prefix else -1
            if last >= 0:
                lc = row[last]
                e[1] = _lse(e[1], pnb + lc)            # repeat collapses into the same token
                if pb != NEG_INF:                       # blank between: the token again
                    np_ = prefix + (last,)
                    f = nxt.get(np_)
                    if f is None:
                        f = nxt[np_] = [NEG_INF, NEG_INF, extend(prefix, state, last)]
                    f[1] = _lse(f[1], pb + lc)
            if tot == NEG_INF:
                continue
            for c in cands:
                if c == last:
                    continue
                np_ = prefix + (c,)
                f = nxt.get(np_)
                if f is None:
                    f = nxt[np_] = [NEG_INF, NEG_INF, extend(prefix, state, c)]
                f[1] = _lse(f[1], tot + row[c])
        scored = []
        for prefix, e in nxt.items():
            s = _lse(e[0], e[1])
            if s != NEG_INF:
                scored.append((-(s + e[2][0]), prefix))
        if len(scored) > width:
            scored.sort()
            scored = scored[:width]
        beams = {prefix: nxt[prefix] for _, prefix in scored}
    eos_cache, final = {}, []
    for prefix, (pb, pnb, (lms, ctx, word, glue)) in beams.items():
        tot = _lse(pb, pnb)
        if word:
            delta, ctx = complete(word, ctx)
            lms += delta
        if end_eos:
            eos = eos_cache.get(ctx)
            if eos is None:
                eos = eos_cache[ctx] = alpha_ln * lm.log10p(lm.eos, ctx)
            lms += eos
        final.append((tot + lms, prefix, tot, lms))
    final.sort(key=lambda r: (-r[0], r[1]))
    best = pieces.join(final[0][1]) if final else ''
    if return_beams:
        return best, {'final': final, 'params': p}
    return best


def decode_logits(logits, lm, pieces, custom_words=None, **overrides):
    return decode(log_softmax(logits), lm, pieces, custom_words, **overrides)


def log_softmax_int8(q, exponent):
    """Per-frame log_softmax of int8 outputs q * 2^exponent (ESP-DL sign) exactly as stt_lm_wordpiece.cpp
    computes it: exp of each (q - qmax) * 2^exponent, summed in class order, natural log of the sum. It
    differs from hw1lm.log_softmax (numpy's pairwise sum) only in the last bits, which can still decide an
    exact-looking tie between beams; the C++ parity test therefore feeds both decoders these values."""
    q = np.asarray(q, dtype=np.int64)
    scale = 2.0 ** exponent
    d = q - q.max(axis=1, keepdims=True)                       # 0 .. -255
    table = np.array([math.exp(-k * scale) for k in range(256)])
    sums = np.cumsum(table[-d], axis=1)[:, -1]                 # sequential, in class order
    return d * scale - np.array([math.log(s) for s in sums])[:, None]
