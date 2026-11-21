# Recognizer frontend guard 2^-30: change, packages, results (12 November 2026)

> **Not in main.** This change lived on branch `voice-model-3-frontend` (archived as tag
> `archive/voice-model-3-frontend`) and was not merged: Citrinet-384 replaced the QuartzNet Voice Model 3,
> with its own front end (guard 2^-24) and the microphone's hardware gain plus automatic gain for quiet
> input. Kept as a record of the equivalence proof and the results.

Summary for integration. Details of the change and its equivalence proof: `docs/STT_LOG_GUARD.md`.

## Why

The P4 microphone delivers speech near -61 dBFS. The recognizer computes `log(mel + G)` on int16/32768 audio with a
fixed guard `G = 2^-24`, so 46-48% of log-mel cells sat on the guard and words were lost. Re-decoding all 38,344
night-1 P4 words with a saturating x8 gain on the recognizer input took the recommended v3 model from 54.01% to
50.48% WER, with zero clipped samples and no words on noise clips (`realmic/gain-t2000-b70-x8.json`).

## The exact equivalence

`log(g^2 mel + G) = 2 log g + log(mel + G/g^2)`, and per-bin CMVN removes the constant `2 log g`. So `G = 2^-30`
(`2^-24/64`) gives the recognizer exactly what `2^-24` gave on audio x8 louder, with no clipping and without
touching the int16 samples, the segmenter, the speaker path or the weights. Measured with the real C++ frontend
(no dither, as on the device), 20 + 10 night-1 P4 segments and 20 normal clips, two independent runs:
raw rows `old(8x) - new(x) - ln 64` max 9.7e-7; CMVN features max 6.4e-6; int8 recognizer inputs differ only at
rounding ties (7 of 732,096 and 6 of 420,288). The equivalence holds only without dither; the device does not dither.

## Files changed (worktree `stt-mic-level-fix`, uncommitted)

- `firmware/components/hw1_stt/stt/quartznet_frontend.h:12` `kLogGuard = 0x1p-30f`, used by `mel_row` and
  `compute_features` in `quartznet_frontend.cpp` (cached and batch rows stay bit-identical).
- `overlap_rule.h` `kGateLogGuard`, `to_gate_log_mel`; `examples/p4_eye_captions/main/overlap_adapter.cpp` converts
  rows back to the gate's trained `2^-24` and pads with `log(2^-24)` (3 int8 differences in 1,460,480 cells).
- Build-time interlock (new): `stt/stt_frontend_interlock.cpp` (in the component's CMake sources) has two
  `static_assert`s: `stt_model_identity.h` `kFrontendSha` must equal the contract in the generated
  `stt/quartznet_frontend_contract.h`, and `kLogGuard` must equal the guard written there. `contract.py` writes that
  header (it is not one of the hashed sources) and `--check` fails when it is stale.
- `tools/stt_p4/frontend/`: `reference_frontend.py` `LOG_GUARD = 2**-30`; `contract.py` (guard cross-check, path,
  interlock header, `--identity`); regenerated `frontend-contract.json`; `test_frontend.py`,
  `test_model_container.py`, `test_runtime_quantization.py`, `export_constants.py` path and anchor fixes; README.
- `tools/stt_p4/export_quartznet.py`: the main checkout's `--ppq-setting` exporter patch, copied unchanged
  (sha `426015a8…`, the v3 manifests' exporter). Exporter tooling, not part of the guard change.
- `tests/overlap_rule_test.cpp` conversion sweep; docs `STT_LOG_GUARD.md`, this file, a handoff section.
- Unchanged by rule: training (`float_quartznet.py:72`, `stt_train*/common.py`, `g2_common.py:236`, still `2^-24`).

New frontend contract `hw1-quartznet5x5ls-en-frontend-v2`:
`b21a8eeca826691406afbd096423429f2bd0fba8ea93032336368b77ef93dcc7` (was `ca91eb96…`).

## The two packages

Both re-exported from the unchanged weights with the v3 recipe (esp-ppq 1.3.11, `mse` activation calibration,
the same calibration clip list, 100 entries in `manifest.json`, seed 2026, 8 steps) but calibrated on `2^-30` features, then packed with
`--frontend-sha256 b21a8eec…`. The ONNX graphs are byte-identical to the v3 exports; only calibration differs.
Exponents are unchanged: input -3, output -1. `--threads 3` instead of the v3 runs' 4.

| | `voice-model-v3-blend-g30` (recommended) | `voice-model-v3-2000h-g30` |
|---|---|---|
| Export | `int8/t2000-b70-mse-g30` (651 s) | `int8/t2000-best-mse-g30` (640 s, rerun) |
| Weights | `blend-nv/t2000-b70` (`e5f1c21c…`) | `tech2000-nvidia/best` (`c229ce87…`) |
| espdl sha | `a8d0f80d…` (v3: `36b2fc69…`) | `9bb4dc24…` (v3: `4a000ec1…`) |
| `.p4.stt` | 20,112,224 B, `a63997e5…` | 20,112,368 B, `a7382518…` |
| `stt_model_identity.h` | `7d0d150b…` | `a7431894…` |

Folders: `/Volumes/Audiodrive/stt/work/release/voice-model-v3-blend-g30/` and `…/voice-model-v3-2000h-g30/`
(README, SOURCE, SHA256SUMS, manifests, fixtures, `identity-vs-<v3 package>.diff`).

Commands (scripts in `guard/repack/` of the 12-13 Nov evidence folder: git-ignored `private/evidence-2026-11-13/`
in the worktree, full copy at `stt/work/engine-evidence-2026-11-13/` on the Audiodrive; see its `INDEX.md`):
```
export_queue.sh / export_best.sh: STT_ROOT=/Volumes/Audiodrive/stt/tech-continue-root HW1_STT_ALLOW_WEIGHTS_SHA256=<digest> \
  export-venv/bin/python -B export_quartznet.py --checkpoint <ckpt> --output int8/<name>-g30 --calib-stores \
  ami-sdm-train:20 ami-ihm-train:20 librispeech-train-clean-100:20 peoples-clean-train:20 voxpopuli-train:20 \
  --calib-seed 2026 --calib-steps 8 --threads 3 --ppq-setting quantize_activation_setting.calib_algorithm=mse
pack_g30.sh: export-venv/bin/python -I pack_model.py --manifest <build>/manifest.json --frontend-sha256 b21a8eec… \
  --artifact-name <package>.p4.stt --identity-name stt_model_identity.h --codec none
```
The first t2000-best export died at calibration 36/102 with no traceback (most likely killed with the session that started it); its partial
folder was removed and the export rerun detached (`setsid nohup`).

## How to integrate (needs the user's go-ahead: it switches the device model from v2 gen2-25397)

1. Copy the package's `stt_model_identity.h` to `firmware/components/hw1_stt/stt/`.
2. Set `kModelPath` in `firmware/examples/p4_eye_captions/main/main.cpp` to `/sd/STT Models/<package>.p4.stt` and copy
   that file to the card. Leave `meeting.lm` as it is.
3. `python3 tools/stt_p4/frontend/contract.py --check --identity` must print `b21a8eec…` and exit 0.
4. Until steps 1-2 are done this tree does not build for the P4: the interlock refuses the v2 header. That is
   intended; v2 and the v3 packages were calibrated for `2^-24`. Do not change the frontend sources, constants or
   reference after exporting: the contract SHA would change and the packages would no longer match.
   Checked with ESP-IDF 5.5.5 on 13 Nov (evidence `nohw/idfbuild/`): with the blend-g30 header and path the
   example builds (onset on, off and with the gate); with the v2 or the v3 blend header the build stops at
   `stt_frontend_interlock.cpp:28` (`static_assert`), and `contract.py --check --identity` exits 1 for both.
5. Before flashing: inspect the board's partition table, keep a private rollback image, flash only the app at the
   verified address (AGENTS.md).

## Results (host; nothing has run on a P4)

Same weights, same segments and clips; only the guard and the calibration differ. "v3" is the published
`voice-model-v3-blend` (`2^-24`). Device LM `meeting.lm`, alpha 0.15, beta 0.5.

| blend model | Night-1 P4 (38,344 words), LM | S / D / I | greedy | words on noise clips |
|---|---|---|---|---|
| v3, `2^-24` | 54.01% | 7,202 / 12,966 / 543 | 56.44% | 0 |
| v3, `2^-24`, x8 gain (old calibration) | 50.48% | 6,942 / 11,811 / 603 | 53.10% | 0 |
| **blend-g30** | **50.59%** | 6,970 / 11,819 / 611 | 53.09% | 0 |

Night 1 by category, LM WER v3 -> g30: AMI test 65.3 -> 61.8, AMI val 61.9 -> 57.2, LibriSpeech test 10.1 -> 8.4,
real32 71.5 -> 64.5, syn-ord 17.4 -> 15.2, syn-quiet 89.7 -> 88.5, syn-two 44.7 -> 43.3. The recalibrated export is 0.11
points behind the old-calibration x8 run.

| blend model, confirm-1 (900 normal-level clips) | LM distant / headset / read, total | int8 greedy | float |
|---|---|---|---|
| v3, `2^-24` (published) | 37.53 / 20.97 / 4.71 — 1,737 | 1,913 | 1,864 |
| **blend-g30** | **37.06 / 21.09 / 4.69 — 1,727** | 1,909 | 1,872 |

No regression on normal-level speech: -10 LM errors overall (distant -12, headset +3, read -1). Both rows use the
stock dithered scorer recipe; only the guard differs. The 2000h-g30 package was not scored.

| blend model, noise-file check (200 ten-second noise chunks, as in the v3 README) | chunks with LM words / LM words | greedy chunks / words |
|---|---|---|
| v3, `2^-24` (from its package README) | 22 / 143 | 19 / 227 |
| **blend-g30** (12 Nov, `guard/noise/noise-check-g30.log`, rc=0) | **22 / 155** | 19 / 216 |

The chunk count is the same; whether they are the same 22 chunks is not known, because neither log lists every
chunk (each prints 12 examples). Relative only: the noise sets were used in training.

## Checks

| Check | Command | Result |
|---|---|---|
| Contract | `cd tools/stt_p4/frontend && python3 -I -B contract.py --check` | `b21a8eec…`, exit 0; warns: v2 identity pinned |
| Identity gate | `contract.py --check --identity` | exit 1 with the v2 header (expected until integration) |
| Stale interlock header | append a line to `quartznet_frontend_contract.h`, `--check` | "is stale: rerun contract.py"; restored |
| Interlock | `clang++ -std=c++17 -Wall -Wextra -Werror -c stt_frontend_interlock.cpp` per identity | v2/v3 header: SHA assert fails; blend-g30, 2000h-g30: compiles; `kLogGuard` set to `2^-24`: guard assert fails |
| Envelope | evidence `guard/interlock/hdr.cpp` (`container::valid_header`) | each `-g30` header accepts only its own file; refuses both v3 files |
| ESP-IDF build (13 Nov) | evidence `nohw/idfbuild/build.sh` (ESP-IDF 5.5.5 with local edits, `-j 3`) on a copy of the tree | blend-g30 header: builds, one third-party warning (ESP-DL); v2 or v3 blend header: `static_assert` at `stt_frontend_interlock.cpp:28`, ninja exit 1 |
| Package sums | `shasum -a 256 -c SHA256SUMS` | 11/11 OK in both (rechecked after the drive dropped and returned at 19:32) |
| Export diff | manifest diff, `shasum` of `quartznet.onnx` | ONNX identical; clips, exponents unchanged; espdl, fixtures, contract, reference sha changed |
| Packer | `export-venv/bin/python -I -B tools/stt_p4/test_pack_model.py` | OK |
| Host tests | `python3 -I -B tests/test_feature_batch_workspace.py`, `test_stt_stream.py`, `test_quartznet_cache.py`; `test_overlap_rule.py` (export venv) | pass (2 s, 3 s, 10 s; 300 random sequences agree) |
| Night 1 | `night1_g30.py t2000-b70-mse-g30 int8/t2000-b70-mse-g30 blend-nv/t2000-b70 OUT` then `summ_night1.py` | 50.59% (91 min, CPU) |
| confirm-1 | `GUARD=g30 DITHER=1 confirm_nd.py --model …:int8/t2000-b70-mse-g30 --sets ami-sdm-validation ami-ihm-validation librispeech-dev-clean --count 300 --seed confirm-1 --threads 3`; `int8/lm_pair.py DIR t2000-b70-mse-g30 0.15:0.5` | 1,727 LM errors (55 min, CPU) |
| Earlier, unchanged since | `test_frontend.py --sanitize --fixtures`, `test_runtime_quantization.py`, `test_model_container.py`, speaker/segmenter tests, `equiv.py` | passed 12 Nov (`STT_LOG_GUARD.md`) |

The scorers in the evidence folder's `guard/score/` (`night1_g30.py`, `confirm_nd.py`; their internal paths
still name the 12 Nov scratch location) refuse an export whose manifest pins another contract or
reference, select the guard explicitly (one per process), and `night1_g30.py` checks its frontend binary against a
fresh worktree build and against the `2^-24` build before decoding. The stock `eval_int8_c.py` would have scored
`-g30` exports on `2^-24` features without noticing (`STT_LOG_GUARD.md`, "Repack, interlock and scoring").

## Not proven

- **On-device self-test:** not run. The packages embed fixture 0 (dithered `2^-30` features, as in v3); the P4 has
  not loaded either package. The tree (interlock, `overlap_adapter.cpp`, frontend) has been compiled with ESP-IDF
  5.5.5 for the P4 (13 Nov, Checks above) but no image has been flashed.
- **Live talker:** every P4 figure is a host replay of saved night-1 audio from the 6 November mic code. No live
  talker, no current-firmware recording, no false-caption playlist on hardware.
- **Close talker:** the guard only lowers the floor; loud close speech is not clipped by it, but close-range board
  recordings (sttraw, 7-8 Nov) were not rescored with the `-g30` exports (the noise-file check was: table
  above). A louder mic
  path later would put more cells far below the guard, where float32 FFT rounding is no longer hidden (tone fixture
  2e-3 max error at `2^-30`).
- **Training-side guard mismatch:** training still uses `2^-24` at normal levels. The `-g30` models are run at an
  effective x8 gain; any fine-tune must use `2^-30` (or the same mismatch reappears), and normal-level read speech
  moves 17-77% of int8 inputs.
- **Dither:** calibration and fixtures are dithered, the device is not; at `2^-30` the gap is slightly larger
  (fixture 0: 2.2% -> 3.0% of int8 inputs).
- Not measured: P4 timing (including the gate's `expf`/`logf` per cell), SD/FAT behaviour, and every score of the
  2000h-g30 package; for the blend-g30: board recordings, confirm-2, tech set, LibriSpeech. Board tests:
  `docs/STT_BOARD_TEST_PLAN.md`.
