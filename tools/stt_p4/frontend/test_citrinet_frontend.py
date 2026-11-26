#!/usr/bin/env python3
"""Citrinet front end: host compile (optionally with sanitizers), numerical parity with the training code's
own front end (tools/stt_train_15x5/common.py Frontend, dither off), and bit-identity of the incremental
row paths (log_mel_frame, log_mel_frame_ring) with the batch path.

  test_citrinet_frontend.py --checkpoint <citrinet384-2000h-best> [--fixtures DIR] [--sanitize] [--output DIR]

Each case passes when it meets QuartzNet's tolerance against the training front end (test_frontend.py: max
|error| <= 0.0006, mean <= 0.00002), or, for ill-conditioned input such as near-silence (a mel bin whose spread
is ~1e-3 turns float32 FFT rounding into ~1e-2 after normalization in any float32 implementation), when it is
no less accurate than the training front end itself: its error against an exact float64 computation of the
same pipeline is at most 1.25x the training front end's.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import soundfile as sf
import torch

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
ENGINE = next(p for p in (ROOT / 'firmware/components/hw1_stt/stt', ROOT / 'components/hardwareone/stt') if p.is_dir())
TRAIN = next(p for p in (ROOT / 'tools/stt_train_15x5',) if p.is_dir())
sys.path.insert(0, str(TRAIN))
sys.path.insert(0, str(HERE))
from common import HOP, Frontend  # noqa: E402
from export_citrinet_constants import load_state  # noqa: E402


def exact_features(pcm, frontend):
    """The same pipeline in float64 (numpy FFT): the yardstick for ill-conditioned cases."""
    x = pcm.astype(np.float64) / 32768.0
    x = np.concatenate((x[:1], x[1:] - 0.97 * x[:-1]))
    padded = np.pad(x, 256, mode='reflect')
    window = np.zeros(512)
    window[56:456] = frontend.window.double().numpy()
    bank = frontend.fb.double().numpy()
    frames = -(-len(pcm) // HOP)
    rows = []
    for t in range(frames):
        spectrum = np.fft.rfft(padded[t * HOP:t * HOP + 512] * window)
        rows.append(np.log(bank @ (spectrum.real ** 2 + spectrum.imag ** 2) + 2 ** -24))
    f = np.array(rows)
    mean = f.mean(0)
    std = np.sqrt(((f - mean) ** 2).sum(0) / (frames - 1)) + 1e-5
    return (f - mean) / std


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--checkpoint', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, help='folder with 0.wav and 1.wav (16 kHz mono) to include')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--output', type=Path, default=HERE.parent / 'private/citrinet-frontend')
    args = parser.parse_args()
    torch.set_num_threads(4)
    frontend = Frontend(load_state(args.checkpoint))
    args.output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if sys.platform == 'darwin' and 'SDKROOT' not in env:
        env['SDKROOT'] = subprocess.run(['xcrun', '--show-sdk-path'], capture_output=True, text=True).stdout.strip()
    with tempfile.TemporaryDirectory(prefix='hw1-citrinet-frontend-') as directory:
        binary = Path(directory) / 'driver'
        cmd = ['clang++', '-std=c++17', '-O2', '-ffp-contract=off', '-Wall', '-Wextra', '-Werror',
               str(ENGINE / 'citrinet_frontend.cpp'), str(HERE / 'citrinet_test_driver.cpp'), '-I' + str(ENGINE),
               '-o', str(binary)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True, env=env)
        subprocess.run([str(binary)], check=True)
        rng = np.random.default_rng(921)
        cases = [('silence320', np.zeros(320, np.int16)), ('silence1600', np.zeros(1600, np.int16)),
                 ('near_silence1601', rng.integers(-1, 2, 1601, dtype=np.int16)),
                 ('random321', rng.integers(-32768, 32768, 321, dtype=np.int16)),
                 ('random1599', rng.integers(-30000, 30001, 1599, dtype=np.int16)),
                 ('impulse1600', np.array([32767] + [0] * 1599, dtype=np.int16)),
                 ('clipped8000', np.clip(rng.normal(0, 30000, 8000), -32768, 32767).astype(np.int16)),
                 ('sine480000', np.round(np.sin(np.arange(480000) * 0.08) * 16000).astype(np.int16))]
        for name in ('0.wav', '1.wav'):
            if args.fixtures and (args.fixtures / name).exists():
                pcm, rate = sf.read(args.fixtures / name, dtype='int16')
                if rate != 16000 or pcm.ndim != 1:
                    raise ValueError('Unexpected fixture format')
                cases.append(('fixture' + name[:-4], pcm))
        results = []
        for name, pcm in cases:
            raw = args.output / (name + '.pcm16le')
            raw.write_bytes(pcm.astype('<i2').tobytes())
            outs = {}
            for mode in ('batch', 'rows', 'ring'):
                out = args.output / f'{name}.{mode}.f32le'
                subprocess.run([str(binary), str(raw), str(out), mode], check=True, capture_output=True)
                outs[mode] = out.read_bytes()
            if outs['rows'] != outs['batch'] or outs['ring'] != outs['batch']:
                raise AssertionError('Incremental rows differ from the batch front end: ' + name)
            actual = np.frombuffer(outs['batch'], dtype='<f4').reshape(-1, 80)
            with torch.inference_mode():
                reference = frontend(pcm.astype(np.float32) / 32768.0, dither=False).numpy().T
            if reference.shape != actual.shape:
                raise AssertionError(f'{name}: shape {actual.shape} vs reference {reference.shape}')
            error = np.abs(actual - reference)
            exact = exact_features(pcm, frontend)
            c_exact, t_exact = np.abs(actual - exact), np.abs(reference - exact)
            within = error.max() <= 0.0006 and error.mean() <= 0.00002
            as_accurate = c_exact.max() <= 1.25 * t_exact.max() and c_exact.mean() <= 1.25 * t_exact.mean()
            item = {'case': name, 'samples': len(pcm), 'frames': len(actual), 'max_abs_error': float(error.max()),
                    'mean_abs_error': float(error.mean()), 'p99_abs_error': float(np.quantile(error, 0.99)),
                    'max_vs_exact': float(c_exact.max()), 'training_max_vs_exact': float(t_exact.max()),
                    'passed_by': 'tolerance' if within else ('as accurate as training' if as_accurate else 'FAILED'),
                    'incremental_bit_identical': True,
                    'pcm_sha256': hashlib.sha256(raw.read_bytes()).hexdigest(),
                    'features_sha256': hashlib.sha256(outs['batch']).hexdigest()}
            results.append(item)
            print(json.dumps(item), flush=True)
            if not np.isfinite(actual).all() or not (within or as_accurate):
                raise AssertionError('Front end parity exceeded tolerance: ' + name)
        record = {'schema': 1, 'scope': 'Host-only Citrinet front end parity, not P4 timing', 'sanitize': args.sanitize,
                  'results': results}
        (args.output / 'results.json').write_text(json.dumps(record, indent=2) + '\n')
        print(f'{len(results)} cases passed')


if __name__ == '__main__':
    main()
