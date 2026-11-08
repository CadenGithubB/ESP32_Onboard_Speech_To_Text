"""Build the bright-noise playlist (nb1) for sttraw recordings on the P4.

The pre-emphasis onset judge (sttdecisionpre) makes the detector more sensitive to high-frequency
energy. ab1's noise is mostly bass-heavy rooms, so this playlist checks the opposite case:
  noise        noise only, 10 s each, no speech expected (manifest field noise_kind says which):
               white hiss at -40 and -32 dBFS, pink noise at -36 dBFS, the six brightest DEMAND test
               environments (two clips each, -36 dBFS) and the four brightest MUSAN noise clips (-36 dBFS)
  ls-hiss      LibriSpeech test-clean at -26 dBFS over white hiss at -44 dBFS (speech still captioned?)
  ls-cafe      LibriSpeech test-clean at -26 dBFS over DEMAND cafeteria babble at -38 dBFS
  silence      three 20 s digital silences (only the room reaches the mic)
Brightness = spectral centroid of the clip. Each item is followed by 2.5 s of silence. Seeded, so the
file is reproducible. Writes /Volumes/Audiodrive/stt/sttraw/playlists/nb1.wav and nb1.json in the
same manifest format as ab1 (tools/sttraw_ab/score.py reads it).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import random
import wave
from pathlib import Path

import numpy as np

import make_playlist as mp

STORES = Path('/Volumes/Audiodrive/stt/stores')
SR = mp.SR
SEED = 20261108
NOISE_S = 10.0


def store(name):
    ix = json.loads((STORES / name / 'index.json').read_text())
    audio = np.memmap(STORES / name / 'audio.bin', dtype='<i2', mode='r')
    return ix, audio


def centroid(x):
    spec = np.abs(np.fft.rfft(x.astype(np.float64) * np.hanning(x.size))) ** 2
    f = np.fft.rfftfreq(x.size, 1 / SR)
    return float((spec * f).sum() / max(spec.sum(), 1e-12))


def level(x, dbfs):
    x = x.astype(np.float64)
    rms = np.sqrt(np.mean(x ** 2)) or 1.0
    return x * (32768 * 10 ** (dbfs / 20) / rms)


def to_i16(x):
    return np.clip(np.rint(x), -32768, 32767).astype('<i2')


def coloured(rng, kind, n):
    w = rng.standard_normal(n)
    if kind == 'white':
        return w
    # pink: 1/f power by shaping the spectrum
    spec = np.fft.rfft(w)
    f = np.fft.rfftfreq(n, 1 / SR)
    spec[1:] /= np.sqrt(f[1:])
    spec[0] = 0
    return np.fft.irfft(spec, n)


def noise_items(rng):
    n = int(NOISE_S * SR)
    items = [dict(noise_kind='white', dbfs=-40.0, x=level(coloured(rng, 'white', n), -40)),
             dict(noise_kind='white', dbfs=-32.0, x=level(coloured(rng, 'white', n), -32)),
             dict(noise_kind='pink', dbfs=-36.0, x=level(coloured(rng, 'pink', n), -36))]
    ix, audio = store('demand-noise-test')
    by = {}
    for k, (o, ln, env) in enumerate(zip(ix['offsets'], ix['lengths'], ix['speakers'])):
        x = np.asarray(audio[o:o + min(ln, n)])
        by.setdefault(env, []).append((k, x, centroid(x)))
    envs = sorted(by, key=lambda e: -np.mean([c for _, _, c in by[e]]))[:6]
    for env in envs:
        for k, x, c in sorted(by[env], key=lambda t: -t[2])[:2]:
            items.append(dict(noise_kind=f'demand-{env}', source=f'demand-noise-test:{k}', centroid_hz=round(c),
                              dbfs=-36.0, x=level(x, -36)))
    ix, audio = store('musan-noise')
    cands = [k for k, t in enumerate(ix['texts']) if t == 'noise']
    rng.shuffle(cands)
    scored = []
    for k in cands[:80]:
        o, ln = ix['offsets'][k], ix['lengths'][k]
        x = np.asarray(audio[o:o + min(ln, n)])
        if x.size < n or np.sqrt(np.mean(x.astype(np.float64) ** 2)) < 30:
            continue
        scored.append((centroid(x), k, x))
    for c, k, x in sorted(scored, key=lambda t: -t[0])[:4]:
        items.append(dict(noise_kind='musan-noise', source=f'musan-noise:{k}', centroid_hz=round(c), dbfs=-36.0,
                          x=level(x, -36)))
    return items, by


def speech_items(rng, demand_by):
    pool = [c for c in mp.pool()['ls-test']]
    pool.sort(key=lambda c: (c['night1_block'], c['night1_index']))
    rng.shuffle(pool)
    cafe = [x for _, x, _ in demand_by['PCAFETER']]
    out = []
    for k, c in enumerate(pool[:14]):
        s = level(c['pcm'], -26)
        if k < 8:
            bed, cat, ndb = level(coloured(rng, 'white', s.size), -44), 'ls-hiss', -44.0
        else:
            src = cafe[k % len(cafe)]
            start = int(rng.integers(0, max(1, src.size - s.size)))
            seg = np.resize(src[start:start + s.size], s.size)
            bed, cat, ndb = level(seg, -38), 'ls-cafe', -38.0
        item = {key: v for key, v in c.items() if key not in ('pcm', '_used', 'category')}
        item.update(category=cat, noise_dbfs=ndb, x=s + bed)
        out.append(item)
    return out


def build(rng):
    noise, demand_by = noise_items(rng)
    for it in noise:
        it['category'] = 'noise'
    picks = noise + speech_items(rng, demand_by)
    rng.shuffle(picks)
    half = len(picks) // 2
    items = [dict(category='silence', seconds=20.0)] + picks[:half] + [dict(category='silence', seconds=20.0)] \
        + picks[half:] + [dict(category='silence', seconds=20.0)]
    parts, pos, clips = [np.zeros(int(mp.LEAD_S * SR), '<i2')], int(mp.LEAD_S * SR), []
    for k, it in enumerate(items):
        x = to_i16(it['x']) if 'x' in it else np.zeros(int(it['seconds'] * SR), '<i2')
        entry = {key: v for key, v in it.items() if key != 'x'}
        entry.update(index=k, start_sample=pos, end_sample=pos + x.size, seconds=round(x.size / SR, 3),
                     reference=it.get('reference', ''), label=bool(it.get('label', False)))
        clips.append(entry)
        parts.append(x)
        pos += x.size
        if it['category'] != 'silence':
            parts.append(np.zeros(int(mp.GAP_S * SR), '<i2'))
            pos += int(mp.GAP_S * SR)
    return np.concatenate(parts), clips


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', type=Path, default=mp.OUT)
    ap.add_argument('--name', default='nb1')
    args = ap.parse_args()
    audio, clips = build(_Rng(SEED))
    args.out.mkdir(parents=True, exist_ok=True)
    wav = args.out / f'{args.name}.wav'
    with wave.open(str(wav), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(audio.tobytes())
    manifest = dict(name=args.name, wav=str(wav), wav_sha256=hashlib.sha256(wav.read_bytes()).hexdigest(), sample_rate=SR,
                    seconds=round(audio.size / SR, 2), lead_s=mp.LEAD_S, gap_s=mp.GAP_S, seed=SEED,
                    scope='bright-noise check for the pre-emphasis onset judge; play like ab1', clips=clips)
    (args.out / f'{args.name}.json').write_text(json.dumps(manifest) + '\n')
    cats = {}
    for c in clips:
        cats[c['category']] = cats.get(c['category'], 0) + 1
    kinds = sorted({c.get('noise_kind') for c in clips if c.get('noise_kind')})
    print(json.dumps(dict(wav=str(wav), seconds=manifest['seconds'], items=len(clips), categories=cats, noise_kinds=kinds)))


class _Rng:
    """numpy Generator plus random.Random-style shuffle, both from one seed."""
    def __init__(self, seed):
        self.g = np.random.default_rng(seed)
        self.r = random.Random(seed)

    def standard_normal(self, n):
        return self.g.standard_normal(n)

    def integers(self, lo, hi):
        return self.g.integers(lo, hi)

    def shuffle(self, x):
        self.r.shuffle(x)


if __name__ == '__main__':
    main()
