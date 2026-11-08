"""Caption-score speech-detector arms on one sttraw recording from the P4.

  score.py --raw raw_x.pcm --manifest ab1.json --out DIR [--arm NAME="REPLAY OPTIONS"]...
           [--played-seconds S] [--replay /Volumes/hw1build/stt_raw_replay] [--source-baseline]

1. Alignment. The playlist WAV is located in the raw recording by cross-correlation:
   - a coarse search over the start of the recording;
   - local refinement of one window per 30 s of programme;
   - a linear fit (offset + clock drift).
   The fit and each window's residual are reported, and windows off by more than 20 ms are
   dropped.
2. Every arm is replayed through HardwareOne's production segmenter (stt_raw_replay; overrides
   as given, e.g. "--speech-floor 36"). The default arm "production" uses no overrides.
3. One host decoder decodes every segment, for every arm:
   - the deployed int8 QuartzNet export plus the word LM meeting.lm, i.e. the HW1LM1 host
     reference (caption_wer.py, same normalisation);
   - on raw[start : min(end, voiced_end + 500 ms)], which is what the device decodes.
   It is the same for all arms, so differences between arms come only from the detector. It is
   not bit-identical to the device's own captions (streaming, speculative drafts).
4. Each segment goes to the playlist item it overlaps most. A 0.5 s collar applies to speech
   clips only; noise and silence items are scored without a collar.
   - Speech clips get the joined text of their segments.
   - Text on noise or silence items, or on no item at all, counts as false captions.
5. Report per arm and category:
   - WER with substitutions, deletions and insertions, and clips with no caption;
   - reference time outside every segment;
   - false-caption segments and words on noise and on silence;
   - segments (recognition jobs) per minute.
   With --source-baseline, the same decoder on the clean source clips gives the ceiling.
Writes DIR/score.json and DIR/score.md.
"""
from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
import wave
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from overlap_gate.caption_wer import norm, edit_ops, LM_PATH  # noqa: E402

SR = 16000
REPLAY = Path('/Volumes/hw1build/stt_raw_replay')
TAIL = SR // 2            # the device decodes a phrase up to its voiced end + 500 ms
COLLAR = SR // 2
NO_SPEECH = ('noise', 'silence')


def read_wav(path):
    with wave.open(str(path)) as w:
        return np.frombuffer(w.readframes(w.getnframes()), '<i2').astype(np.float32)


def xcorr(a, b):
    """Full cross-correlation c[k] = sum_t a[t + k] b[t] for k in [0, len(a) - len(b)]."""
    n = 1 << int(np.ceil(np.log2(a.size + b.size)))
    c = np.fft.irfft(np.fft.rfft(a, n) * np.conj(np.fft.rfft(b, n)), n)
    return c[:a.size - b.size + 1]


def align(raw, src, played):
    """raw_sample = offset + (1 + drift) * src_sample, fitted on 4 s windows of programme."""
    first = next(i for i in range(0, played - SR, SR // 10) if np.abs(src[i:i + SR]).max() > 100)
    w0 = src[first:first + 8 * SR]
    coarse = int(np.argmax(xcorr(raw[:min(raw.size, first + 120 * SR)], w0))) - first
    fits = []
    for s in range(first, played - 4 * SR, 30 * SR):
        w = src[s:s + 4 * SR]
        if np.sqrt(np.mean(w ** 2)) < 30:
            continue
        lo = max(0, coarse + s - SR)
        seg = raw[lo:lo + 6 * SR]
        if seg.size < w.size:
            break
        c = xcorr(seg, w)
        fits.append((s, lo + int(np.argmax(c)), float(c.max() / (np.linalg.norm(seg) * np.linalg.norm(w) + 1e-9))))
    s, r = np.array([f[0] for f in fits], float), np.array([f[1] for f in fits], float)
    # Start from the median offset, not a least-squares line: one or two windows that locked onto the wrong
    # place (a bump, the room, a repeated phrase) pull the first fit seconds away and the 20 ms sieve then
    # keeps nothing (seen on 2026-11-07 x1/x4 recordings). Keep windows within 0.5 s of the median first.
    keep = np.abs((r - s) - np.median(r - s)) <= 0.5 * SR
    if keep.sum() < 2:
        keep = np.ones(s.size, bool)
    for _ in range(3):
        p = np.polyfit(s[keep], r[keep], 1)
        res = r - np.polyval(p, s)
        nxt = np.abs(res) <= 0.02 * SR
        if nxt.sum() < 2:
            break
        keep = nxt
    p = np.polyfit(s[keep], r[keep], 1)
    return dict(offset=float(p[1]), slope=float(p[0]), drift_ppm=round((p[0] - 1) * 1e6, 1), windows=len(fits), kept=int(keep.sum()),
                max_residual_ms=round(float(np.abs(r[keep] - np.polyval(p, s[keep])).max()) / SR * 1000, 2), coarse=coarse)


HOP = 160  # 10 ms envelope frames


def envelope(x):
    n = x.size // HOP
    f = x[:n * HOP].astype(np.float64).reshape(n, HOP)
    return np.log(1.0 + np.sqrt((f ** 2).mean(1)))


def _ncc(a, b):
    a = a - a.mean()
    b = b - b.mean()
    return float((a * b).sum() / (np.linalg.norm(a) * np.linalg.norm(b) + 1e-9))


def align_envelope(raw, src, played):
    """Piecewise alignment on the 10 ms log-loudness envelope, for sources that are not sample-locked to the
    playlist: the G2 glasses mic reaches STT as LC3 over BLE, after the glasses' own AGC, so the waveform no
    longer correlates with the source, and lost or concealed packets shift the timeline (0.7% over 10 min on
    2026-11-08). Anchors every 15 s of programme (8 s windows, searched +-2 s around the running offset);
    anchors with low correlation or a jump of more than 0.5 s against their neighbours are dropped; raw ->
    programme positions are interpolated between anchors."""
    er, es = envelope(raw), envelope(src[:played])
    first = next(i for i in range(0, es.size - 100, 10) if es[i:i + 100].max() > np.log(1 + 100))
    w0 = es[first:first + 6000] - es[first:first + 6000].mean()
    search = er[:min(er.size, first + 12000)]
    c = xcorr(search - search.mean(), w0)
    lag = int(np.argmax(c)) - first          # raw frame = src frame + lag
    anchors = []
    for s in range(first, es.size - 800, 1500):
        w = es[s:s + 800]
        if w.max() < np.log(1 + 100):
            continue
        lo = max(0, s + lag - 200)
        seg = er[lo:lo + 1200]
        if seg.size < w.size:
            break
        best, at = -1.0, 0
        for k in range(0, seg.size - w.size + 1):
            v = _ncc(seg[k:k + w.size], w)
            if v > best:
                best, at = v, k
        if best < 0.5:
            continue
        r = lo + at
        lag = r - s                          # follow the drift
        anchors.append((s, r, best))
    s_a = np.array([a[0] for a in anchors], float)
    r_a = np.array([a[1] for a in anchors], float)
    keep = np.ones(s_a.size, bool)
    d = r_a - s_a
    for i in range(s_a.size):
        nb = d[max(0, i - 2):i + 3]
        keep[i] = abs(d[i] - np.median(nb)) <= 50
    s_a, r_a = s_a[keep] * HOP, r_a[keep] * HOP
    slope = float(np.polyfit(s_a, r_a, 1)[0]) if s_a.size >= 2 else 1.0
    return dict(mode='envelope', anchors_src=s_a.tolist(), anchors_raw=r_a.tolist(), windows=len(anchors), kept=int(keep.sum()),
                offset=float(r_a[0] - s_a[0] * slope) if s_a.size else 0.0, slope=slope,
                drift_ppm=round((slope - 1) * 1e6, 1), max_residual_ms=None, coarse=lag * HOP)


def make_to_src(fit):
    """raw sample -> programme sample for either fit."""
    if fit.get('mode') == 'envelope' and len(fit.get('anchors_raw', [])) >= 2:
        ra, sa, slope = np.array(fit['anchors_raw']), np.array(fit['anchors_src']), fit['slope']

        def to_src(r):
            if r <= ra[0]:
                return sa[0] + (r - ra[0]) / slope
            if r >= ra[-1]:
                return sa[-1] + (r - ra[-1]) / slope
            return float(np.interp(r, ra, sa))
        return to_src
    return lambda r: (r - fit['offset']) / fit['slope']


def replay(raw_path, options, sidecar=None, exe=REPLAY):
    cmd = [str(exe), str(raw_path)] + (['--sidecar', str(sidecar)] if sidecar else []) + shlex.split(options)
    out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    return json.loads(out)


SPEECH_MIN = int(0.3 * SR)  # a segment overlapping speech this much is that speech's caption


def assign(segments, items, to_src):
    """Map each segment (raw samples) to a playlist item (src samples): the speech clip it overlaps
    most if it overlaps one by at least SPEECH_MIN (a phrase that opened early in a silence or noise
    item is still that clip's caption, not a false one), otherwise the item it overlaps most."""
    out = []
    for g in segments:
        a, b = to_src(g['start']), to_src(g['end'])
        best, cover, speech, speech_cover = None, 0.0, None, 0.0
        for it in items:
            pad = 0 if it['category'] in NO_SPEECH else COLLAR
            ov = min(b, it['end_sample'] + pad) - max(a, it['start_sample'] - pad)
            if ov > cover:
                best, cover = it, ov
            if it['category'] not in NO_SPEECH:
                core = min(b, it['end_sample']) - max(a, it['start_sample'])
                if core >= SPEECH_MIN and ov > speech_cover:
                    speech, speech_cover = it, ov
        out.append(speech or best)
    return out


def score_arm(segments, texts, items, owner, to_src, minutes):
    rows, cats = {}, {}
    hyp = {it['index']: [] for it in items}
    false = dict(noise=[0, 0], silence=[0, 0], unassigned=[0, 0])
    for g, t, it in zip(segments, texts, owner):
        words = norm(t)
        if it is None or it['category'] in NO_SPEECH:
            key = 'unassigned' if it is None else it['category']
            if words:
                false[key][0] += 1
                false[key][1] += len(words)
        else:
            hyp[it['index']].append(t)
    cov = []
    for g in segments:
        cov.append((to_src(g['start']), to_src(g['end'])))
    for it in items:
        if it['category'] in NO_SPEECH:
            continue
        ref, h = norm(it['reference']), norm(' '.join(hyp[it['index']]))
        s, d, i = edit_ops(ref, h)
        n = it['end_sample'] - it['start_sample']
        inside = sum(max(0.0, min(b, it['end_sample']) - max(a, it['start_sample'])) for a, b in cov)
        c = cats.setdefault(it['category'], dict(clips=0, words=0, S=0, D=0, I=0, empty=0, ref_s=0.0, outside_s=0.0))
        c['clips'] += 1
        c['words'] += len(ref)
        c['S'] += s
        c['D'] += d
        c['I'] += i
        c['empty'] += not h
        c['ref_s'] += n / SR
        c['outside_s'] += max(0.0, n - min(inside, n)) / SR
        rows[it['index']] = dict(category=it['category'], ref=' '.join(ref), hyp=' '.join(h), S=s, D=d, I=i)
    tot = dict(clips=0, words=0, S=0, D=0, I=0, empty=0, ref_s=0.0, outside_s=0.0)
    for c in cats.values():
        for k in tot:
            tot[k] += c[k]
        c['wer'] = round(100 * (c['S'] + c['D'] + c['I']) / max(1, c['words']), 2)
    tot['wer'] = round(100 * (tot['S'] + tot['D'] + tot['I']) / max(1, tot['words']), 2)
    return dict(categories=cats, total=tot, false_captions=false, segments=len(segments),
                jobs_per_minute=round(len(segments) / minutes, 2), clips=rows)


def table(result):
    arms = result['arms']
    cats = sorted({c for a in arms.values() for c in a['categories']})
    lines = ['| arm | segments/min | ' + ' | '.join(f'{c} WER (D) empty' for c in cats) + ' | all WER | false on noise | false on silence | unassigned |',
             '|---' * (len(cats) + 6) + '|']
    for name, a in arms.items():
        cells = []
        for c in cats:
            x = a['categories'].get(c)
            cells.append(f"{x['wer']:.1f} ({100 * x['D'] / max(1, x['words']):.1f}) {x['empty']}/{x['clips']}" if x else '')
        f = a['false_captions']
        lines.append(f"| {name} | {a['jobs_per_minute']} | " + ' | '.join(cells) + f" | {a['total']['wer']:.1f} | {f['noise'][0]} seg / {f['noise'][1]} w"
                     f" | {f['silence'][0]} / {f['silence'][1]} | {f['unassigned'][0]} / {f['unassigned'][1]} |")
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--raw', type=Path, required=True)
    ap.add_argument('--sidecar', type=Path)
    ap.add_argument('--manifest', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--arm', action='append', default=[], help='NAME=REPLAY OPTIONS, e.g. floor36="--speech-floor 36"')
    ap.add_argument('--played-seconds', type=float, help='only part of the playlist was played')
    ap.add_argument('--replay', type=Path, default=REPLAY)
    ap.add_argument('--source-baseline', action='store_true')
    ap.add_argument('--align', choices=('waveform', 'envelope'), default='waveform',
                    help='envelope: piecewise on the loudness envelope (G2 glasses mic over BLE)')
    args = ap.parse_args()
    sys.path.insert(0, str(ROOT / 'tools/stt_train/lm'))
    import hw1lm
    from overlap_gate.g0_extract import Recognizer
    rec = Recognizer(ROOT / 'private/training-track/overlap-gate/phase4/caption-cache')
    lm = hw1lm.read_lm(LM_PATH)

    def decode(x):
        return hw1lm.decode_int8(rec.logits(x.astype('<i2')), rec.exponent, lm) if x.size >= 320 else ''

    m = json.loads(args.manifest.read_text())
    raw16 = np.fromfile(args.raw, '<i2')
    raw, src = raw16.astype(np.float32), read_wav(m['wav'])
    played = int((args.played_seconds or src.size / SR) * SR)
    items = [it for it in m['clips'] if it['end_sample'] <= played]
    fit = align_envelope(raw, src, played) if args.align == 'envelope' else align(raw, src, played)
    to_src = make_to_src(fit)
    minutes = raw.size / SR / 60
    arms = {'production': ''}
    for a in args.arm:
        k, v = a.split('=', 1)
        arms[k] = v
    out = dict(raw=str(args.raw), manifest=str(args.manifest), played_seconds=played / SR, alignment=fit,
               decoder='int8 QuartzNet export gen2-25397 + meeting.lm (HW1LM1 host reference), CTC prefix beam search', arms={})
    args.out.mkdir(parents=True, exist_ok=True)
    for name, opts in arms.items():
        r = replay(args.raw, opts, args.sidecar, args.replay)
        segs = r['segments']
        texts = [decode(raw16[g['start']:min(g['end'], g['voiced_end'] + TAIL)]) for g in segs]
        res = score_arm(segs, texts, items, assign(segs, items, to_src), to_src, minutes)
        res.update(options=opts, config=r['config'], texts=texts)
        out['arms'][name] = res
        print(name, json.dumps(dict(segments=res['segments'], wer=res['total']['wer'], false=res['false_captions'])), flush=True)
    if args.source_baseline:
        base = {}
        for it in items:
            if it['category'] in NO_SPEECH:
                continue
            ref = norm(it['reference'])
            s, d, i = edit_ops(ref, norm(decode(src[it['start_sample']:it['end_sample']].astype(np.int16))))
            c = base.setdefault(it['category'], dict(words=0, S=0, D=0, I=0))
            c['words'] += len(ref)
            c['S'] += s
            c['D'] += d
            c['I'] += i
        for c in base.values():
            c['wer'] = round(100 * (c['S'] + c['D'] + c['I']) / max(1, c['words']), 2)
        out['source_baseline'] = base
    (args.out / 'score.json').write_text(json.dumps(out) + '\n')
    md = [f"# sttraw A/B score: {args.raw.name}", '',
          f"Alignment: offset {fit['offset']:.0f} samples, drift {fit['drift_ppm']} ppm, {fit['kept']}/{fit['windows']} windows, max residual {fit['max_residual_ms']} ms.",
          f"Decoder: {out['decoder']}. Cells: WER % (deletions %) clips with no caption.", '', table(out)]
    if args.source_baseline:
        md += ['', 'Same decoder on the clean source clips: ' + ', '.join(f"{k} {v['wer']:.1f}%" for k, v in sorted(out['source_baseline'].items()))]
    (args.out / 'score.md').write_text('\n'.join(md) + '\n')
    print('\n'.join(md))


if __name__ == '__main__':
    main()
