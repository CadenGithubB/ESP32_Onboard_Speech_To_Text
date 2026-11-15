#!/usr/bin/env python3
"""Digitally attenuate a scored sttraw recording to a chosen speech level (night-level proxy).

  make_proxy.py --score score.json --out proxy.pcm (--db -12 | --target-p90 44) [--raw raw.pcm] [--manifest m.json]

No continuous raw recording exists from a night-quiet room, so a close-range recording scaled down
stands in for one: out = round(in * gain) as int16 (round half to even), nothing else. Speech and
room noise are scaled together, so the proxy's room is quieter than a real quiet room and its
samples carry the coarser quantisation of the lower level. Sample positions do not change, so the
original recording's score.json (alignment, manifest, played_seconds) stays valid for the proxy.

"Speech level" is the 90th percentile of the segmenter's own frame level (10 ms, mean removed,
floor(sqrt), stt_segmenter.h evaluate_frame) over the frames inside the playlist's speech clips.
--db applies a fixed attenuation; --target-p90 picks the gain that puts that percentile at the
target. Levels before and after are printed; a sidecar JSON is written next to the output.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

SR = 16000
F = 160
NO_SPEECH = ('noise', 'silence')


def frame_levels(x):
    """floor(sqrt(variance)) per 160-sample frame, the segmenter's current_rms."""
    n = x.size // F
    f = x[:n * F].astype(np.int64).reshape(n, F)
    v = ((f * f).sum(1) * F - f.sum(1) ** 2) // (F * F)
    lv = np.floor(np.sqrt(v.astype(np.float64))).astype(np.int64)
    lv[lv * lv > v] -= 1
    lv[(lv + 1) * (lv + 1) <= v] += 1
    return lv


def make_to_raw(fit):
    """playlist sample -> raw sample (inverse of tools/sttraw_ab/score.py make_to_src)."""
    if fit.get('mode') == 'envelope' and len(fit.get('anchors_raw', [])) >= 2:
        ra, sa, slope = np.array(fit['anchors_raw']), np.array(fit['anchors_src']), fit['slope']

        def to_raw(s):
            if s <= sa[0]:
                return ra[0] + (s - sa[0]) * slope
            if s >= sa[-1]:
                return ra[-1] + (s - sa[-1]) * slope
            return float(np.interp(s, sa, ra))
        return to_raw
    return lambda s: fit['offset'] + fit['slope'] * s


def levels(x, items, to_raw):
    lv = frame_levels(x)
    pick = lambda keep: np.concatenate([lv[max(0, int(to_raw(it['start_sample'])) // F):max(0, int(to_raw(it['end_sample'])) // F)]
                                        for it in items if keep(it['category'])] or [np.zeros(0, np.int64)])
    speech, silence = pick(lambda c: c not in NO_SPEECH), pick(lambda c: c == 'silence')
    pct = lambda a, q: float(np.percentile(a, q)) if a.size else float('nan')
    return dict(speech_frames=int(speech.size), speech_p10=pct(speech, 10), speech_p50=pct(speech, 50), speech_p90=pct(speech, 90),
                speech_share_at_or_above_45=round(float(np.mean(speech >= 45)), 4) if speech.size else float('nan'),
                silence_item_p25=pct(silence, 25), silence_item_p50=pct(silence, 50), silence_item_p90=pct(silence, 90),
                peak_abs=int(np.abs(x.astype(np.int32)).max()))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--score', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--raw', type=Path, help='default: the score.json raw path')
    ap.add_argument('--manifest', type=Path, help='default: the score.json manifest path')
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--db', type=float, help='attenuation in dB, e.g. -12')
    g.add_argument('--target-p90', type=float, help='speech-frame p90 to reach, in int16 counts')
    args = ap.parse_args()

    score = json.loads(args.score.read_text())
    raw = args.raw or Path(score['raw'])
    manifest = json.loads((args.manifest or Path(score['manifest'])).read_text())
    played = int(score['played_seconds'] * SR)
    items = [it for it in manifest['clips'] if it['end_sample'] <= played]
    to_raw = make_to_raw(score['alignment'])

    x = np.fromfile(raw, '<i2')
    before = levels(x, items, to_raw)
    gain = 10 ** (args.db / 20) if args.db is not None else args.target_p90 / before['speech_p90']
    y = np.round(x.astype(np.float64) * gain)
    if np.abs(y).max() > 32767:
        sys.exit('the scaled audio would clip')
    y = y.astype('<i2')
    after = levels(y, items, to_raw)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    y.tofile(args.out)
    sidecar = raw.with_suffix('.json')
    meta = json.loads(sidecar.read_text()) if sidecar.exists() else dict(format='s16le', channels=1, rate=SR, samples_dropped=0)
    meta.update(proxy_of=str(raw), proxy_gain=gain, proxy_db=round(20 * np.log10(gain), 3), proxy_levels_before=before, proxy_levels_after=after)
    args.out.with_suffix('.json').write_text(json.dumps(meta, separators=(',', ':')) + '\n')
    print(json.dumps(dict(out=str(args.out), samples=int(y.size), gain=round(gain, 6), db=round(20 * np.log10(gain), 3), before=before, after=after), indent=1))


if __name__ == '__main__':
    main()
