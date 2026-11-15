"""Re-decode an existing score.py result with the current decoder, keeping its alignment, arms and replay options.

score.py runs before 10 November 2026 passed the ESP-DL output exponent (-1) to hw1lm.decode_int8 unnegated, so
the word LM saw logits 4x too large. This replays each arm's segments (stt_raw_replay is deterministic; the segment
count must equal the saved texts, otherwise the next --replay candidate is tried), decodes them again with the fixed
score.py decoder and recomputes every per-arm result and the --source-baseline, if the run had one. The previous
files are kept as score.json.wrong-sign and score.md.wrong-sign.

    python redecode.py DIR [DIR...] [--replay EXE ...]
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sttraw_ab import score  # noqa: E402
from overlap_gate.caption_wer import norm, edit_ops, LM_PATH  # noqa: E402

DEFAULT_REPLAYS = [Path('/Volumes/hw1build/stt-1.0-tools/stt_raw_replay'), score.REPLAY]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dirs', type=Path, nargs='+')
    ap.add_argument('--replay', type=Path, action='append', help='replay executables to try, in order')
    args = ap.parse_args()
    replays = args.replay or DEFAULT_REPLAYS
    sys.path.insert(0, str(score.ROOT / 'tools/stt_train/lm'))
    import hw1lm
    from overlap_gate.g0_extract import Recognizer
    rec = Recognizer(score.ROOT / 'private/training-track/overlap-gate/phase4/caption-cache')
    lm = hw1lm.read_lm(LM_PATH)

    def decode(x):
        return hw1lm.decode_int8(rec.logits(x.astype('<i2')), -rec.exponent, lm) if x.size >= 320 else ''

    for d in args.dirs:
        path = d / 'score.json'
        old = json.loads(path.read_text())
        if (d / 'score.json.wrong-sign').exists():
            print(d, 'already re-decoded; skipped')
            continue
        m = json.loads(Path(old['manifest']).read_text())
        played = int(old['played_seconds'] * score.SR)
        items = [it for it in m['clips'] if it['end_sample'] <= played]
        to_src = score.make_to_src(old['alignment'])
        raw = Path(old['raw'])
        raw16 = np.fromfile(raw, '<i2')
        minutes = raw16.size / score.SR / 60
        side = raw.with_suffix('.json')
        new = dict(old, arms={}, decoder=old.get('decoder', '') + '; re-decoded 10 Nov 2026 with the exponent sign fixed')
        for name, arm in old['arms'].items():
            for exe in replays:
                segs = score.replay(raw, arm['options'], side if side.exists() else None, exe)['segments']
                if len(segs) == len(arm['texts']):
                    break
            else:
                raise SystemExit(f'{d} {name}: no replay matches the saved {len(arm["texts"])} segments')
            texts = [decode(raw16[g['start']:min(g['end'], g['voiced_end'] + score.TAIL)]) for g in segs]
            res = score.score_arm(segs, texts, items, score.assign(segs, items, to_src), to_src, minutes)
            res.update(options=arm['options'], config=arm['config'], texts=texts, replay=str(exe),
                       wrong_sign_wer=arm['total']['wer'], wrong_sign_false_captions=arm['false_captions'])
            new['arms'][name] = res
        if 'source_baseline' in old:
            src = score.read_wav(m['wav'])
            base = {}
            for it in items:
                if it['category'] in score.NO_SPEECH:
                    continue
                ref = norm(it['reference'])
                s, de, i = edit_ops(ref, norm(decode(src[it['start_sample']:it['end_sample']].astype(np.int16))))
                c = base.setdefault(it['category'], dict(words=0, S=0, D=0, I=0))
                c['words'] += len(ref)
                c['S'] += s
                c['D'] += de
                c['I'] += i
            for c in base.values():
                c['wer'] = round(100 * (c['S'] + c['D'] + c['I']) / max(1, c['words']), 2)
            new['source_baseline'] = base
        (d / 'score.json.wrong-sign').write_text(path.read_text())
        path.write_text(json.dumps(new) + '\n')
        md = d / 'score.md'
        if md.exists():
            (d / 'score.md.wrong-sign').write_text(md.read_text())
        md.write_text(f"# sttraw A/B score (re-decoded, exponent sign fixed): {raw.name}\n\n" + score.table(new) + '\n')
        print(d, json.dumps({n: dict(wer=a['total']['wer'], was=a['wrong_sign_wer'], false=a['false_captions'],
                                     false_was=a['wrong_sign_false_captions']) for n, a in new['arms'].items()}), flush=True)


if __name__ == '__main__':
    main()
