# Recognizer log guard 2^-30 (12 November 2026)

> **Not in main.** This change lived on branch `voice-model-3-frontend` (archived as tag
> `archive/voice-model-3-frontend`) and was not merged: Citrinet-384 replaced the QuartzNet Voice Model 3,
> with its own front end (guard 2^-24) and the microphone's hardware gain plus automatic gain for quiet
> input. Kept as a record of the equivalence proof and the results.

## What changed

The recognizer front end now computes `log(mel + 2^-30)` on int16/32768 audio. Before this change it used
`2^-24`. The P4 microphone delivers speech at about -61 dBFS, and at that level 46-48% of log-mel cells sat
under the old guard. After per-bin CMVN, `log(64 mel + 2^-24) = log 64 + log(mel + 2^-30)` loses its constant
`log 64`. Lowering the guard by 64 therefore gives the recognizer exactly what it got from audio 8 times louder,
and nothing clips. The int16 samples, the segmenter, the speaker path, the model weights and training are
unchanged.

The x8 re-decode of night 1 was 54.01% -> 50.48% WER (`realmic/gain-t2000-b70-x8.json`), but that run used
the existing int8 export. The `-g30` packages are recalibrated on `2^-30` features, so night 1 and confirm-1
both have to be measured again on them.

- Device: `quartznet_frontend.h` `kLogGuard = 0x1p-30f` is the only device constant. `quartznet_frontend.cpp`
  uses it in `mel_row` (incremental and capture-time rows) and in `compute_features` (batch), so cached and
  batch rows stay bit-identical.
- Host reference: `tools/stt_p4/frontend/reference_frontend.py` `LOG_GUARD = 2**-30`. Export calibration,
  fixtures and host scoring use it.
- Contract: `contract.py` `LOG_GUARD = 2**-30`, name `hw1-quartznet5x5ls-en-frontend-v2`, path fixed to
  `firmware/components/hw1_stt/stt`. It refuses to write unless the C++ and Python guards equal its own. New
  identity: `b21a8eeca826691406afbd096423429f2bd0fba8ea93032336368b77ef93dcc7` (was `ca91eb96…`). The
  recorded `.cpp`/`.h` hashes also change because the old ones were already stale (`2fc57170`/`262ab1ad`
  recorded, `09073499`/`8ef041c8` actual).
- Overlap gate: the gate was trained on raw, non-CMVN `log(mel + 2^-24)` rows (`tools/overlap_gate/g2_common.py`
  `RawFrontend`, silence constant `log(2^-24)`). Its input is not mean-normalised, so a guard change is not an
  offset for the gate. The gate model is unchanged, so it keeps receiving `2^-24` rows. `overlap_rule.h` adds
  `kGateLogGuard = 2^-24` and `to_gate_log_mel(value, row_guard) = log(exp(value) + (2^-24 - row_guard))`.
  `overlap_adapter.cpp` applies that to cached and tail rows before `quantize_bin`, and pads with
  `log(kGateLogGuard)`, which is still -128 at exponent -3. The cost is one `expf` and one `logf` per gate cell,
  64 per 10 ms frame. P4 timing for that is not measured.
- Untouched: the speaker front end (`speaker_frontend.h` `log_eps` 1e-6) and training
  (`tools/speech_portable/float_quartznet.py:72`, `tools/stt_train*/common.py`, `g2_common.py:236`), which
  stays at `2^-24`. A future fine-tune at `2^-24` would bring back a train/deploy guard mismatch.

## Equivalence on real audio (host, C++ binaries)

The old guard was built from git `HEAD` (`quartznet_frontend.cpp` `09073499…`), byte-identical to the main
checkout's source. The new guard was built from this worktree. Both use `-O2 -ffp-contract=off`, without dither,
as on the device.

The check covered 20 night-1 P4 segments spread across the 2,740 captured finals and 20 normal-level original
clips spread across the 1,260 night-1 manifest clips (`tools/overlap_gate/caption_wer.py` NIGHTS enumeration).

| | P4 segments | Normal-level originals |
|---|---|---|
| RMS level | -64.0 to -55.6 dBFS | -44 to -26 dBFS |
| Clipped samples at x8 | 0 | 58,373 (19 of 20 clips) |
| Raw rows compared (frames none of whose samples clip at x8) | 10,689 of 10,689 frames | 8,936 of 12,131 frames |
| max \|old(8x) - new(x) - log 64\| on raw rows | 9.7e-7 | 9.7e-7 |
| CMVN features, unclipped items | 684,096 cells, max 5.7e-6 | 1 item, 48,000 cells, max 6.4e-6 |
| int8 input differences at exponent -3 / -4 | 5 / 1 (rounding ties) | 0 / 1 |
| Gate int8 (new rows converted back vs old rows) | 2 of 684,096 | 1 of 776,384 |

The `test_driver` binaries gave byte-identical CMVN features to the row driver on every item.

How far the guard moves the recognizer input on the same audio:
- P4 segments: 65-90% of int8 inputs change, mean 0.16-0.28 sigma.
- Normal-level AMI: 0.3-2.4% change.
- LibriSpeech test clips: 17-77% change, up to 0.15 sigma, because their pauses are close to digital silence.

Read speech is therefore where a normal-level regression would show. That is consistent with fixture 0 changing
32.4% of its bytes in the earlier study.

Scripts and results: `guard/` of the 12-13 Nov evidence folder (git-ignored `private/evidence-2026-11-13/` in
the worktree; the same files at `stt/work/engine-evidence-2026-11-13/` on the Audiodrive) holds `equiv.py`,
`equivalence.json`, `equiv.log` and the parity scripts; `INDEX.md` there lists them. The frontend binaries the
scripts used (`bin/`) were not kept; rebuild them from the old and new sources as described above.

## Tests

The following passed on 12 November 2026, single-threaded:
- `tests/test_feature_batch_workspace.py`
- `test_stt_stream.py`
- `test_quartznet_cache.py`
- `test_feature_cache_ownership.py`
- `test_overlap_rule.py`. It now also sweeps mel over 1e-14..1e3: conversion error at most 1.9e-6, and 1
  int8 rounding-tie flip in 400,001 values.
- `test_speaker_frontend_equivalence.py`
- `test_recognition_gain.py`
- `test_stt_internal_memory.py`
- `test_segmenter_boundaries.py` and `test_segmenter_decision.py`
- `tools/stt_p4/test_pack_model.py`
- `frontend/contract.py --check`
- `frontend/test_frontend.py --sanitize`. It now uses window/mel from `quartznet_constants.h`, verified against
  the manifest hashes. Floor-dominated fixtures get separate limits; see the README.
- `frontend/test_runtime_quantization.py`. Its end anchor was stale.
- `frontend/test_model_container.py` on v2 `gen2-25397`. Its order anchors were stale, and the
  `components/hardwareone/stt` paths were fixed.

Envelope check of the old `voice-model-v3-blend.p4.stt`:
- Accepted with its own identity header.
- Refused (`valid_header` false) when the header's `kFrontendSha` is the new contract.

Not run:
- `export_constants.py --check` and `test_transcripts.py`. They need the pinned 5x5 checkpoint, which is absent.
- `test_feature_cache_ownership.py --hardwareone`. HardwareOne was not touched, and its copy now differs.

## Dither

The equivalence above is exact only without dither, which is the device path (`quartznet_runtime.cpp`
passes `dither=false`; capture rows go through `mel_row`, which never dithers). With dither
(`kDither = 1e-5` added to x/32768), new(x) and old(clip(8x)) differ by up to 1.40 and in 16.4% of int8
inputs on 10 P4 segments, because the dither is 8 times larger relative to the signal. The exporter's
calibration clips and fixtures (`reference_frontend.features_pcm16`, `dither=True` by default) and the stock
`evaluate_dev_int8.py` scorer are dithered. On the normal-level fixtures the dithered-vs-undithered gap grows
slightly: fixture 0 (-26.5 dBFS) from 2.2% to 3.0% of int8 inputs, fixture 1 (-30.3 dBFS) from 0.76% to
0.80%. The repack keeps the dithered calibration recipe so it stays comparable with v3; host scoring of the
`-g30` exports is done without dither.

## Repack, interlock and scoring

These are in `docs/STT_FRONTEND_GUARD.md`: both `-g30` packages, the build-time interlock
(`stt_frontend_interlock.cpp`) and every score with its command.

Scorer notes (corrected): `int8/eval_int8_c.py` uses no frontend binary. It puts the main checkout's
`tools/stt_p4` first on `sys.path`, and `evaluate_dev_int8.py:129` computes features with the main checkout's
Python reference (`2^-24`, dithered). It checks only that each export reproduces its own stored fixture
features; it never compares the manifest's `frontend_contract_sha256` with the frontend it runs, and it uses
one frontend for all models in a run. A `-g30` export would pass and then be scored on `2^-24` features. The
binary-based scorers (`realmic/realmic.py`, `realmic/gain_redecode.py`, `release/noise_check2.py`, through
`tools/overlap_gate/g0_extract.py` FRONTEND) do use the main checkout's `2^-24` C++ binary, and their logit
caches are keyed only by the PCM hash. The scorers used for the `-g30` results (evidence `guard/score/`) select the guard
explicitly, refuse an export whose manifest pins another contract, run one guard per process and use fresh
caches.

## Not done or not proven

- On 13 Nov the tree was built with ESP-IDF 5.5.5 for the P4 with the blend-g30 identity installed
  (`overlap_adapter.cpp`, the frontend and the interlock compile; the v2 and v3 identities stop at the
  interlock), see `docs/STT_FRONTEND_GUARD.md` "Checks". Nothing has been flashed: P4 timing (including the
  gate's `expf`/`logf`), live-mic accuracy, noise false captions and SD/FAT behaviour are unmeasured.
- `tools/stt_p4/integration.patch`, `integration-inputs.json`, `docs/stt-source-parity*.json` and
  `heldout-provenance.json` keep old front-end hashes as history. `evaluate_input_sensitivity.py` will refuse
  to run under the new contract.
