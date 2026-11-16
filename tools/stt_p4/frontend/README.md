# Portable QuartzNet frontend and CTC

The single C++17 implementation lives in `firmware/components/hw1_stt/stt/`.
The experiment probe and host tests compile those production files directly;
this folder contains references, generators, and tests only. The implementation converts **raw mono 16 kHz int16 PCM** into contiguous
float32 `[T,64]` features. `T = ceil(samples/160)`, without padded frames; the
ESP-DL input is the same bytes viewed as NHWC `[1,T,1,64]`. Accepted recordings
contain 320–480,000 samples (20 ms–30 s). It is a whole-utterance frontend,
not incremental streaming: normalization requires all valid feature frames.

`compute_features` uses caller-owned storage, has no heap/filesystem/HAL calls,
and does not change microphone gain or settings. Allocate input, output, and
workspace separately. At the 30-second bound, PCM needs 960,000 bytes, features
768,000 bytes, and `FrontendWorkspace` 8,200 bytes on the verified host ABI
(use `sizeof` on the target). The workspace includes a 512-sample ring instead
of another complete float waveform. All 64 mel filters use the 498 nonzero
checkpoint coefficients. Compile with `-ffp-contract=off`, without fast-math.

Do not feed the existing processed WAV/recorder path into this function: that
path already applies pre-emphasis. The contract requires the raw HAL samples
before HardwareOne's software DC removal, high-pass, pre-emphasis, gain, and
clipping chain. PCM scaling here is exactly `int16 / 32768`.

The frontend applies deterministic Gaussian dither at amplitude `1e-5`, then
pre-emphasis `0.97` exactly once; centered reflect-padded FFT 512, hop 160,
checkpoint Hann window 320; squared magnitude, checkpoint mel filters, and
`log(mel + 2^-30)` (`kLogGuard` in `quartznet_frontend.h`, `LOG_GUARD` in the
reference; `contract.py` refuses to write a contract unless both equal its
`log_guard`). Until 12 November 2026 the guard was `2^-24`, the value the models
were trained with. Per-bin CMVN removes the constant `2 log 8`, so `2^-30` gives
the recognizer exactly what `2^-24` gave on the same audio 8 times louder, with
no clipping and no change to the int16 samples, segmenter or speaker path
(`docs/STT_LOG_GUARD.md`). The overlap gate was trained on raw `2^-24` rows and
converts each row back (`overlap_rule.h` `to_gate_log_mel`). Dither resets to `0x12345678` at each utterance. Its generator
is xorshift32 followed by Box–Muller; the exact float32 operations and uniform
mapping appear in the C++ and independent Python reference. This deliberately
does not reproduce Torch's RNG. C/C++ math-library last-bit variation remains
possible across processors; fixture checks qualify the actual target.

CMVN uses each mel bin's valid frames only, unbiased sample variance (`T-1`),
and denominator `sqrt(variance) + 1e-5`. Mean and variance sums use float64;
mean and variance are cast to float32 before subtraction/square root. This
stabilizes near-silent input: the original Torch float32 mean reduction could
shift a mean by one ULP and amplify it to approximately 0.10 normalized units,
even when both FFT paths produced identical log-mel values. The mathematical
normalization is unchanged; its reduction precision is explicit and shared
with export/calibration.

`frontend-contract.json` pins this behavior and the hashes of the C++ source,
header, generated constants, provenance manifest, and Python reference.
`contract.py` resolves the C++ files in the production directory while retaining
the original basename keys, so deduplication does not change the model identity. Its
SHA256 is the frontend identity carried in the model envelope:

```
b21a8eeca826691406afbd096423429f2bd0fba8ea93032336368b77ef93dcc7
```

That is contract `hw1-quartznet5x5ls-en-frontend-v2` (log guard `2^-30`). The
`2^-24` contract was `ca91eb9683f7151b8be1a01532a6790c6eb7d515f67169a207751e3fc48cc58b`.
Its recorded C++ hashes had gone stale (`2fc57170…`/`262ab1ad…` against the files'
`09073499…`/`8ef041c8…`) because `contract.py` still pointed at HardwareOne's
`components/hardwareone/stt`. Models exported against `ca91eb96…`, including
v2 `gen2-25397` and the v3 packages, are refused by a firmware whose
`stt_model_identity.h` pins `b21a8eec…`. The other way round is checked at
build time: `contract.py` also writes
`firmware/components/hw1_stt/stt/quartznet_frontend_contract.h` (the contract
SHA-256 and the guard; not one of the hashed sources), and
`stt_frontend_interlock.cpp` (compiled only in the ESP-IDF component) fails with
a `static_assert` unless `stt_model_identity.h` `kFrontendSha` equals that SHA and
`kLogGuard` equals the generated guard. `contract.py --check` fails when the
generated header is stale, warns when `stt_model_identity.h` pins another
contract, and `--identity` makes that an error.

`ctc_feed` accepts float32 or common-scale int8 scores in `[frames,29]` order.
It uses first-index argmax ties, blank 28, and vocabulary space + a–z + apostrophe.
Its state preserves blank/repeat collapse across arbitrary chunk boundaries.
Output remains NUL-terminated; overflow is reported, and `ctc_finish` trims
trailing spaces and prevents further writes. Leading spaces are suppressed.
The 3,000-frame cumulative bound exceeds the model's maximum 1,500 output
frames. Float scores must be finite; NaNs are rejected before state mutation.

## Reproduce host checks

Run from the worktree root with the already isolated, pinned host ML environment
from `experiments/speech_portable/stt-requirements.txt`. No model download occurs
in these commands. The exact checkpoint and public fixture provenance are in
`experiments/speech_portable/float_quartznet.py` and the constants manifest.

```sh
python3 tools/stt_p4/frontend/contract.py --check            # add --identity once a matching package header is installed
$PY tools/stt_p4/frontend/test_model_container.py --model <package>.p4.stt   # must match stt_model_identity.h
$PY tools/stt_p4/frontend/test_frontend.py --sanitize --fixtures <checkpoint>/fixtures
$PY tools/stt_p4/frontend/test_runtime_quantization.py
# These two need the pinned 5x5 checkpoint in tools/speech_portable/private/stt-quartznet:
$PY tools/stt_p4/frontend/export_constants.py --check
$PY tools/stt_p4/frontend/test_transcripts.py   # compares with speech_portable's 2^-24 Torch decoder
```

`$PY` is an environment with torch, numpy and soundfile (for example the export venv).
`test_frontend.py` now builds the reference window and mel filters from the
compiled `quartznet_constants.h`, verified against the pinned checkpoint tensor
hashes in `constants-manifest.json`, so it needs only the two public speech
clips. The v3 checkpoints' own window differs from the pinned one by up to
`6e-8`. Export calibration uses the checkpoint window and the device uses the
pinned one; that difference was there before the guard change.

Results and generated feature/PCM fixtures stay in ignored
`experiments/stt_p4/private/frontend/`. The quantizer test extracts and compiles
the actual runtime rounding loop; it tests negative and positive half-ties,
adjacent representable values, saturation, and both speech feature buffers.

Host checks on 2026-11-12, guard `2^-30`, ASan/UBSan, fixtures from
`blend-nv/t2000-b70/fixtures`:

- Speech and broadband fixtures stay within `6e-4` max / `2e-5` mean, and
  their int8 inputs at exponent -3 are identical. Speech `0`: max `1.98e-5`.
  Speech `1`: max `1.54e-4`, mean `3.4e-7`.
- Two floor-dominated fixtures now exceed `6e-4`. Dithered `silence1600` reaches
  max `3.44e-3`, and `sine480000` reaches max `2.05e-3` / mean `1.1e-4`, with
  154 of 192,000 int8 values different. At `2^-24` they had max `2.4e-7` and
  `4.5e-4`, with 19 int8 differences. On the tone, the C++ and Torch float32
  frontends each differ from a float64 frontend by up to about `4e-3` at
  `2^-30`. The lower guard no longer hides float32 FFT rounding in cells far
  below the frame peak. These fixtures, `silence*`, `near_silence1601` and
  `sine480000`, get their own limits: `5e-3` max, `2e-4` mean, and at most 0.2%
  of int8 inputs different.

Host checks on 2026-09-28, guard `2^-24`, passed with ASan/UBSan:

- Nine frontend fixtures: silence, near-silence, impulses, short random inputs,
  a full 30-second sinusoid, and both public speech clips. Maximum error across
  all fixtures was `4.36e-4`, mean error at most `1.15e-5`. Regression thresholds
  are `6e-4` maximum and `2e-5` mean absolute error.
- Silence fixtures matched exactly. Speech maximum errors were `8.70e-6` and
  `1.02e-4`; their int8 model inputs were byte-identical to the Torch reference.
- Actual runtime quantization matched NumPy nearest-even on 124,579 edge/random
  values and all 149,440 speech feature values.
- Float transcripts from C++ features, the portable Torch reference, and the
  original Torch RNG/reduction path were identical for both speech clips.
- The runtime container validator accepted the real pinned envelope and rejected
  all 768 one-bit header mutations, all 96 truncated header lengths, incorrect
  total sizes, and invalid reader behavior under ASan/UBSan. These tests cover
  the envelope gate; the runtime separately checks decompressed model SHA256
  before the vendor parser. Tests compile the canonical production validator
  and verify the production runtime gate order.
- CTC tests passed all chunk split positions, float/int8 parity, tied scores,
  bounded output truncation, NaN rejection, extent overflow, and finalization.

These two clips are sanity checks, not held-out accuracy evaluation. Host
parity establishes neither P4 timing nor a realtime streaming capability.
The synchronous frontend has no cancellation callback; its target execution
latency must be measured before choosing watchdog/cancellation expectations.

## Constants and attribution

`export_constants.py` verifies the SHA256 of the inspected NVIDIA checkpoint
and configuration before `torch.load(..., weights_only=True)` with PyTorch
>=2.6. It exports exact window/mel float bits plus deterministic FFT tables.
The archive hash, tensor hashes, official download URL, and generated-header
hash are recorded in `constants-manifest.json`.

The source model is [NVIDIA QuartzNet5x5LS-En](https://catalog.ngc.nvidia.com/orgs/nvidia/-/models/nemospeechmodels/1.0.0a5).
NVIDIA's [maintainer clarification](https://github.com/NVIDIA-NeMo/Speech/discussions/1681)
states CC-BY-4.0 for its NGC NeMo checkpoints unless otherwise specified;
the downloaded archive did not include a separate license file. Preserve
NVIDIA attribution and checkpoint provenance with redistributed derived
constants/model artifacts. The reference algorithm follows the inspected
checkpoint and [NeMo feature implementation](https://github.com/NVIDIA/NeMo/blob/v1.0.0rc1/nemo/collections/asr/parts/features.py).
