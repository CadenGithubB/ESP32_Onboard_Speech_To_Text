"""Build the speech-detector A/B playlist for sttraw recordings on the P4.

One ~11-minute 16 kHz mono WAV with a manifest that gives each item's exact start and end
sample, category and reference text. Clips are taken, already levelled, from the night-1
overlap-gate playlist blocks (night_playlists.py; evaluation sources only):
  ls-test     LibriSpeech test-clean, -26 dBFS                               read speech
  ls-quiet    other LibriSpeech test-clean clips, 18 dB quieter (-44 dBFS)   quiet read speech
  syn-ord     synthetic qualification single speaker, -26 dBFS
  syn-quiet   the same suite at -18 dB (-44 dBFS)
  ami-one     AMI validation/test distant-mic windows without overlap
  ami-two     AMI validation/test distant-mic windows with overlap
  noise       DEMAND test noise, 10 s at -40 dBFS                            no speech
  silence     digital silence: only the room reaches the mic                no speech
Each clip is followed by 2.5 s of silence; there are three 20 s silences (start, middle,
end). Selection and order are seeded, so the file is reproducible. Writes
/Volumes/Audiodrive/stt/sttraw/playlists/ab1.wav and its manifest ab1.json (same clip fields as
the night-1 manifests, which tools/sttraw_ab/score.py reads).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import random
import wave
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
NIGHT1 = ROOT / 'private/training-track/overlap-gate/phase4/night1'
OUT = Path('/Volumes/Audiodrive/stt/sttraw/playlists')
SR = 16000
SEED = 20261107
LEAD_S, GAP_S, SILENCE_S = 5.0, 2.5, 20.0
QUIET_DB = -18.0
PLAN = {'ls-test': 10, 'ls-quiet': 8, 'syn-ord': 4, 'syn-quiet': 8, 'ami-one': 16, 'ami-two': 6, 'noise': 6}


def read_wav(path):
    with wave.open(str(path)) as w:
        assert w.getframerate() == SR and w.getnchannels() == 1 and w.getsampwidth() == 2
        return np.frombuffer(w.readframes(w.getnframes()), '<i2')


def pool():
    """Night-1 clips by playlist category, with their already levelled samples."""
    by = {}
    for f in sorted(NIGHT1.glob('block*.json')):
        m = json.loads(f.read_text())
        audio = read_wav(m['wav'])
        for c in m['clips']:
            cat = c['category']
            if cat in ('ami-val', 'ami-test'):
                cat = 'ami-two' if c['label'] else 'ami-one'
            if cat not in ('ls-test', 'syn-ord', 'syn-quiet', 'ami-one', 'ami-two', 'noise'):
                continue
            item = {k: v for k, v in c.items() if k not in ('index', 'start_sample', 'end_sample', 'category')}
            item.update(category=cat, night1_block=m['block'], night1_index=c['index'],
                        pcm=audio[c['start_sample']:c['end_sample']].copy())
            by.setdefault(cat, []).append(item)
    return by


def build(by, rng):
    picks = []
    for cat, n in PLAN.items():
        src = 'ls-test' if cat == 'ls-quiet' else cat
        cands = [c for c in by[src] if not c.get('_used')]
        cands.sort(key=lambda c: (c['night1_block'], c['night1_index']))
        rng.shuffle(cands)
        if cat.startswith('ami'):
            cands = [c for c in cands if 2.0 <= c['pcm'].size / SR <= 12.0]
        assert len(cands) >= n, (cat, len(cands))
        for c in cands[:n]:
            c['_used'] = True
            item = dict(c)
            if cat == 'ls-quiet':
                g = 10 ** (QUIET_DB / 20)
                item['pcm'] = np.clip(np.rint(item['pcm'].astype(np.float64) * g), -32768, 32767).astype('<i2')
                item['gain_db'] = round(item.get('gain_db', 0.0) + QUIET_DB, 2)
                item['category'] = 'ls-quiet'
            picks.append(item)
    rng.shuffle(picks)
    half = len(picks) // 2
    items = [dict(category='silence', seconds=SILENCE_S)] + picks[:half] + [dict(category='silence', seconds=SILENCE_S)] \
        + picks[half:] + [dict(category='silence', seconds=SILENCE_S)]
    parts, pos, clips = [np.zeros(int(LEAD_S * SR), '<i2')], int(LEAD_S * SR), []
    for k, it in enumerate(items):
        x = it['pcm'] if 'pcm' in it else np.zeros(int(it['seconds'] * SR), '<i2')
        entry = {key: v for key, v in it.items() if key not in ('pcm', '_used')}
        entry.update(index=k, start_sample=pos, end_sample=pos + x.size, seconds=round(x.size / SR, 3),
                     reference=it.get('reference', ''), label=bool(it.get('label', False)))
        clips.append(entry)
        parts.append(x)
        pos += x.size
        if it['category'] != 'silence':
            parts.append(np.zeros(int(GAP_S * SR), '<i2'))
            pos += int(GAP_S * SR)
    return np.concatenate(parts), clips


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', type=Path, default=OUT)
    ap.add_argument('--name', default='ab1')
    args = ap.parse_args()
    audio, clips = build(pool(), random.Random(SEED))
    args.out.mkdir(parents=True, exist_ok=True)
    wav = args.out / f'{args.name}.wav'
    with wave.open(str(wav), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(audio.tobytes())
    manifest = dict(name=args.name, wav=str(wav), wav_sha256=hashlib.sha256(wav.read_bytes()).hexdigest(), sample_rate=SR,
                    seconds=round(audio.size / SR, 2), lead_s=LEAD_S, gap_s=GAP_S, seed=SEED, plan=PLAN, quiet_db=QUIET_DB,
                    scope='play through the Mac mini speaker at the bench volume into the P4 with sttraw on', clips=clips)
    (args.out / f'{args.name}.json').write_text(json.dumps(manifest) + '\n')
    cats = {}
    for c in clips:
        cats[c['category']] = cats.get(c['category'], 0) + 1
    print(json.dumps(dict(wav=str(wav), seconds=manifest['seconds'], items=len(clips), categories=cats)))


if __name__ == '__main__':
    main()
