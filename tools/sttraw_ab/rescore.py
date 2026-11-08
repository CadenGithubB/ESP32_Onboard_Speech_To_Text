"""Re-score an existing score.py result without decoding again.

Replays each arm's segments (stt_raw_replay is deterministic, so they match the saved per-segment texts
one for one), re-assigns them to the playlist with the current score.assign rule and recomputes the
per-arm results. Writes DIR/score.json and score.md in place (the previous files are kept as *.prev).

    python rescore.py DIR [--replay /Volumes/hw1build/stt-1.0-tools/stt_raw_replay]
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sttraw_ab import score  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir', type=Path)
    ap.add_argument('--replay', type=Path, default=score.REPLAY)
    args = ap.parse_args()
    path = args.dir / 'score.json'
    old = json.loads(path.read_text())
    m = json.loads(Path(old['manifest']).read_text())
    played = int(old['played_seconds'] * score.SR)
    items = [it for it in m['clips'] if it['end_sample'] <= played]
    fit = old['alignment']
    to_src = score.make_to_src(fit)
    raw = Path(old['raw'])
    minutes = raw.stat().st_size / 2 / score.SR / 60
    side = raw.with_suffix('.json')
    for name, arm in old['arms'].items():
        segs = score.replay(raw, arm['options'], side if side.exists() else None, args.replay)['segments']
        if len(segs) != len(arm['texts']):
            raise SystemExit(f'{name}: replay gives {len(segs)} segments, saved texts {len(arm["texts"])}')
        res = score.score_arm(segs, arm['texts'], items, score.assign(segs, items, to_src), to_src, minutes)
        res.update(options=arm['options'], config=arm['config'], texts=arm['texts'])
        old['arms'][name] = res
    path.with_suffix('.json.prev').write_text(path.read_text())
    path.write_text(json.dumps(old) + '\n')
    md = args.dir / 'score.md'
    if md.exists():
        md.with_suffix('.md.prev').write_text(md.read_text())
    md.write_text(f"# sttraw A/B score (re-assigned): {raw.name}\n\n" + score.table(old) + '\n')
    print(score.table(old))


if __name__ == '__main__':
    main()
