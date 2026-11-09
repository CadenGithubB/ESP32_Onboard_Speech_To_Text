# Training diagnostics and reproducible comparisons

Implemented 3 November 2026 in the standalone speech checkout. This is training
and evaluation tooling, not a recognition-model or firmware release. Start new
experiments in new private output directories. Old checkpoints remain usable as
initial weights; old resume files and unversioned caches are intentionally not
accepted as reproducible runs.

## Prepared on 9 November 2026

The scratch AdamW baseline finished on 9 November at
`/Volumes/Audiodrive/stt/work/scratch-tech-1e5` (`--weights scratch`, learning
rate 1e-5): step 20314, 500.02 audio hours, final score 1.0014, kept
checkpoint step 13297, score 0.9989. The switches below stayed off on that run. Each
later run changes one thing, in a new output directory, and a gain counts only
when the speaker-resampled interval on the WER difference stays off zero.
The int8-during-training arm started at 16:16 on 9 November at
`/Volumes/Audiodrive/stt/work/scratch-tech-rough` (`--rough-numbers on`, same
scratch recipe otherwise). Its config identity is
`76dfbcfedf811d7ce92d292bd043eb00707350dcdfc33e1ffbb5439df3a0ba30`. After it
finishes: `--farther`, then `--tech-far-fraction` only if the farther run
leaves ordinary speech intact, then one full NovoGrad run
(`--optimizer novograd`, learning rate 1e-3, same recipe otherwise). The
30 September 12-hour NovoGrad continuation started at score 0.279 and ended at
0.279, so that bake-off kept AdamW; the full run was still requested and waits
until those arms finish.

`--weights scratch` starts the same QuartzNet untrained. `--init` still supplies
the network shape and the feature front-end. Batch-norm statistics update,
because a scratch run has none to freeze. Step 0 is evaluated before any update,
then health, loss, and the error report continue for the whole run. The default,
`--weights checkpoint`, still loads the checkpoint and keeps batch-norm frozen.

- `--rough-numbers on` fake-quantizes each convolution weight and each post-ReLU
  activation to signed int8 during training. Batch-norm stays in the graph. The
  network shape and the int8 export are unchanged. Leave this off for a short
  continuation.
- `--farther on` raises room reverberation from about 45% of close clips and 15%
  of already-far clips to about 90% and 70%, and draws the level from -57 to -47
  dBFS instead of -40 to -12. The mix, the network and the tech clips stay as
  they are. Do not combine this with `--tech-far-fraction`.
- `--tech-far-fraction` is for a later run only. That fraction of tech-speech
  clips is rendered far and quiet in the loader. The rest stay at the clean
  stored level. `prepare_tech_speech.py` still writes clean clips at about -20
  dBFS. Skip that run if the farther run makes ordinary speech worse.
- A richer meeting language model, a wider beam, an int16 block, a bigger
  network and two-speaker separation are not part of these runs.

## Pipeline coverage

| Diagram steps | Diagnostic / correction |
|---|---|
| 2–3: preparation and splitting | Required stores, array lengths, audio ranges, keep/exclusion bounds, missing speaker IDs and cross-partition speaker overlap across all same-corpus shards. Independent `excluded.json` survives filtering. |
| 4–5: mixture and batches | Per-source requested, CTC-dropped, bucket-dropped, repeated and delivered items; unique source indices; augmented seconds and realized item shares. |
| 6: learning | Per-source normalized CTC loss; sampled gradient norms of each source's microbatch mean loss; actual effective batch size; global pre-clipping gradient norm and cumulative clipping count. Nonfinite CTC/gradients stop before the optimizer update. |
| 6: accumulation | Sum losses across microbatches, divide gradients by the actual total number of delivered items, then clip once and update. This deliberately changes the old equal-microbatch weighting. |
| 6: resume | Model, optimizer, Python/NumPy/Torch CPU/CUDA/MPS RNG, exposure counters and consumed-batch cursor are saved. Item-local augmentation/SpecAugment and a fixed sampler seed decouple examples from worker/prefetch order. |
| 6: schedule / rollback | Audio-based scheduling is the default; wall time only stops the run. `legacy-wall` explicitly selects the older schedule. Rollback loads the matching optimizer and weights from the selected evaluation snapshot, while keeping forward data exposure/schedule progress. |
| 7: evaluation | Required-set failures are fatal. Every selected example has a stable store/raw-index ID, PCM hash, speaker, duration, reference, hypothesis, insertion/deletion/substitution counts and paired changes against the previous evaluation. `evaluate.py --compare` also prints those counts for read, quiet, distant and tech speech, and a speaker-resampled interval on the WER difference. A change counts only when that interval stays off zero. |
| 7: additional objectives | `--diagnostic-sets` evaluates additional development stores without changing checkpoint selection. Empty references use phantom words/minute; WER is null. Reserved test stores are rejected for this option. |
| 8: cache provenance | Cache keys include model/config/tokenizer, dataset index/filter/exclusion/audio content, evaluator code and library versions. Legacy benchmark and CER cache imports are rejected rather than relabeled as verified. |
| 9–11: attribution | Float and frozen-int8 evaluation emit linked stage records; saved logits carry audio/model identity into LM decoding. A separate importer handles complete aligned device recordings. The comparator requires matching IDs, audio, references and model provenance. Missing stages are explicit. |

## Dataset audit and migration

Use a Python with the existing training dependencies (`torch`, `numpy`, `scipy`,
`PyYAML`; `sentencepiece` for Citrinet). The standalone audit and stage comparison
only require Python's standard library.

From the project root, a read-only inspection of the known split is:

```sh
python3 -B tools/stt_train_15x5/diagnostics.py \
  --stores /Volumes/Audiodrive/stt/stores \
  --train voxpopuli-train voxpopuli-train-b --dev voxpopuli-dev \
  --metadata-only --output private/training-diagnostics/voxpopuli-audit.json
```

The audit exits nonzero for issues. The real metadata screen found **35 overlapping
speakers**, plus missing speaker identities in both training shards. A broader
general2 audit also found **two AMI speaker IDs** shared by train and development. No real store
was modified. This is not proof of historical byte-level duplicate recordings.

For a new run, use prepared working copies of the stores, retain original manifests,
and repair speaker identity/exclusions there. Re-running `make_dev_views.py SRC DEV`
on those copies preserves existing DEV audio and excludes its speakers from every
same-corpus `*-train*` shard. The repaired `excluded.json` is applied independently
of `keep.json`. Re-run the audit after any new shard is added. Missing speaker IDs
must be resolved or explicitly excluded, not silently treated as a known speaker.

Legacy filters with `dev_held_out` but no `excluded.json` retain the intersection
with the old keep list. This conservatively prevents reintroduction; it cannot
recover the original reason for every rejected example. New CER caches bind to
actual audio, model/config and evaluator code. Archive old caches rather than
editing their identity to match.

Training performs full audio SHA-256 hashing by default, including augmentation
stores. This can be expensive on large drives. `--fingerprint-audio metadata` is
an explicit weaker diagnostic mode: it hashes indices/filters but only records
audio size/mtime; it does not certify audio content. Neither mode detects every
cross-corpus duplicate utterance with inconsistent speaker IDs, nor audits the
pretrained model's original corpus membership. Speaker IDs must be meaningful
within each corpus. Same-speaker leakage checks exclude noise environment IDs;
the DEMAND time split is not an unseen-environment test.

Add `--audit-test-stores NAME ...` to audit reserved test identities without running
recognition on them. Supply this list explicitly; dataset-name heuristics are not
used to infer every reserved partition.

## Running and diagnosing training

Use the normal `train.py` recipe with a **new** output directory under `private/`
or the existing private data root. Key options:

- `--seed 2026`: fixed sampler and augmentation seed, saved in the run identity.
- `--schedule audio`: default; keeps compute speed/deadlines out of the LR curve.
- `--gradient-diagnostics-every 50`: source-specific microbatch gradient norms.
  Set `0` only when deliberately disabling this expensive diagnostic. These are
  separate norms, not additive shares of the combined gradient.
- `--diagnostic-sets NAME ...`: additional **development** noise/noisy-speech/channel
  sets. Create genuine held-out development sets first; do not substitute the
  existing test sets. They do not yet influence the selection objective.
- `--stop-after-steps N`: save and stop at an absolute optimizer-step boundary,
  without adding a selection evaluation. Omit it or use a larger value to resume.
- `--workers 0`: supported for a small CPU smoke run; workers can change on resume.

`run-manifest.json` binds the recipe, source code, dataset identities, initial
weights/config/tokenizer and runtime versions. Changed inputs fail resume. The
manifest records current thread count and MPS fallback setting. Different hardware,
kernels, library versions or worker math can still affect floating-point results;
CPU smoke parity is not a guarantee of bitwise CUDA/MPS parity.

`train.jsonl` records audit/config, source-gradient, health, evaluation, resume,
rollback and stop/finish events. `resume.pt` includes counters and RNG state.
`evals/step_*/utterances.json` includes private transcripts and paired changes.
Selected evaluation snapshots also retain matching training state for rollback.
`best/` is restored from the resume checkpoint's selected snapshot on startup.
Snapshots retain all attempted source exposure; rollback does not pretend rejected
training examples were never processed.

CTC-infeasible examples are counted when dropped by collation. An all-invalid
microbatch fails with its source counters. Infinite/NaN CTC losses are no longer
zeroed silently; they fail with example IDs before updating weights. Nonfinite
gradients similarly fail. This detects numerical failures, not incorrect but
finite labels or gradients. Inspect source loss and error examples for those.

The current policy still repeats/trims rows to fixed bucket sizes. It is now
visible, deterministic and correctly weighted, not removed. Unique items are
unique store indices, not deduplicated audio; `audio_hours` counts augmented
exposure, including repeats. Do not call it hours of unique training data.

## Comparing checkpoints and recognition stages

For two float checkpoints, run `evaluate.py` on exactly the same sets and limits,
write each `--out` under private storage, and pass the first report as `--compare`
to the second run. Changed coverage, audio or references fail the paired comparison.
For noise-only sets, use phantom words/minute rather than a fabricated zero WER.

For the linked deployment comparison:

1. Run `tools/stt_p4/evaluate_dev_int8.py` with the existing checkpoint/export and
   model hash opt-in required by that tool. In addition to its summary and logits,
   it writes `MODEL-SET-float.json` and `MODEL-SET-int8.json`. Calibration PCM hashes
   are excluded even if filtered store positions have changed. This is frozen CPU
   int8 reconstruction, not measured hardware execution.
2. Run `tools/stt_train/lm/lm_int8_eval.py` with those new NPZ files and
   `--output-dir private/...`. It writes `MODEL-SET-LMHASH-lm.json`. Legacy NPZ files
   without audio/model provenance cannot produce a linked report.
3. Use `import_streaming.py --manifest private/.../manifest.json --output private/.../streaming.json`
   for **one complete capture per input**. It reads standalone final-event JSONL
   (`rawText`, never formatted `text`) or bench JSON containing `captions` with
   `sttText`. It rejects duplicate final sequences and mixed sessions.
4. Run `compare_stages.py --stage FLOAT.json --stage INT8.json --stage LM.json
   --stage STREAMING.json --output private/.../comparison.json`.
   `--allow-incomplete` explicitly permits an offline-only report with missing
   stages listed; it never claims complete device evidence.

A streaming import manifest has this shape (all paths relative to the manifest):

```json
{
  "provenance": {
    "weights_sha256": "actual acoustic weights hash",
    "espdl_sha256": "actual exported device model hash",
    "lm_sha256": "actual LM hash"
  },
  "cases": [{
    "id": "store-name:original-index",
    "pcm": "exact-input.pcm",
    "ref": "normalized reference words",
    "capture": "device-final-events.jsonl",
    "alignment": "alignment-evidence.json",
    "capture_complete": true
  }]
}
```

The alignment evidence must contain `passed: true`, a nonempty `method`, and the
actual `pcm_sha256` and `capture_sha256`. The importer binds evidence to those files;
it does not independently redo alignment or establish that the supplied model
identity was installed. Retain the existing device hash/readback and alignment
reports. A single recording containing many stimuli needs explicit verified
per-input alignment before import; the tool does not guess caption ownership.

Use the **same PCM** at each stage. For a microphone experiment, save and process
the captured microphone PCM offline too; playing a clean source through a speaker
does not make the clean source and microphone recording identical input. Paired
error deltas localize observed changes; they do not by themselves prove causality.

## Validation and limits

The **22-test targeted suite passed** and exercises actual torch collation, uneven accumulation math,
model/optimizer/RNG recovery, the real rollback path, sampler continuation, dataset
integrity, raw streaming import, and paired stage identity/coverage failures.

```sh
PYTHONDONTWRITEBYTECODE=1 "$PY" tests/test_training_diagnostics.py
PYTHONDONTWRITEBYTECODE=1 "$PY" tools/stt_train_15x5/smoke_diagnostics.py \
  --output private/training-diagnostics/NEW-SMOKE-DIRECTORY
```

The final smoke run passed with exact model/optimizer tensor equality and equal
exposure counters. Transcript, storage, viewer and bridge host checks also passed.
Private evidence is in `private/training-diagnostics/`.

The smoke script uses tiny synthetic audio and a small instance of the real
QuartzNet implementation, runs the real trainer and evaluates its output. It
compares uninterrupted vs resumed weights, optimizer and exposure counters after
four steps, including a loader change from zero to two workers. Its final run
exercises checkpoint selection output and paired reports. It proves plumbing,
not recognition accuracy or convergence.

No deployed models, real data stores or firmware were changed, and no board access
was performed. A fresh full float/int8/LM/device comparison, GPU/MPS resume parity,
long-run overhead and accuracy qualification remain separate experiments. The
available training Python lacks ONNX, so the new int8 producer's full inference
path is not qualified by this change's CPU smoke test.
