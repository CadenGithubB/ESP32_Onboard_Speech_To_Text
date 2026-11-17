#!/usr/bin/env python3
"""C++/Python parity of the word-piece LM decoder (stt_lm_wordpiece.cpp against hw1lm_wordpiece.py).

  wordpiece_parity.py --lm meeting.lm --vocab vocab.txt --cases DIR [--custom WORDS] [--limit N]

DIR holds int8 Citrinet outputs as <name>.i8 with <name>.json ({"frames", "stride", "exponent"}; exponent in
the ESP-DL sign, value = q * 2^exponent). Every case is decoded by both under several settings (Citrinet's
tuned one, the LM file's, a narrow and a wide beam, a tight prune) and, with --custom, with a hotword list.
Any text difference is printed and the exit status is non-zero.
"""
import argparse
import json
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
ENGINE = next(p for p in (ROOT / 'firmware/components/hw1_stt/stt', ROOT / 'components/hardwareone/stt') if p.is_dir())
sys.path.insert(0, str(HERE.parent))
import hw1lm  # noqa: E402
import hw1lm_wordpiece as wp  # noqa: E402

SETTINGS = [dict(alpha=0.2, beta=0.75, beam_width=32, char_prune=-7.0),     # Citrinet's tuned setting
            dict(alpha=0.15, beta=0.5, beam_width=32, char_prune=-7.0),     # the LM file's (QuartzNet's)
            dict(alpha=0.5, beta=2.0, beam_width=8, char_prune=-7.0),       # narrow beam, strong LM
            dict(alpha=0.2, beta=0.75, beam_width=64, char_prune=-12.0),    # wide beam, loose prune
            dict(alpha=0.0, beta=0.0, beam_width=16, char_prune=-3.0)]      # acoustic only, tight prune


def build(out):
    env = dict(os.environ)
    if sys.platform == 'darwin' and 'SDKROOT' not in env:
        env['SDKROOT'] = subprocess.run(['xcrun', '--show-sdk-path'], capture_output=True, text=True).stdout.strip()
    cmd = ['clang++', '-std=c++17', '-O2', '-ffp-contract=off', '-Wall', '-Wextra', '-Werror', '-I' + str(ENGINE),
           str(ENGINE / 'stt_lm.cpp'), str(ENGINE / 'stt_lm_wordpiece.cpp'), str(HERE / 'wordpiece_parity.cpp'), '-o', str(out)]
    subprocess.run(cmd, check=True, env=env)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--lm', required=True)
    ap.add_argument('--vocab', required=True)
    ap.add_argument('--cases', type=Path, required=True)
    ap.add_argument('--custom', type=Path)
    ap.add_argument('--limit', type=int, default=0)
    a = ap.parse_args()
    lm = hw1lm.read_lm(a.lm)
    pieces = wp.Pieces(Path(a.vocab).read_text().splitlines())
    custom = hw1lm.load_custom_words(a.custom) if a.custom else None
    names = sorted(p.stem for p in a.cases.glob('*.i8'))
    if a.limit:
        names = names[:a.limit]
    with tempfile.TemporaryDirectory(prefix='hw1-wordpiece-parity-') as tmp:
        binary = Path(tmp) / 'harness'
        build(binary)
        lines, expected = [], []
        rng = random.Random(11)   # the tokenizer join alone, on random sequences of every token kind
        for i in range(2000):
            ids = [rng.randrange(pieces.blank) for _ in range(rng.randrange(0, 12))]
            lines.append('JOIN\t' + ' '.join(map(str, ids)))
            expected.append((f'join-{i}', ids, pieces.join(ids)))
        for name in names:
            meta = json.loads((a.cases / f'{name}.json').read_text())
            q = np.fromfile(a.cases / f'{name}.i8', np.int8).reshape(meta['frames'], meta['stride'])
            lp = wp.log_softmax_int8(q[:, :pieces.blank + 1], meta['exponent'])   # the C++'s own arithmetic
            for s in SETTINGS:
                lines.append('\t'.join(map(str, [a.cases / f'{name}.i8', meta['frames'], meta['stride'], meta['exponent'],
                                                 s['alpha'], s['beta'], s['beam_width'], s['char_prune']])))
                expected.append((name, s, wp.decode(lp, lm, pieces, custom, **s)))
        (Path(tmp) / 'cases.txt').write_text('\n'.join(lines) + '\n')
        cmd = [str(binary), a.lm, a.vocab, str(Path(tmp) / 'cases.txt')] + ([str(a.custom)] if a.custom else [])
        out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.splitlines()
    assert len(out) == len(expected), (len(out), len(expected))
    differ, stats = 0, []
    for (name, s, want), got in zip(expected, out):
        status, text, nodes, cands, completions, ws = got.split('\t')
        stats.append((int(nodes), int(cands), int(completions), int(ws)))
        if status != '0' or text != want:
            differ += 1
            print(f'DIFF {name} {s}: status {status}\n  python: {want}\n  c++:    {text}')
    ws = [x[3] for x in stats]
    decodes = [x for x in ws if x]
    print(f'{len(expected) - differ} of {len(expected)} identical: 2000 joins + {len(names)} cases x {len(SETTINGS)} settings'
          f'{" with custom words" if a.custom else ""}; workspace max {max(decodes)} bytes, mean {sum(decodes) // len(decodes)}')
    sys.exit(1 if differ else 0)


if __name__ == '__main__':
    main()
