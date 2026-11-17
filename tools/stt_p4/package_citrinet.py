#!/usr/bin/env python3
"""Package a Citrinet-384 int8 export for the P4 local-STT loader.

  package_citrinet.py --export <export folder with manifest.json> --vocab <vocab.txt> --engine <stt engine folder>

Writes, with every input checked against the export manifest:
  <engine>/citrinet-frontend-contract.json  front end parameters + SHA-256 of its sources; its SHA-256 is the
                                            front-end identity in the model envelope and the identity header
  <engine>/citrinet_vocab.h                 the WordPiece vocabulary compiled into the firmware (+ SHA-256)
  <engine>/stt_model_identity_citrinet384.h the pinned identity (stt_model_selection.h includes it)
  <export>/citrinet384-2000h.p4.stt         the model file for "/sd/STT Models/": the 96-byte HW1STT1 envelope of
                                            pack_model.py, codec 0 (stored: read straight into place from microSD)
  <export>/citrinet384-2000h.repacked.espdl the packaged graph on its own (generate_citrinet_compile_requirements.py)
The export's graph is packaged repacked (repack_espdl.py: the same model with what only a build reads, 1.3 MB,
at the end), so the firmware can return that part once its resident model is built; the identity header's
kResidentKeepBytes says how much of the image stays. --no-repack packages the export as it is
(kResidentKeepBytes == kRawBytes). Repacking needs esp-ppq: run this with the export environment's Python.
--check compares the generated text files instead of writing them.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

HEADER = struct.Struct('<8sIIIIII32s32s')
MAGIC = b'HW1STT1\0'
MAX_RAW_BYTES = 24 * 1024 * 1024
ARTIFACT = 'citrinet384-2000h.p4.stt'
FRONTEND_SOURCES = ['citrinet_frontend.h', 'citrinet_frontend.cpp', 'citrinet_constants.h', 'quartznet_constants.h',
                    'quartznet_frontend.h']


def sha(data): return hashlib.sha256(data).hexdigest()
def cbytes(raw): return ', '.join(f'0x{x:02x}' for x in raw)


def contract(engine, vocab_sha):
    value = {
        'schema': 1, 'name': 'hw1-citrinet384-2000h-frontend-v1',
        'input': {'sample_rate_hz': 16000, 'dtype': 'int16', 'channels': 1, 'minimum_samples': 320,
                  'maximum_samples': 480000, 'scale': 32768, 'preprocessed': False},
        'dither': None,
        'preemphasis': 0.97,
        'stft': {'fft': 512, 'window': 400, 'window_offset': 56, 'hop': 160, 'center': True, 'padding': 'reflect',
                 'window_source': 'checkpoint'},
        'mel': {'bins': 80, 'source': 'checkpoint', 'sparse_nonzero': 500, 'power': 2, 'log_guard': 2 ** -24},
        'cmvn': {'scope': 'whole valid utterance', 'variance_correction': 1, 'std_epsilon': 1e-5,
                 'implementation': 'float32, shifted by frame 0, Kahan sums (as quartznet_frontend)'},
        'output': {'dtype': 'float32', 'layout': 'T,80', 'frames': 'ceil(samples/160)', 'padding': False,
                   'maximum_frames': 3000},
        'ctc': {'tokens': 'WordPiece', 'vocabulary_sha256': vocab_sha, 'classes': 257, 'blank': 256,
                'time_stride': 8},
        'source_sha256': {n: sha((engine / n).read_bytes()) for n in FRONTEND_SOURCES},
    }
    return (json.dumps(value, sort_keys=True, indent=2) + '\n').encode()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--export', type=Path, required=True)
    p.add_argument('--vocab', type=Path, required=True)
    p.add_argument('--engine', type=Path, required=True)
    p.add_argument('--no-repack', action='store_true')
    p.add_argument('--check', action='store_true')
    a = p.parse_args()
    manifest = json.loads((a.export / 'manifest.json').read_text())
    vocab = a.vocab.read_bytes()
    vocab_sha = sha(vocab)
    if manifest['checkpoint']['vocab_sha256'] != vocab_sha:
        raise ValueError('vocab.txt is not the export checkpoint\'s')
    lines = vocab.decode().splitlines()
    inp, out = manifest['runtime']['inputs'][0], manifest['runtime']['outputs'][0]
    if (manifest['target'] != 'esp32p4' or inp['dtype'] != 3 or out['dtype'] != 3 or len(inp['exponents']) != 1
            or len(out['exponents']) != 1 or inp['shape'][3] != 80 or out['shape'][3] != len(lines) + 1
            or manifest['unexpected_operators'] or not manifest['embedded_fixture0']['output']['byte_exact']):
        raise ValueError('Not a checked Citrinet int8 export for the P4')
    model = a.export / manifest['model']['file']
    raw = model.read_bytes()
    if len(raw) != manifest['model']['bytes'] or sha(raw) != manifest['model']['sha256']:
        raise ValueError('Model differs from the export manifest')
    if not 16 < len(raw) <= MAX_RAW_BYTES or raw[:4] != b'EDL2':
        raise ValueError('Invalid bounded ESPDL graph')
    keep, packed = len(raw), 'as exported'
    if not a.no_repack:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import repack_espdl
        raw, report = repack_espdl.repack(raw)      # checks the result against the export field by field
        keep = report['keep_bytes']
        packed = f'repacked as {report["output"]["sha256"][:16]} (repack_espdl.py: the last {len(raw) - keep} bytes are build-only)'
        if not 16 < keep <= len(raw) <= MAX_RAW_BYTES or keep % 16:
            raise ValueError('Invalid repacked ESPDL graph')
    frontend = contract(a.engine, vocab_sha)
    frontend_sha = hashlib.sha256(frontend).digest()
    envelope = HEADER.pack(MAGIC, 1, 0, len(raw), len(raw), 16000, 480000, hashlib.sha256(raw).digest(), frontend_sha)
    assert len(envelope) == 96
    identity = f'''// Generated by tools/stt_p4/package_citrinet.py. Do not hand-edit model/frontend identities.
// Citrinet-384 2000 h ({manifest['checkpoint']['directory']}, weights {manifest['checkpoint']['weights_sha256'][:16]}),
// int8 export {manifest['model']['sha256'][:16]} ({manifest['scope'].split(';')[0]}, {manifest['calibration_steps']} calibration steps),
// {packed}.
#pragma once
#include <cstddef>
#include <cstdint>
namespace hw1::stt::identity {{
inline constexpr std::size_t kRawBytes = {len(raw)};
inline constexpr std::size_t kCompressedBytes = {len(raw)};
inline constexpr std::uint32_t kCodec = 0;  // 1 zlib, 0 stored (payload == raw)
inline constexpr std::uint8_t kRawSha[32] = {{{cbytes(hashlib.sha256(raw).digest())}}};
// Image bytes a built model still reads: tables and weights. What follows (per-channel exponents, parameter
// shapes and names, the embedded test tensors) is read only while a model is built; kRawBytes if not repacked.
inline constexpr std::size_t kResidentKeepBytes = {keep};
inline constexpr std::uint8_t kFrontendSha[32] = {{{cbytes(frontend_sha)}}};
inline constexpr char kInputName[] = "{inp['name']}";
inline constexpr char kOutputName[] = "{out['name']}";
inline constexpr int kInputExponent = {inp['exponents'][0]};
inline constexpr int kOutputExponent = {out['exponents'][0]};
inline constexpr int kNodeCount = {manifest['runtime']['node_count']};
inline constexpr int kSampleRate = 16000;
inline constexpr int kMaxSamples = 480000;
inline constexpr int kMaxFeatureFrames = 3000;
inline constexpr int kFeatureBins = {inp['shape'][3]};
inline constexpr int kOutputClasses = {out['shape'][3]};   // WordPiece tokens + the CTC blank (last)
inline constexpr int kTimeStride = 8;                      // output frames = ceil(feature frames / 8)
inline constexpr std::uint8_t kVocabSha[32] = {{{cbytes(bytes.fromhex(vocab_sha))}}};
}} // namespace hw1::stt::identity
'''
    escaped = ''.join('    "' + t.replace('\\', '\\\\').replace('"', '\\"') + '\\n"\n' for t in lines)
    vocab_header = f'''// Generated by tools/stt_p4/package_citrinet.py from the Citrinet-384 2000 h checkpoint's vocab.txt.
// One WordPiece token per line, in model output order; the CTC blank is index {len(lines)}.
#pragma once
#include <cstddef>
namespace hw1::stt::citrinet {{
inline constexpr char kVocabulary[] =
{escaped};
inline constexpr std::size_t kVocabularyTokens = {len(lines)};
}}  // namespace hw1::stt::citrinet
'''
    outputs = {a.engine / 'citrinet-frontend-contract.json': frontend.decode(),
               a.engine / 'citrinet_vocab.h': vocab_header,
               a.engine / 'stt_model_identity_citrinet384.h': identity}
    for path, text in outputs.items():
        if a.check:
            if path.read_text() != text:
                raise SystemExit('stale: ' + path.name)
        else:
            path.write_text(text)
    artifact = a.export / ARTIFACT
    if not a.check:
        tmp = artifact.with_suffix('.tmp')
        with open(tmp, 'wb') as f:
            f.write(envelope)
            f.write(raw)
        tmp.replace(artifact)
        (a.export / ARTIFACT.replace('.p4.stt', '.repacked.espdl')).write_bytes(raw)
    print(json.dumps({'artifact': str(artifact), 'artifact_bytes': 96 + len(raw),
                      'artifact_sha256': sha(envelope + raw), 'raw_sha256': sha(raw),
                      'export_sha256': manifest['model']['sha256'], 'resident_keep_bytes': keep,
                      'frontend_sha256': frontend_sha.hex(), 'vocab_sha256': vocab_sha,
                      'node_count': manifest['runtime']['node_count'], 'check': a.check}, indent=2))


if __name__ == '__main__':
    main()
