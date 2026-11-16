#!/usr/bin/env python3
"""Speech coverage of segmenter arms on one scored sttraw recording. No decoding.

  coverage.py --score score.json ARM.json [ARM2.json ...] [--name A,B] [--json out.json]
  coverage.py --score score.json --replay segmenter_replay [--raw raw.pcm]
              --arm base="" --arm floor24="--set adaptive_speech_floor=24" [--check production]

--score is a score.json written by tools/sttraw_ab/score.py: it names the playlist manifest and holds
the alignment (raw sample <-> playlist sample) and played_seconds. ARM.json is the output of
tools/segmenter_replay/segmenter_replay (or HardwareOne's stt_raw_replay). With --replay the tool
runs the binary itself on the score's raw file (or --raw), once per --arm NAME="OPTIONS".

Reported per arm:
  speech    playlist speech seconds outside every segment, total and per category (the same sum as
            score.py's outside_s: clip length minus its overlap with all segments, in playlist
            samples), clips with no segment at all and clips less than 90% covered
  nonspeech seconds of the noise and silence playlist items inside segments, and the segments owned
            by a noise item, a silence item or no item (score.py's assign(): these are the segments
            whose text would count as a false caption)
  load      segments, segments per minute of recording, hard-limit cuts, seconds inside segments and
            seconds handed to the recognizer (start .. decode_end, i.e. voiced end + tail)
Every later arm is also shown as a difference from the first.

--check ARM compares the first arm with that arm of the score.json (segment count and outside
seconds per category) and exits 1 on a mismatch: use it to prove a replay reproduces a stored score.
"""
from __future__ import annotations

import argparse
import bisect
import json
import shlex
import subprocess
import sys
from pathlib import Path

SR = 16000
TAIL = SR // 2            # score.py TAIL: used only when a segment has no decode_end
COLLAR = SR // 2          # score.py COLLAR
SPEECH_MIN = int(0.3 * SR)  # score.py SPEECH_MIN
NO_SPEECH = ('noise', 'silence')


def make_to_src(fit):
    """raw sample -> playlist sample (tools/sttraw_ab/score.py make_to_src, both alignment modes)."""
    if fit.get('mode') == 'envelope' and len(fit.get('anchors_raw', [])) >= 2:
        ra, sa, slope = fit['anchors_raw'], fit['anchors_src'], fit['slope']

        def to_src(r):
            if r <= ra[0]:
                return sa[0] + (r - ra[0]) / slope
            if r >= ra[-1]:
                return sa[-1] + (r - ra[-1]) / slope
            i = bisect.bisect_right(ra, r) - 1
            return sa[i] + (r - ra[i]) * (sa[i + 1] - sa[i]) / (ra[i + 1] - ra[i])
        return to_src
    return lambda r: (r - fit['offset']) / fit['slope']


def assign(spans, items):
    """score.py assign(): the speech clip a segment overlaps most (0.5 s collar) if it overlaps one by
    at least SPEECH_MIN, otherwise the item it overlaps most; None when it overlaps nothing."""
    out = []
    for a, b in spans:
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


def measure(replay, items, to_src):
    segs = replay['segments']
    spans = [(to_src(g['start']), to_src(g['end'])) for g in segs]
    owner = assign(spans, items)
    cats, clips = {}, {}
    nonspeech = {k: dict(items=0, item_s=0.0, inside_s=0.0, segments=0) for k in NO_SPEECH}
    for it in items:
        n = it['end_sample'] - it['start_sample']
        inside = sum(max(0.0, min(b, it['end_sample']) - max(a, it['start_sample'])) for a, b in spans)
        inside = min(inside, n)
        if it['category'] in NO_SPEECH:
            c = nonspeech[it['category']]
            c['items'] += 1
            c['item_s'] += n / SR
            c['inside_s'] += inside / SR
        else:
            c = cats.setdefault(it['category'], dict(clips=0, ref_s=0.0, outside_s=0.0, no_segment=0, under_90=0))
            c['clips'] += 1
            c['ref_s'] += n / SR
            c['outside_s'] += max(0.0, n - inside) / SR
            c['no_segment'] += inside <= 0
            c['under_90'] += 0 < inside < 0.9 * n
        clips[it['index']] = dict(category=it['category'], seconds=round(n / SR, 3), inside_s=round(inside / SR, 3),
                                  segments=sum(1 for o in owner if o is it))
    total = dict(clips=0, ref_s=0.0, outside_s=0.0, no_segment=0, under_90=0)
    for c in cats.values():
        for k in total:
            total[k] += c[k]
    for c in list(cats.values()) + [total]:
        c['outside_pct'] = round(100 * c['outside_s'] / c['ref_s'], 2) if c['ref_s'] else 0.0
    unowned = 0
    for o in owner:
        if o is None:
            unowned += 1
        elif o['category'] in NO_SPEECH:
            nonspeech[o['category']]['segments'] += 1
    in_item = sum(v['inside_s'] for v in nonspeech.values()) + sum(c['ref_s'] - c['outside_s'] for c in cats.values())
    covered = sum(g['end'] - g['start'] for g in segs) / SR
    minutes = replay['samples'] / SR / 60
    decode = sum(g.get('decode_end', min(g['end'], g['voiced_end'] + TAIL)) - g['start'] for g in segs) / SR
    lengths = sorted((g['end'] - g['start']) / SR for g in segs)
    return dict(
        speech=dict(total=total, categories=dict(sorted(cats.items()))),
        nonspeech=dict(noise=nonspeech['noise'], silence=nonspeech['silence'], unassigned_segments=unowned,
                       nonspeech_inside_s=nonspeech['noise']['inside_s'] + nonspeech['silence']['inside_s']),
        load=dict(segments=len(segs), minutes=round(minutes, 3), segments_per_minute=round(len(segs) / minutes, 2) if minutes else 0.0,
                  hard_limit_cuts=sum(g.get('end_kind') == 'hard_limit' for g in segs),
                  continuations=sum(bool(g.get('continuation')) for g in segs),
                  segment_s=round(covered, 2), decode_s=round(decode, 2),
                  outside_items_s=round(max(0.0, covered - in_item), 2),
                  median_segment_s=round(lengths[len(lengths) // 2], 2) if lengths else 0.0,
                  max_segment_s=round(lengths[-1], 2) if lengths else 0.0),
        config=replay.get('segmenter_config', replay.get('config')), clips=clips)


FLAT = [  # (label, path into measure()'s result, decimals)
    ('speech outside segments (s)', ('speech', 'total', 'outside_s'), 2),
    ('speech outside segments (%)', ('speech', 'total', 'outside_pct'), 2),
    ('speech clips with no segment', ('speech', 'total', 'no_segment'), 0),
    ('speech clips under 90% covered', ('speech', 'total', 'under_90'), 0),
    ('noise items inside segments (s)', ('nonspeech', 'noise', 'inside_s'), 2),
    ('silence items inside segments (s)', ('nonspeech', 'silence', 'inside_s'), 2),
    ('segments owned by a noise item', ('nonspeech', 'noise', 'segments'), 0),
    ('segments owned by a silence item', ('nonspeech', 'silence', 'segments'), 0),
    ('segments owned by no item', ('nonspeech', 'unassigned_segments'), 0),
    ('segments', ('load', 'segments'), 0),
    ('segments per minute', ('load', 'segments_per_minute'), 2),
    ('hard-limit cuts', ('load', 'hard_limit_cuts'), 0),
    ('seconds inside segments', ('load', 'segment_s'), 2),
    ('seconds sent to the recognizer', ('load', 'decode_s'), 2),
]


def dig(d, path):
    for k in path:
        d = d[k]
    return d


def report(arms, ref_s, nonspeech_s):
    names = list(arms)
    first = arms[names[0]]
    rows = [['measure'] + names + [f'{n} - {names[0]}' for n in names[1:]]]

    def row(label, values, decimals):
        cells = [f'{v:.{decimals}f}' for v in values]
        cells += [f'{v - values[0]:+.{decimals}f}' for v in values[1:]]
        rows.append([label] + cells)
    for label, path, decimals in FLAT:
        row(label, [dig(a, path) for a in arms.values()], decimals)
    cats = sorted({c for a in arms.values() for c in a['speech']['categories']})
    for c in cats:
        row(f'  {c} outside (s) of {first["speech"]["categories"][c]["ref_s"]:.2f}',
            [a['speech']['categories'].get(c, dict(outside_s=0.0))['outside_s'] for a in arms.values()], 2)
    widths = [max(len(r[i]) for r in rows) for i in range(len(rows[0]))]
    lines = [f'playlist speech {ref_s:.2f} s in {first["speech"]["total"]["clips"]} clips; noise + silence items {nonspeech_s:.2f} s; '
             f'recording {first["load"]["minutes"]:.2f} min']
    for r in rows:
        lines.append('  '.join(c.ljust(widths[i]) if i == 0 else c.rjust(widths[i]) for i, c in enumerate(r)))
    return '\n'.join(lines)


def check(result, stored, name):
    """Compare with one arm of the score.json. Returns a list of mismatches."""
    bad = []
    if result['load']['segments'] != stored['segments']:
        bad.append(f"segments {result['load']['segments']} vs stored {stored['segments']}")
    for c, v in stored['categories'].items():
        mine = result['speech']['categories'].get(c)
        if mine is None or abs(mine['outside_s'] - v['outside_s']) > 1e-6 or abs(mine['ref_s'] - v['ref_s']) > 1e-6:
            bad.append(f"{c}: outside {mine and mine['outside_s']} vs stored {v['outside_s']}")
    if abs(result['speech']['total']['outside_s'] - stored['total']['outside_s']) > 1e-6:
        bad.append(f"total outside {result['speech']['total']['outside_s']} vs stored {stored['total']['outside_s']}")
    print(f"check against stored arm '{name}': " + ('MISMATCH: ' + '; '.join(bad) if bad else
          f"segments {stored['segments']} and outside {stored['total']['outside_s']:.2f} s reproduced in every category"))
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--score', type=Path, required=True, help='score.json of the recording (manifest, alignment, played_seconds)')
    ap.add_argument('replays', nargs='*', type=Path, help='segmenter_replay JSON outputs, one per arm')
    ap.add_argument('--name', help='comma-separated arm names for the JSON files (default: file stems)')
    ap.add_argument('--replay', type=Path, help='segmenter_replay binary: run it for every --arm')
    ap.add_argument('--arm', action='append', default=[], help='NAME="REPLAY OPTIONS", e.g. floor24="--set adaptive_speech_floor=24"')
    ap.add_argument('--raw', type=Path, help='raw PCM for --replay (default: the score.json raw path)')
    ap.add_argument('--manifest', type=Path, help='playlist manifest (default: the score.json manifest path)')
    ap.add_argument('--check', metavar='ARM', help="compare the first arm with this arm of the score.json")
    ap.add_argument('--json', type=Path, help='also write every number here')
    args = ap.parse_args()

    score = json.loads(args.score.read_text())
    manifest = json.loads((args.manifest or Path(score['manifest'])).read_text())
    played = int(score['played_seconds'] * SR)
    items = [it for it in manifest['clips'] if it['end_sample'] <= played]
    to_src = make_to_src(score['alignment'])

    replays = {}
    names = args.name.split(',') if args.name else [p.stem for p in args.replays]
    if len(names) != len(args.replays):
        ap.error('--name needs one name per JSON file')
    for n, p in zip(names, args.replays):
        replays[n] = json.loads(p.read_text())
    if args.arm:
        if not args.replay:
            ap.error('--arm needs --replay')
        raw = args.raw or Path(score['raw'])
        sidecar = raw.with_suffix('.json')
        for a in args.arm:
            n, opts = a.split('=', 1)
            cmd = [str(args.replay), str(raw)] + (['--sidecar', str(sidecar)] if sidecar.exists() else []) + shlex.split(opts)
            replays[n] = json.loads(subprocess.run(cmd, check=True, capture_output=True, text=True).stdout)
    if not replays:
        ap.error('give at least one replay JSON or --replay with --arm')

    arms = {n: measure(r, items, to_src) for n, r in replays.items()}
    ref_s = sum((it['end_sample'] - it['start_sample']) / SR for it in items if it['category'] not in NO_SPEECH)
    nonspeech_s = sum((it['end_sample'] - it['start_sample']) / SR for it in items if it['category'] in NO_SPEECH)
    print(report(arms, ref_s, nonspeech_s))
    bad = []
    if args.check:
        bad = check(next(iter(arms.values())), score['arms'][args.check], args.check)
    if args.json:
        args.json.write_text(json.dumps(dict(score=str(args.score), manifest=str(args.manifest or score['manifest']),
                                             played_seconds=score['played_seconds'], arms=arms)) + '\n')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
