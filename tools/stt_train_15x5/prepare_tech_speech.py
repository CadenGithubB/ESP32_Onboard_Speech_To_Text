#!/usr/bin/env python3
"""Tech-term training store for a continuation of checkpoint 25397.

Reads the spoken forms in the tech-term list, puts each term in a full
sentence, and drops any sentence that matches the frozen tech test set.
Speech comes from Piper, a local neural voice engine. Clips that are silent,
far too short, or far too long for their text are dropped. The store is clean
speech at about -20 dBFS. tools/stt_train_15x5/data.py applies MUSAN, DEMAND
and room impulse responses on the fly, the same way it treats every other
training store. A later run can pass --tech-far-fraction so that some of these
clips are rendered far and quiet during training; this writer stays clean.

  STT_ROOT=/Volumes/Audiodrive/stt prepare_tech_speech.py

Writes $STT_ROOT/stores/tech-speech-train/{audio.bin,index.json}.
A crash resumes from $STT_ROOT/tech-speech-build/.
"""
import argparse
import hashlib
import json
import os
import random
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np

ROOT = Path(os.environ.get('STT_ROOT', 'stt-data'))
TERMS = ROOT / 'hw1-work/analysis/techvocab/terms.tsv'
TESTS = ROOT / 'hw1-work/analysis/techvocab/test_sentences.jsonl'
BUILD = ROOT / 'tech-speech-build'
STORE = ROOT / 'stores/tech-speech-train'
DEV_STORE = ROOT / 'stores/tech-speech-dev'
RATE = 16000
DEV_ITEMS = 300
DEV_SEED = 2026

# Piper voices. Models live in $STT_ROOT/piper-voices/<name>.onnx.
VOICES = [
    'en_US-lessac-medium', 'en_US-ryan-medium', 'en_US-amy-medium',
    'en_GB-alba-medium', 'en_GB-alan-medium', 'en_GB-cori-medium',
]
VOICE_DIR = ROOT / 'piper-voices'
TEMPLATES = [
    'we shipped {term} this morning',
    'can you open {term} for me',
    'the {term} build failed again',
    'i left a note about {term}',
    'ask them to update {term} tonight',
    'the log mentions {term} twice',
]


def load_terms():
    rows = []
    for line in TERMS.read_text(encoding='utf-8').splitlines():
        parts = line.split('\t')
        if len(parts) < 2 or not parts[1].strip():
            continue
        rows.append((parts[0].strip(), parts[1].strip(), parts[2].strip() if len(parts) > 2 else ''))
    return rows


def load_forbidden():
    forbidden = set()
    for line in TESTS.read_text(encoding='utf-8').splitlines():
        if not line.strip():
            continue
        row = json.loads(line)
        forbidden.add(row['ref'].strip())
        spoken = row.get('tts', '').lower()
        if spoken:
            forbidden.add(spoken)
    return forbidden


def plan_items(terms, forbidden, takes):
    items, seen = [], set()
    for i, (display, spoken, category) in enumerate(terms):
        made = 0
        for k in range(len(TEMPLATES)):
            if made >= takes:
                break
            sentence = TEMPLATES[(i + k) % len(TEMPLATES)].format(term=spoken)
            if sentence in forbidden or sentence in seen:
                continue
            seen.add(sentence)
            items.append({
                'term': spoken, 'display': display, 'category': category,
                'text': sentence, 'voice': VOICES[(i * 3 + made) % len(VOICES)],
            })
            made += 1
    return items


def synthesize_batch(job):
    """Speak one voice's sentences with Piper and return 16 kHz PCM."""
    voice, items = job
    from piper import PiperVoice, SynthesisConfig
    from scipy.signal import resample_poly
    model = PiperVoice.load(VOICE_DIR / f'{voice}.onnx')
    config = SynthesisConfig()
    native = model.config.sample_rate
    out = []
    for item in items:
        parts = [chunk.audio_float_array for chunk in model.synthesize(item['text'], config)]
        if not parts:
            out.append((item['text'], voice, None))
            continue
        pcm = np.concatenate(parts).astype(np.float32)
        if native != RATE:
            g = int(np.gcd(native, RATE))
            pcm = resample_poly(pcm, RATE // g, native // g).astype(np.float32)
        kept = accept(pcm, item['text'])
        out.append((item['text'], voice, None if kept is None else kept.tobytes()))
    return out


def accept(pcm, text):
    if pcm is None or len(pcm) < int(0.3 * RATE):
        return None
    peak = float(np.max(np.abs(pcm)))
    if peak < 0.01:
        return None
    thresh = max(0.02, peak * 0.05)
    active = np.flatnonzero(np.abs(pcm) >= thresh)
    if len(active) < int(0.25 * RATE):
        return None
    pad = int(0.06 * RATE)
    start = max(0, int(active[0]) - pad)
    end = min(len(pcm), int(active[-1]) + pad)
    y = pcm[start:end]
    words = max(1, len(text.split()))
    seconds = len(y) / RATE
    if seconds < max(0.40, 0.12 * words) or seconds > 1.8 * words + 2.5:
        return None
    rms = float(np.sqrt(np.mean(np.square(y)) + 1e-12))
    y = y * (10 ** (-20 / 20) / rms)
    peak = float(np.max(np.abs(y)))
    if peak > 0.99:
        y *= 0.99 / peak
    return np.clip(np.round(y * 32767), -32768, 32767).astype('<i2')




def term_of(text):
    """Pull the vocabulary phrase back out of a templated sentence."""
    order = sorted(TEMPLATES, key=lambda template: len(template.split('{term}')[0]), reverse=True)
    for template in order:
        pre, post = template.split('{term}')
        if not text.startswith(pre) or (post and not text.endswith(post)):
            continue
        body = text[len(pre):len(text) - len(post)] if post else text[len(pre):]
        if body and body == body.strip():
            return body
    raise ValueError(f'not a templated sentence: {text}')


def hold_out_dev(items=DEV_ITEMS, seed=DEV_SEED):
    """Copy a fixed set of whole terms into tech-speech-dev and keep them out of training.

    The dev store's vocab.txt is the phrase list for the vocab score. Replacing that
    file points the same score at a different custom vocabulary.
    """
    if (DEV_STORE / 'index.json').exists():
        print(f'{DEV_STORE.name}: already written')
        _exclude_held_out()
        return
    index = json.loads((STORE / 'index.json').read_text())
    groups = {}
    for i, text in enumerate(index['texts']):
        groups.setdefault(term_of(text), []).append(i)
    order = sorted(groups)
    random.Random(seed).shuffle(order)
    held_terms, held_ids = [], []
    for term in order:
        group = groups[term]
        if len(held_ids) + len(group) > items:
            continue
        held_terms.append(term)
        held_ids.extend(group)
        if len(held_ids) == items:
            break
    if len(held_ids) < items // 2:
        raise SystemExit(f'only held out {len(held_ids)} clips')
    held_ids.sort()
    audio = np.memmap(STORE / 'audio.bin', dtype='<i2', mode='r')
    DEV_STORE.mkdir(parents=True, exist_ok=True)
    offsets, position = [], 0
    tmp = DEV_STORE / 'audio.bin.tmp'
    with tmp.open('wb') as out:
        for i in held_ids:
            clip = np.asarray(audio[index['offsets'][i]:index['offsets'][i] + index['lengths'][i]])
            out.write(clip.tobytes())
            offsets.append(position)
            position += int(index['lengths'][i])
    os.replace(tmp, DEV_STORE / 'audio.bin')
    sha = hashlib.sha256((DEV_STORE / 'audio.bin').read_bytes()).hexdigest()
    dev_index = {
        'offsets': offsets,
        'lengths': [index['lengths'][i] for i in held_ids],
        'texts': [index['texts'][i] for i in held_ids],
        'speakers': [index['speakers'][i] for i in held_ids],
        'audio_sha256': sha,
        'source': {
            'store': STORE.name, 'train_indices': held_ids, 'seed': seed,
            'role': 'development; excluded from tech-speech-train; vocab score only',
        },
    }
    (DEV_STORE / 'index.json').write_text(json.dumps(dev_index))
    (DEV_STORE / 'vocab.txt').write_text(''.join(term + '\n' for term in sorted(set(held_terms))))
    _exclude_held_out()
    link = ROOT / 'tech-continue-root' / 'stores' / DEV_STORE.name
    if link.parent.is_dir() and not link.exists():
        link.symlink_to(DEV_STORE)
    print(f'{DEV_STORE.name}: {len(held_ids)} clips, {len(set(held_terms))} phrases, sha256 {sha}')


def _exclude_held_out():
    """Keep the dev clips out of every copy of the training store."""
    dev = json.loads((DEV_STORE / 'index.json').read_text())
    ids = dev['source']['train_indices']
    copies = [STORE]
    extra = ROOT / 'tech-continue-root' / 'stores' / STORE.name
    if (extra / 'index.json').exists():
        copies.append(extra)
    for directory in copies:
        path = directory / 'excluded.json'
        previous = json.loads(path.read_text()) if path.exists() else {}
        previous['excluded'] = sorted(set(previous.get('excluded', [])) | set(ids))
        previous['reason'] = 'held out for the vocab score'
        path.write_text(json.dumps(previous) + '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--takes', type=int, default=2, help='sentences per term, different wording and voice')
    ap.add_argument('--workers', type=int, default=4)
    ap.add_argument('--limit', type=int, default=0, help='stop after this many planned items, for a trial')
    ap.add_argument('--hold-out-dev', action='store_true',
                    help='Hold whole terms out of the training store and write tech-speech-dev plus vocab.txt')
    a = ap.parse_args()
    if a.hold_out_dev:
        hold_out_dev()
        return
    if (STORE / 'index.json').exists():
        print(f'{STORE.name}: already written')
        return
    terms = load_terms()
    forbidden = load_forbidden()
    items = plan_items(terms, forbidden, a.takes)
    if a.limit:
        items = items[:a.limit]
    BUILD.mkdir(parents=True, exist_ok=True)
    (BUILD / 'sentences.jsonl').write_text(''.join(json.dumps(it) + '\n' for it in items))
    done = {}
    progress = BUILD / 'progress.jsonl'
    if progress.exists():
        for line in progress.read_text().splitlines():
            if line.strip():
                row = json.loads(line)
                done[row['text']] = row
    pending = [it for it in items if it['text'] not in done]
    print(f'terms {len(terms)}  planned {len(items)}  already {len(done)}  left {len(pending)}', flush=True)
    audio_path = BUILD / 'audio.bin'
    offsets, lengths, texts, speakers = [], [], [], []
    position = 0
    for it in items:
        row = done.get(it['text'])
        if row is None or row.get('dropped'):
            continue
        n = int(row['n'])
        offsets.append(position)
        lengths.append(n)
        texts.append(it['text'])
        speakers.append(it['voice'])
        position += n
    if audio_path.exists() and audio_path.stat().st_size != position * 2:
        raise SystemExit(f'{audio_path} is {audio_path.stat().st_size} bytes, progress accounts for {position * 2}')
    if not audio_path.exists() and position:
        raise SystemExit(f'missing {audio_path} with {position} samples already logged')

    by_voice = {}
    for it in pending:
        by_voice.setdefault(it['voice'], []).append(it)
    with progress.open('a') as log, audio_path.open('ab') as audio, ProcessPoolExecutor(a.workers) as pool:
        for batch in pool.map(synthesize_batch, by_voice.items()):
            for text, voice, pcm in batch:
                if pcm is None:
                    log.write(json.dumps({'text': text, 'voice': voice, 'dropped': True}) + '\n')
                    log.flush()
                    continue
                audio.write(pcm)
                audio.flush()
                n = len(pcm) // 2
                log.write(json.dumps({'text': text, 'voice': voice, 'dropped': False, 'n': n}) + '\n')
                log.flush()
                offsets.append(position)
                lengths.append(n)
                texts.append(text)
                speakers.append(voice)
                position += n
                if len(texts) % 200 == 0:
                    print(f'kept {len(texts)}  {position / RATE / 3600:.2f} h', flush=True)

    if a.limit:
        print(f'trial kept {len(texts)} of {len(items)}')
        return
    sha = hashlib.sha256(audio_path.read_bytes()).hexdigest()
    index = {
        'offsets': offsets, 'lengths': lengths, 'texts': texts, 'speakers': speakers,
        'audio_sha256': sha,
        'source': {
            'terms': str(TERMS), 'excluded_test': str(TESTS),
            'voices': VOICES, 'engine': 'piper', 'takes': a.takes, 'level_dbfs': -20,
            'noise': 'none in the store; data.Augmenter adds MUSAN, DEMAND and RIRs during training',
        },
    }
    STORE.mkdir(parents=True, exist_ok=True)
    tmp = STORE / 'audio.bin.tmp'
    os.replace(audio_path, tmp)
    tmp.replace(STORE / 'audio.bin')
    (STORE / 'index.json').write_text(json.dumps(index))
    print(f'{STORE.name}: {len(texts)} items, {position / RATE / 3600:.2f} h, sha256 {sha}')


if __name__ == '__main__':
    main()
