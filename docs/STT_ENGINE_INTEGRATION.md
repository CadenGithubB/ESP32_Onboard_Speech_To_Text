# Engine changes: HardwareOne integration handoff

> **Where the pieces are (15 Nov 2026).** Version 3.1 on `main` has the onset and the opt-in noise controls
> and builds with gen2-25397. The log guard `2^-30`, the interlock, the frontend-tool fixes (`contract.py`,
> `frontend-contract.json`, `quartznet_frontend_contract.h`), `docs/STT_LOG_GUARD.md` and
> `docs/STT_FRONTEND_GUARD.md` are on branch `voice-model-3-frontend`. Steps here that use them, `contract.py` or
> blend-g30 apply to that branch; on `main`, `contract.py` still points at HardwareOne's path.

13 November 2026 (Claude). For porting the `mic-level-fix` worktree's engine changes into the HardwareOne
firmware on another computer. Changes 1 and 2 are committed in version 3.1; change 3 (log guard `2^-30`, interlock, blend-g30
pairing) is on branch `voice-model-3-frontend` until Voice Model 3 is chosen. HardwareOne was read, not
changed: the counterparts below are from its checkout at 4bc86e67 ("Add glasses session gain and a
within-phrase leveler on both mics"). Re-check them against the HardwareOne tree you port into; it may have
moved on.

Everything measured here is host replay of saved P4 audio. Nothing in this document has run on a P4. The
worktree has been built with ESP-IDF 5.5.5 for the P4 (13 Nov, on a copy with the blend-g30 identity and
model path installed; section 10), but no image has been flashed.

## Summary

| # | Change | Port? | Why, in one line |
|---|---|---|---|
| 1 | Level-robust phrase onset (`decision_pre_emphasis` 97, `decision_gain_pct` 150) | Yes. It replaces HardwareOne's own pre-emphasis judge | Most far speech was never segmented: WER 74.58 -> 52.50% on the 1 ft day recording |
| 2 | Opt-in noise controls (modulation gate, raw-latch hold) | Optional, default off | They cut noise inside phrases but give back far speech; not a default candidate |
| 3 | Recognizer log guard `2^-30`, build interlock, `voice-model-v3-blend-g30` package | Yes, as one step, with the user's go-ahead for the model switch | Quiet P4 speech sat on the log floor: night-1 P4 audio 54.01 -> 50.59% WER |

Recommended order: 1 first (on the current model), then 3, and 2 only as a hidden option. Details in
"Integration order". The board plan runs that first step on the device as its own build (G: 1a51eac plus
the onset files, gen2-25397), because the worktree tree itself only builds with blend-g30. In this repository that split is version 3.1
(changes 1 and 2, builds on gen2-25397) and branch `voice-model-3-frontend` (change 3).

## 1. What changed and why

### 1a. Level-robust onset

**Problem.** Through the P4 microphone at 1 ft, far speech arrives at about 43 counts of 10 ms frame RMS.
The raw-only detector opens a phrase only at 45 counts or more, so 50-58% of the speech in the day-room
recordings fell outside every phrase and the recognizer never saw it.

**Design.** A frame opens or holds a phrase when either of two levels passes its rule:
- the raw level, exactly as before;
- the level of the pre-emphasised signal `x[n] - 0.97 x[n-1]`.

Room tone through this microphone is mostly low-frequency, so pre-emphasis removes most of the room while
speech stands out. The 45/16 floors reach the pre-emphasised domain through an ambient gain: the ratio of
the lower quartiles of idle raw and pre-emphasised energies, times 150%. The gain is re-learned on every idle
frame and never while a phrase is open. All decisions use exact 64-bit integer energies; there is no square
root and no float. The audio given to the recognizer is unchanged. Option 0 is the old detector, bit for bit
(diagnostics hash 10559851243095114257, equal to 1a51eac's).
Full arithmetic: [STT_LEVEL_ROBUST_ONSET.md](STT_LEVEL_ROBUST_ONSET.md).

**Measured.** Deployed gen2-25397 with `meeting.lm`, host replay of board recordings captured at PDM x1.
WER %:

| Recording (words) | Raw only (option 0) | HardwareOne judge 97 (today's default) | This onset 97/150 |
|---|---|---|---|
| ab1 `ee4c9cea`, 1 ft day (960) | 74.58 | 52.60 | **52.50** |
| ab1 `076aff4a`, 1 ft day (960) | 71.25 | 46.98 | **45.21** |
| nb1 `63c8c20c`, noise playlist (321) | 28.35 | 28.35 | **26.79** |
| `7c9393cb`, close, first 240 s (373) | 20.64 | 21.45 | 21.18 |
| errors, all four (2,614 words) | 1,568 | 1,127 | **1,103** |

Against the raw-only detector the onset removes 30% of the errors, nearly all of them deletions. Against
HardwareOne's judge it is only 24 errors better on these recordings, because they start with quiet room tone,
where the judge's calibration happens to work. Its real advantage is that it does not share the judge's
defects (section 2). With the loudest 0.5 s copied over the start of each recording, the judge leaves 159.78 s
(ee4c) and 156.06 s (076a) of speech outside every phrase. The onset leaves 10.63 s and 6.12 s, the same as on
the unmodified files.

### 1b. Noise controls (opt-in)

Two `SegmenterConfig` fields, both ignored while `decision_pre_emphasis` is 0:
- `decision_modulation_ratio_pct` (0 = off, 101..1000): the pre-emphasised level votes only while its last
  500 ms is modulated, that is, while the p90 frame exceeds the p10 frame by the ratio;
- `decision_pre_hold_requires_raw` (0/1): raw energy holds a phrase open only after the raw level has met
  the raw onset rule in that phrase.

With both off, production is unchanged bit for bit (hash 3709073732580116122).

| Arm | ee4c | 076a | nb1 | close | errors | Noise inside phrases, ee4c / 076a / nb1 (s) |
|---|---|---|---|---|---|---|
| onset 97/150 | 52.50 | 45.21 | 26.79 | 21.18 | 1,103 | 30.78 / 36.11 / 183.94 |
| gate 135 | 52.19 | 45.10 | 26.48 | 21.45 | 1,099 | 20.96 / 30.24 / 173.11 |
| gate 300 + hold (the worktree Kconfig option) | 61.25 | 46.56 | 27.41 | 21.18 | 1,202 | 3.66 / 2.76 / 65.53 |
| hold alone | 52.50 | 46.04 | 26.79 | 21.18 | 1,111 | 30.75 / 36.07 / 183.94 |

- Gate 300 + hold removes most of the noise, but gives back the far speech (ee4c +8.75 points). It does not
  reduce false words on nb1 noise: 6 segments / 8 words, against 4 / 8 for the onset alone.
- Gate 135 is neutral in-sample. Synthetic talker-under-steady-noise streams (not decoded) go against it.

Keep both off. If HardwareOne exposes them at all, present them as a noisy-room mode.

### 1c. Log guard `2^-30` and the blend-g30 package

**Problem.** The recognizer computes `log(mel + 2^-24)`. At the P4's speech level (about -61 dBFS),
46-48% of log-mel cells sat on that guard.

**Change.** After per-bin CMVN, `log(mel + 2^-30)` is exactly what the old guard gave on audio 8 times
louder, without clipping. On the real C++ frontend without dither, int8 inputs differ only at rounding ties.

The model's int8 calibration depends on the guard, so the guard ships only with a package recalibrated on
`2^-30` features: `voice-model-v3-blend-g30`. It has the v3 blend weights, the v3 recipe and the frontend
contract `b21a8eeca826691406afbd096423429f2bd0fba8ea93032336368b77ef93dcc7`.

HardwareOne runs v2 today (`quartznet15x5-gen2-25397`, the `stt_model_identity_15x5_gen2.h` identity), so
this step changes both the weights and the guard:

| | v2 gen2-25397 (HardwareOne today) | v3 blend, `2^-24` | **v3 blend-g30** |
|---|---|---|---|
| Night-1 P4 audio, 38,344 words, device LM | 55.7% | 54.01% | **50.59%** |
| confirm-1, 900 normal-level clips, LM errors | 2,035 | 1,737 | **1,727** |
| Noise-only, 200 chunks: chunks with words / LM words | 43 / 210 | 22 / 143 | 22 / 155 |
| Tech vocabulary, 2,101 words | 23.13% | 8.76% | not scored |

Sources: [STT_FRONTEND_GUARD.md](https://github.com/CadenGithubB/HardwareOne_Speech_To_Text/blob/voice-model-3-frontend/docs/STT_FRONTEND_GUARD.md) (on branch `voice-model-3-frontend`) and the package and v3 blend READMEs under
`/Volumes/Audiodrive/stt/work/release/`. The noise-only check is relative only; both noise sets were used in
training.

Combined with the onset, the guard was approximated by an x8 saturating gain on the recognizer input only,
which equals the `2^-30` guard on the gen2-25397 calibration (0 clipped samples). With the onset, errors went
from 1,103 to 1,080 (076a 45.21 -> 43.65, nb1 26.79 -> 25.55, close 21.18 -> 19.57, ee4c 52.50 -> 52.71).
With the raw-only detector they went from 1,568 to 1,574. **The blend-g30 package itself has not been scored
on these recordings, and the onset and the package have never been scored together.**

## 2. HardwareOne's pre-emphasis onset judge, and how this replaces it

**Where it is**
- `components/hardwareone/stt/stt_segmenter.h`:
  - the `uint8_t decision_pre_emphasis` field (0..99) with its comment;
  - per-sample `dframe_sum_` / `dframe_square_` with `d = x - (pre_prev_*c)/100`;
  - `decision_level()`;
  - `if(config_.adaptive && config_.decision_pre_emphasis)current_rms_=decision_level(n);` in
    `evaluate_frame`;
  - members `dframe_sum_`, `dframe_square_`, `pre_prev_`, `cal_raw_sum_`, `cal_dec_sum_` and
    `decision_gain_q8_`.
- `stt/stt_continuous_config.h`: `continuousSegmenterConfig(..., uint8_t decisionPreEmphasis = 0)` clamps the
  value to 99.
- Settings, defaulting to 97 for both mics:
  - `sttDecisionPre` and `sttG2DecisionPre` in `System_Settings.h`;
  - `sttdecisionpre` and `sttg2decisionpre` in `System_Settings.cpp` (range 0..99).
- `System_STT.cpp`: `sttDetectorProfileFor()` picks the per-mic value; it reaches
  `continuousSegmenterConfig` at the session start.
- Recording and replay:
  - `System_STTRawCapture.cpp` writes `"decision_pre"` into the `sttraw` sidecar;
  - `test/host/stt_raw_replay.cpp` takes `--decision-pre`;
  - `test/host/stt_segmenter_harness.cpp` asserts the clamp;
  - `test/host/stt_continuous_harness.cpp` sets the per-mic values.

Apart from the judge, HardwareOne's `stt_segmenter.h` is the 1a51eac header without the opt-in
`freeze_noise_during_speech` field. `Audio_VadPolicy.h` is byte-identical in both trees.

**Its defects, and what the new design does instead**

| HardwareOne judge | New onset |
|---|---|
| **Replaces** the raw level. `current_rms_` becomes the scaled pre-emphasised RMS, so the noise window, the peak and both floors all run on it, and raw loudness no longer opens a phrase. | **Adds** a second level. A frame passes when either the raw or the pre-emphasised energy passes its own rule; each domain keeps its own ambient, peak and thresholds. Worst per-clip loss against raw-only on the recordings: 0.02 s. |
| **Precision.** The decision RMS is `integer_sqrt` (floor) of the frame variance, then multiplied by the Q8 gain (clamped x1/8..x64). Flooring a small RMS before scaling quantizes the scaled level in steps of up to the gain. The gain is itself a ratio of summed floored RMS values. | Exact uint64 energies in 10^-4 count^2, with no square root in any decision. The floors are converted into the decision domain (`45^2*10^4 / G`) instead of rounding the signal. Unit case U3 fails on a floor-sqrt mutant. |
| **Gain frozen from the first 500 ms.** It is learned only while `!calibrated_`, that is, the first `calibration_samples` (8,000 = 500 ms) of a session, which is the first 500 ms after boot for the automatic session. A loud start (speech, a knock) or an atypical one fixes a wrong gain for the whole session: 159.78 s of ee4c speech outside phrases. | Lower quartiles of the idle raw and decision energies over 500-frame windows. The gain is re-learned on every calibration and idle frame, never while a phrase is open. Constant frames (digital zero, mute, DC) are skipped, and a zero quartile keeps the previous gain. After two hard-limit releases in a row with no raw onset, both ambients are re-seeded, so steady HF noise can end a phrase. Cases U4, U11 and U12. |
| **`publish_quiet_cut` unit mismatch.** With the judge on, `threshold_rms_` is in decision units, but the carry into the continuation counts raw `frame_energy(at)` against `threshold_rms_^2*F^2`, so the voiced carry depends on the room's spectral tilt. | The quiet-cut search ranks frames by decision energy; the carry compares each domain with its own threshold. Case U6. |

The setting's value keeps its meaning (a coefficient x100; 97 in both designs), but **the detector behind
value 97 changes**. Recordings and A/B results made with HardwareOne's judge cannot be reproduced with
`sttdecisionpre 97` after the port; they need the previous firmware build.

## 3. File map

Worktree root: the `mic-level-fix` worktree (`$W` in the board plan). HardwareOne root: its checkout.
"Verbatim" means a byte copy. The frontend files must be byte copies, or the contract SHA changes and the
package no longer matches (section 5).

### Segmenter (changes 1 and 2)

| Worktree file | HardwareOne counterpart | Action |
|---|---|---|
| `firmware/components/hw1_stt/stt/stt_segmenter.h` | `components/hardwareone/stt/stt_segmenter.h` | Replace with the worktree file. It is a superset of HardwareOne's public API (`push`, `segment`, `release`, `finish`, `noise_rms`, `threshold_rms`, `peak_rms`, `current_rms`, `calibrated`, `live_view`, `speculation`, `reset_session`, ...). HardwareOne's judge members disappear. It also adds `freeze_noise_during_speech` (default false, unused by HardwareOne). The field is `decision_pre_emphasis` in both trees; the valid range becomes 0..100. |
| `firmware/examples/p4_eye_captions/main/main.cpp` (`cfg.segmentation.*` under `CONFIG_CAPTIONS_DECISION_PRE_EMPHASIS` / `_MODULATION_GATE`) | `components/hardwareone/stt/stt_continuous_config.h` `continuousSegmenterConfig()` | Set `config.decision_pre_emphasis` from the per-mic value as today, and **also set `config.decision_gain_pct = 150`**: the struct default is 100, which scored 17 errors worse. Optionally add the two noise-control fields as parameters, default 0. |
| `firmware/examples/p4_eye_captions/main/Kconfig.projbuild` (`CAPTIONS_DECISION_PRE_EMPHASIS` default y, `CAPTIONS_DECISION_MODULATION_GATE` default n) and `sdkconfig.prev_onset` | HardwareOne runtime settings `sttdecisionpre` / `sttg2decisionpre` (`System_Settings.{h,cpp}`), routed by `System_STT.cpp` `sttDetectorProfileFor()` | No Kconfig needed. 0 = raw-only (the old detector, bit for bit); 97 = the new onset. If you expose the gate, add settings such as `sttdecisionmod` (0 or 101..1000, default 0) and `sttdecisionhold` (0/1, default 0). Update the help text: "applies to the next session" is still true. |
| (none) | `components/hardwareone/System_STTRawCapture.cpp` sidecar | Add `decision_gain_pct`, `decision_modulation_ratio_pct` and `decision_pre_hold_requires_raw`, plus a detector marker. Otherwise `"decision_pre":97` is ambiguous between the old judge and the new onset. |
| `tools/segmenter_replay/` (`segmenter_replay.cpp` with `--set field=value` and `--trace`; `coverage.py`; `make_proxy.py`; `README.md`) | `components/hardwareone/test/host/stt_raw_replay.cpp` (`--decision-pre`) | Add `--decision-gain` and the gate flags to `stt_raw_replay`, or run this repo's `tools/segmenter_replay/` with its include path pointed at HardwareOne's `components/hardwareone`. `tools/sttraw_ab/score.py --replay` drives either. |
| `tests/segmenter_decision_test.cpp`, `tests/test_segmenter_decision.py` (U1-U16, 11 mutants, option-0 and production hashes) | new, beside `test/host/stt_segmenter_harness.cpp` | Port them, and point `component` at `components/hardwareone`. Port check: with the new header, the option-0 hash must stay 10559851243095114257 and the production hash 3709073732580116122. The script's git-based base build (1a51eac) and the `private/segmenter_replay/stt_segmenter.before-gate.h` comparison are worktree-only; skip them in HardwareOne. |
| `tests/segmenter_boundary_test.cpp` modes 6-11, `tests/stt_stream_test.cpp` `decision_onset_pipeline`, `tests/test_segmenter_replay.py` | `test/host/stt_segmenter_harness.cpp` / `test_stt_segmenter.py`, `test_stt_raw_replay.py`, `test_stt_continuous.py` | Port the boundary modes (chunk invariance with the option on). HardwareOne has no `stt_stream.cpp`, so the pipeline case maps onto its continuous harness. Update `stt_segmenter_harness.cpp:421` if the clamp becomes 100. |

### Frontend guard, interlock and model (change 3)

| Worktree file | HardwareOne counterpart | Action |
|---|---|---|
| `firmware/components/hw1_stt/stt/quartznet_frontend.h` (`kLogGuard = 0x1p-30f`) | `components/hardwareone/stt/quartznet_frontend.h` | **Verbatim.** HardwareOne's copy differs from 1a51eac's only by a 5-line comment on `workspace->conditioned` staging; the worktree file has it. |
| `firmware/components/hw1_stt/stt/quartznet_frontend.cpp` (`kLogGuard` moved to the header) | `components/hardwareone/stt/quartznet_frontend.cpp` | **Verbatim.** HardwareOne's copy is byte-identical to 1a51eac's (`09073499…`). Keep its `-ffp-contract=off;-O2` compile options. |
| `quartznet_constants.h`, `tools/stt_p4/frontend/constants-manifest.json` | `components/hardwareone/stt/quartznet_constants.h` (the manifest has no HardwareOne copy) | Already byte-identical. Do not touch. |
| `tools/stt_p4/frontend/reference_frontend.py` (`LOG_GUARD = 2**-30`) | none: HardwareOne has kept no copy of this repo's tooling since 2026-11-26 | Nothing to port. It stays here, hashed into the contract. |
| `tools/stt_p4/frontend/contract.py` (guard cross-check, writes the interlock header, `--identity`) | none: HardwareOne has kept no copy of this repo's tooling since 2026-11-26 | Nothing to port. To check HardwareOne's integrated copy, run this repo's `contract.py --check` with `PRODUCTION` pointed at HardwareOne's `components/hardwareone/stt`. Point the `--identity` check at the identity header the P4 build actually uses (the `stt_model_identity_15x5_gen2.h` row below), not `stt_model_identity.h`. The path is not hashed. |
| `tools/stt_p4/frontend/frontend-contract.json` | none: HardwareOne has kept no copy of this repo's tooling since 2026-11-26 | Regenerate here with `contract.py`. It must print `b21a8eec…`. Any other value means a hashed file is not byte-identical. |
| `firmware/components/hw1_stt/stt/quartznet_frontend_contract.h` (generated) | new `components/hardwareone/stt/quartznet_frontend_contract.h` | Generated by `contract.py`; never hand-edit. |
| `firmware/components/hw1_stt/stt/stt_frontend_interlock.cpp` | new `components/hardwareone/stt/stt_frontend_interlock.cpp` | Copy it, but replace `#include "stt_model_identity.h"` with `#include "../System_BuildConfig.h"` (or however `System_STTLocal.cpp` resolves it) followed by `#include "stt_model_selection.h"`. HardwareOne picks the identity through `ENABLE_STT_MODEL_15X5_GEN2`; without the build config the selection falls back to the 5x5 identity, which pins the old contract, and the assert fires. |
| `firmware/components/hw1_stt/CMakeLists.txt` (adds the interlock) | `components/hardwareone/CMakeLists.txt` | Append `stt/stt_frontend_interlock.cpp` to the `esp32p4` local-STT list next to `stt/quartznet_frontend.cpp`. |
| `firmware/components/hw1_stt/stt/stt_model_identity.h` (still v2 in the worktree; replace it from the package) | `components/hardwareone/stt/stt_model_identity_15x5_gen2.h`, selected in `stt/stt_model_selection.h` | Install the package's `stt_model_identity.h` (`7d0d150b…`) as the P4 identity. Either overwrite the gen2 file, or, better for rollback, add it as a new file behind a new build flag. Against v2 it changes only `kRawSha`, `kFrontendSha` and `kInputExponent` (-4 -> -3); sizes, names and `kOutputExponent` (-1) are the same. |
| `main.cpp` `kModelPath` | `HW1_STT_MODEL_PATH` in `stt/stt_model_selection.h` (used by `System_STTLocal.cpp` `kModelPath`) | `/sd/STT Models/voice-model-v3-blend-g30.p4.stt`. Update `test/host/test_stt_model_selection.py` and this repo's staging scripts (`tools/stt_p4/deploy_gen2.sh`, `stage_stt_files.py`) that name the gen2 file. |
| `tools/stt_p4/frontend/{test_frontend.py, test_model_container.py, test_runtime_quantization.py, export_constants.py, README.md}` | none: HardwareOne has kept no copy of this repo's tooling since 2026-11-26 | Nothing to port. |
| `tools/stt_p4/export_quartznet.py` (`--ppq-setting`) | none: HardwareOne has kept no copy of this repo's tooling since 2026-11-26 | Export tooling only; run it from this repo to re-export. |
| `overlap_rule.h` (`kGateLogGuard`, `to_gate_log_mel`), `examples/p4_eye_captions/main/overlap_adapter.cpp`, `tests/overlap_rule_test.cpp` | none: HardwareOne has no overlap gate | Skip. If HardwareOne later adopts the gate, it must convert rows back to `2^-24` the same way. |

Unchanged and not to be ported as part of this: the speaker front end (`speaker_frontend.h` `log_eps`), the
speaker model and clustering, `stt_lm.*`, `meeting.lm`, the recognizer weights' training code (still
`2^-24`), `stt_auto_gain.h` and the leveler.

## 4. Configuration and defaults after the port

| Item | Worktree example | HardwareOne after the port (proposed) |
|---|---|---|
| Onset on/off and coefficient | Kconfig `CAPTIONS_DECISION_PRE_EMPHASIS`, default **y** -> C = 97 | `sttdecisionpre` (onboard) default 97; `sttg2decisionpre` (glasses) default 97 today; see the note below |
| Onset gain | `decision_gain_pct = 150` in `main.cpp` | `decision_gain_pct = 150` in `continuousSegmenterConfig` (constant, or a new setting 25..400) |
| Modulation gate | Kconfig `CAPTIONS_DECISION_MODULATION_GATE`, default **n**; y sets 300 | not exposed, or a setting with default 0 |
| Raw-latch hold | same Kconfig option, y sets it on | not exposed, or a setting with default 0 |
| Restore the old detector | `CONFIG_CAPTIONS_DECISION_PRE_EMPHASIS=n` or append `sdkconfig.prev_onset` | `sttdecisionpre 0` (raw-only, takes effect the next session); HardwareOne's old judge only by reflashing the previous build |
| Log guard | `quartznet_frontend.h` `kLogGuard = 0x1p-30f` (compile-time) | same, compile-time only; no runtime switch |
| Model | `kModelPath` + `stt_model_identity.h` | `HW1_STT_MODEL_PATH` + the identity selected by `stt_model_selection.h` |

**Glasses.** The new onset has never been run on glasses audio (LC3 after the glasses' AGC). HardwareOne's
glasses default is the judge at 97. Before choosing the glasses default, replay saved glasses `sttraw`
recordings through the replay tool: three from 8 Nov are on the drive with stored raw-only and judge scores
(board plan section 6, H1, with the decision rule). Until then, either keep 97 (new onset, unmeasured on
glasses) or set `sttg2decisionpre` to 0 (raw-only).

## 5. Model package pairing

- Package: `/Volumes/Audiodrive/stt/work/release/voice-model-v3-blend-g30/`.
  - `voice-model-v3-blend-g30.p4.stt`: 20,112,224 B, `a63997e5…`, stored (codec 0).
  - `stt_model_identity.h`: `7d0d150b…`.
  - `meeting.lm`: unchanged, `7bae9ff5…`.
  - Also `SHA256SUMS`: run `shasum -a 256 -c SHA256SUMS` (11 OK) after copying.
- Frontend contract: `hw1-quartznet5x5ls-en-frontend-v2`, `b21a8eeca826691406afbd096423429f2bd0fba8ea93032336368b77ef93dcc7`
  (v1 was `ca91eb96…`). It is a SHA-256 over the contract JSON. The JSON includes the SHA-256 of five files:
  `quartznet_frontend.h`, `quartznet_frontend.cpp`, `quartznet_constants.h`, `reference_frontend.py` and
  `constants-manifest.json`. Those five must be byte-identical to the worktree's, or the computed contract
  differs.
- Three checks guard the pairing:
  1. **Compile time:** `stt_frontend_interlock.cpp` `static_assert`s that the identity header's
     `kFrontendSha` equals the generated `quartznet_frontend_contract.h` `kSha`, and that `kLogGuard` equals
     the guard written there. A v2 or v3 (`2^-24`) identity fails to compile with this frontend; the
     blend-g30 identity compiles.
  2. **Load time:** `stt_model_container.h` `valid_header` accepts only the file whose header matches the
     identity. The blend-g30 identity refuses the v3 blend, v3 2,000-hour and 2,000-hour-g30 files.
  3. **Tooling:** `contract.py --check --identity` exits 1 unless the identity pins the contract.
- If HardwareOne must keep its own text in a hashed file, the contract SHA changes. The fix is then to repack
  the same export with `pack_model.py --frontend-sha256 <new sha>`, which is valid only if the computed
  features are unchanged. Do not change frontend arithmetic, constants or the reference after the export;
  that needs a re-export.
- Never pair the `2^-30` frontend with v2 (gen2-25397) or any `2^-24` package; the interlock exists to stop
  exactly that. The sibling `voice-model-v3-2000h-g30` also matches the contract but is unscored.

## 6. Integration order

1. **Preparation.**
   - Branch HardwareOne. Record the board's partition table and keep a private rollback image of the app and
     the card's `STT Models` folder (AGENTS.md).
   - Confirm `components/hardwareone/stt/stt_segmenter.h` and `quartznet_frontend.*` still match the
     descriptions above.
2. **Onset only, current model.**
   - Port the segmenter header, `continuousSegmenterConfig` (with gain 150), the sidecar fields and the
     replay flags.
   - Port and run the host tests (section 10). The option-0 and production hashes must match.
   - Build, flash the app only at the verified address, and run the board checks for the onset
     (section 9) on the unchanged gen2-25397 model. This isolates the segmenter change.
   - Evidence before this step: the board plan's build G against A (the same onset on gen2-25397 and the
     `2^-24` front end in the standalone example) and its section 6 H1-H2 (HardwareOne's gain chain and
     the glasses, replayed on the host). After it: section 6 H3 on HardwareOne itself.
   - Rollback: `sttdecisionpre 0`, or the previous app.

   Why staged, not one step: shipping onset, front end and blend-g30 together would need no extra board
   build, but a regression could not be attributed, each part could not be rolled back alone, and the
   onset would wait for the model go-ahead. The staged order costs one more standalone build (G) and about
   45 minutes of device runs. This document keeps the staged order.
3. **Guard, interlock and model, in one commit.**
   - Copy the five hashed files and port `contract.py`. Run it: it must print `b21a8eec…`.
   - Add the interlock with the selection include, install the blend-g30 identity and model path, and
     update model-selection tests and staging.
   - The build must fail with the gen2 identity and succeed with blend-g30 (a negative check worth doing
     once).
   - Copy the `.p4.stt` to `/sd/STT Models/`, keeping the gen2 file beside it, and run the board checks for
     the model.
   - This switches the shipped model and needs the user's go-ahead.
4. **Noise controls (optional).** Port the fields as hidden settings, default 0. Do not make them the
   default on the present evidence.
5. Update HardwareOne's CHANGELOG, user guide and command reference for `sttdecisionpre`: its meaning and
   default stay, but the detector behind it changes.

## 7. Rollback

| What | How | Note |
|---|---|---|
| Onset -> raw-only detector | `sttdecisionpre 0` / `sttg2decisionpre 0` (next session) | Bit-for-bit the 1a51eac detector (U1 hash) |
| Onset -> HardwareOne's old judge | reflash the previous app build | The judge code is removed by the port |
| Noise controls | setting 0 | Production unchanged bit for bit when 0 (U13 hash) |
| Guard + blend-g30 -> `2^-24` + gen2-25397 | reflash the previous app build; the gen2 `.p4.stt` must still be on the card | Each build accepts only its own model file; there is no runtime switch for the guard |
| Worktree example (reference) | `sdkconfig.prev_onset` or `CONFIG_CAPTIONS_DECISION_PRE_EMPHASIS=n`; previous `kModelPath`/identity with the `2^-24` frontend | The worktree tree does not build for the P4 until a `-g30` identity is installed (13 Nov: builds with blend-g30, refuses v2 and v3); the onset on `2^-24` needs a 1a51eac-based tree (board plan build G) |

## 8. Costs and risks

- **Noise acceptance (onset).**
  - nb1 noise items: 32.06 s (raw-only) -> 183.94 s of 190 s inside phrases (HardwareOne judge 185.52 s).
  - The three ab1 recordings carry +31 to +39 s of noise inside phrases.
  - False words on nb1 noise items: 2 / 4 -> 4 / 8 (judge 5 / 9).
  - A babble-only day stream gets 59 phrases / 287 s; raw-only gets none.
  - Bursty 4 kHz noise holds phrases open for as long as it lasts.
  - Steady HF noise ends after two hard cuts (re-seed), about 19 s of each 120 s.
  - Against HardwareOne today (judge 97) the noise exposure is about the same; against raw-only it is much
    larger.
- **Decode load.**
  - Audio sent to the recognizer (s, raw-only -> onset): ee4c 141.7 -> 383.6 (x2.7), 076a 180.0 -> 402.8
    (x2.2), nb1 138.8 -> 314.1 (x2.3), close x1.04.
  - Hard cuts: ee4c 1 -> 7, nb1 3 -> 21, 076a 0 -> 10.
  - HardwareOne's judge already sends a similar amount (similar phrase counts), so the change against today
    is small. The P4 decode queue, LM and speaker time under that load are unmeasured.
- **RAM and stack.** `sizeof(Segmenter)` on RV32 (`riscv32-esp-elf-g++` 14.2, `-Os`) is 1,288 B for
  HardwareOne's header and 4,472 B for the worktree's: +3,184 B, also when the option is off. Its config is
  60 -> 64 B. In the standalone example's 13 Nov ESP-IDF build this is the whole RAM cost: internal .data
  +3,264 B against 1a51eac (`gPipeline` 2,440 -> 5,680 B), the same with the onset on, off or gated.
  - In HardwareOne the `Segmenter` sits inside `StreamRun run`, a local in `sttContinuousWorker`, on the
    12,288-B `stt` task stack (`kWorkerStackBytes`). Check that task's stack high-water mark (HardwareOne
    registers it with `taskStackRecord`) before and after. If the margin is under about 4 KB, raise the
    stack or move `StreamRun` off it.
  - `push()` runs on the 8,192-B `stt_capture` task. The deepest segmenter chain grows from about 320 B to
    about 770 B at `-Os` (`percentile_energy` 544 B), plus 100 B with the gate on.
- **CPU.** Per frame: two energies coded once, and radix percentiles when the ambient or gain windows are
  refreshed. The gate adds a 100-byte copy, two `nth_element` over 50 and one 64-bit divide. RV32 `-O2` has
  13 `__udivdi3` call sites. No float. P4 cycles are not measured.
- **HardwareOne's gain chain is untested with the onset.**
  - The four recordings of the main tables are PDM x1 with the 36 Hz high-pass.
  - HardwareOne's onboard path starts at PDM x8 with the 120 Hz high-pass (`sttautogain`) and applies the
    within-phrase leveler (`sttPhraseLevel`, x1..x8 every 0.1 s) before the segmenter and the model.
  - The only x8 recording replayed with the onset, `e8f93103`, was covered (2.58 s speech outside, judge
    4.62 s) and word-scored on the host (13 Nov): WER option 0 56.67%, production 43.44%, gate 135
    44.17%, gate 300 + hold 44.79%, HardwareOne judge 97 43.65%; no false words on noise. This is x8 +
    120 Hz without the leveler (evidence `nohw/wer-e8f9/`).
  - A linear gain leaves the learned ambient ratio unchanged. It does lower the effective 45/16 floors, and
    a leveler step during a phrase is not seen by the gain, which updates only while idle.
  - Two more x8 recordings (`stt10-auto`) and three glasses recordings are on the drive with stored
    raw-only and judge scores, not yet replayed with the onset; new takes with the leveler are needed too.
    Steps: [STT_BOARD_TEST_PLAN.md](STT_BOARD_TEST_PLAN.md) section 6 (H1-H3).
- **Guard at higher input levels.** The guard's gain was measured at about -61 dBFS. At HardwareOne's x8 PDM
  plus leveler, fewer cells sit on the guard, so the benefit is probably smaller. A louder path puts more
  cells far below the guard, where float32 FFT rounding is no longer hidden (tone fixture: max error 2e-3 at
  `2^-30`). Close-range board audio was not rescored with the package.
- **Model switch.**
  - v2 -> v3 blend changes weights as well as the guard. Against v2 it is better on every scored set (tech
    vocabulary errors 486 -> 184).
  - Inference time on the P4 is expected to match v2 (same graph) but is not measured.
  - The overlap gate's `expf`/`logf` cost does not apply to HardwareOne.
- **Training mismatch.** Training code still uses `2^-24`. Any fine-tune for these packages must use
  `2^-30`. For such a fine-tune, real P4 noise floor is prepared on the Audiodrive (`stt/stores/p4-noise`,
  130 clips / 1,662 s from recordings never word-scored, and `p4-noise/heldout`, 93 clips / 1,241 s from
  the 16 word-scored ones; README in `stt/work/p4noise/`). Nothing has been trained with it.
- **Dither.** Calibration and fixtures are dithered; the device is not. The gap grows slightly at `2^-30`.
- **Status fields.** With the onset on, `noise_rms()`, `threshold_rms()` and `peak_rms()` are square roots of
  energies. HardwareOne copies them into `gSTT.snapshot` (`noiseRms`, `thresholdRms`, `peakRms`) and logs
  them, so their meaning changes on any status page or log reader.

## 9. What must be measured on the board

The full plan is [STT_BOARD_TEST_PLAN.md](STT_BOARD_TEST_PLAN.md): standalone builds A (1a51eac), G (onset on
gen2-25397), F, B, C (blend-g30 without the onset, with it, with the gate), and section 6 for HardwareOne's
gain chain and the glasses (H1 host replay of existing recordings, H2 HardwareOne raw takes, H3 HardwareOne
device after step 2). Minimum before shipping:
1. **Build and boot.**
   - The ESP-IDF build of the ported tree passes, and the interlock refuses the gen2 identity once (shown for
     the standalone tree on 13 Nov, section 10).
   - The model self-test passes against the package's embedded fixture.
   - Load time and SHA verification of the 20 MB stored file.
2. **Real time.**
   - Over a long session with the onset on: `counters.dropped` (or HardwareOne's audio-loss stop) stays 0.
   - Decode queue depth and caption latency under x2-3 decode audio.
   - CPU per frame of `push()`.
   - Stack high-water marks of `stt` and `stt_capture`.
   - Speaker labels on and off.
3. **Accuracy, recorded with `sttraw` and replayed on the host** with HardwareOne's real gain chain (PDM
   auto gain x8, leveler on):
   - the 1 ft day playlist (ab1) and the noise playlist (nb1);
   - a close-range take;
   - a real night-quiet take.

   Each with raw-only (0), the onset (97) and the blend-g30 package. Count WER, false words on noise and
   silence items, and speech outside or noise inside phrases. Board plan section 6 H2.
4. **Glasses.** Glasses `sttraw` sessions replayed with 0 and 97 before setting the glasses default: three
   8 Nov recordings already on the drive (H1, host only) and new ones with the glasses session gain (H2).
5. **Live talker.** At least one live person at 1 ft and 3 ft; all evidence so far is speaker playback.
6. **Storage.** SD/FAT behaviour with the larger model and longer phrase audio; host tests do not prove it.

## 10. Host checks and the ESP-IDF build

In the worktree, all passed on 13 Nov (single-threaded, `nice -n 10`, export venv for numpy; logs in the
evidence folder's `nohw/tests-final/`):
- `test_segmenter_decision.py`: U1-U16 (15 case groups), 11 mutants each detected, option-0 and production
  hashes at their golden values.
- `test_segmenter_boundaries.py`: 1,440 chunk comparisons, 2,346 reference segments.
- `tests/test_sttraw_ab.py` and `tests/test_overlap_gate.py` (31 tests) after the scorer's exponent-sign fix;
  neither covers the decode call, which the e8f93103 rerun checked instead (option 0 reproduced the stored
  score exactly).
- `test_segmenter_noise_freeze.py`, `test_stt_stream.py` (also `--thread-sanitize` on 12 Nov),
  `test_stream_telemetry`, `test_segmenter_replay.py`, `test_stream_sample_alignment`,
  `test_stream_comparison`.
- Frontend: `contract.py --check`, `test_frontend.py --sanitize`, `test_runtime_quantization.py`,
  `test_model_container.py`, `test_overlap_rule.py`, `test_feature_batch_workspace.py`,
  `test_quartznet_cache.py`, `test_pack_model.py` (12 Nov, [STT_LOG_GUARD.md](https://github.com/CadenGithubB/HardwareOne_Speech_To_Text/blob/voice-model-3-frontend/docs/STT_LOG_GUARD.md) on branch `voice-model-3-frontend`).

Not run: `export_constants.py --check` and `test_transcripts.py` (they need the pinned 5x5 checkpoint), and
`test_feature_cache_ownership.py --hardwareone`.

**ESP-IDF build (13 Nov).** `firmware/examples/p4_eye_captions` for `esp32p4`, built from a copy of the
worktree's files with the blend-g30 `stt_model_identity.h` (`7d0d150b…`) and `kModelPath` installed
(`contract.py --check --identity` printed `b21a8eec…`, exit 0) and ESP-DL 3.3.12 staged by
`tools/prepare_esp_dl.py` from the local component cache. ESP-IDF v5.5.5 with local edits (`portasm.S`,
`rvruntime-frames.h`: the PIE context-save fix), riscv32-esp-elf-gcc 14.2.0, `nice -n 10`, `-j 3`. Plain
`sdkconfig.defaults` (no metrics overlay, speaker labels off), so these are not the board plan's images.

| Build | App image | Flash .text / .rodata | Internal DIRAM | Result |
|---|---|---|---|---|
| 1a51eac (v2 identity, gen2-25397) | 650,288 B | 448,650 / 112,696 | 135,526 B | built |
| worktree, onset on (default) | 658,576 B | 453,694 / 112,680 | 138,790 B | built |
| worktree, onset + gate | 658,592 B | 453,706 / 112,680 | 138,790 B | built |
| worktree, `sdkconfig.prev_onset` | 658,560 B | 453,678 / 112,680 | 138,790 B | built |
| worktree with the v2 identity | | | | refused: `static_assert` at `stt_frontend_interlock.cpp:28` |
| worktree with the v3 blend identity | | | | refused, same assert |

- Every image leaves about 91% of the `0x715000` app slot free; the bootloader is unchanged.
- Worktree vs 1a51eac: image +8,288 B, flash .text +5,044 B, internal .data +3,264 B (3,240 B of it
  `gPipeline`). Onset on, off and gate have identical RAM.
- One warning per build, in third-party ESP-DL (`dl_image_jpeg.cpp:349`); none in the changed files
  (`-Wall -Werror=all -Wextra`, with ESP-IDF's `-Wno-unused-parameter -Wno-sign-compare`).
- Not built: the board plan's images (metrics overlay, speaker labels on), build G (1a51eac + onset files)
  and the optional overlap-gate build O. The example ships no `dependencies.lock`; a clean computer resolves
  `dl_fft` 0.7.0 and `esp_new_jpeg` 1.0.2 from the registry. Which ESP-IDF (with or without the local edits)
  the shipped images use needs recording.

In HardwareOne after the port, run:
- the ported decision test (both hashes);
- `test_stt_segmenter.py`, `test_stt_raw_replay.py`, `test_stt_continuous.py` and
  `test_stt_model_selection.py`;
- `test_quartznet_cache.py`, `test_stt_internal_memory.py` and `test_stt_lm_parity.py`;
- this repo's `tools/stt_p4/frontend/contract.py --check`, with `PRODUCTION` pointed at HardwareOne's
  `components/hardwareone/stt` (must print `b21a8eec…`).

## Evidence

Not in the repository. `$S` is the evidence folder, in two copies with an `INDEX.md` (which file supports
which claim) and a `SHA256SUMS`:
- the worktree's git-ignored `private/evidence-2026-11-13/` (text only: scripts, logs, scores, manifests,
  sidecars, build logs and size reports). It does not travel with a commit or a copy of the tracked files.
- `stt/work/engine-evidence-2026-11-13/` on the Audiodrive: the same plus caption caches, synthetic noise
  streams, traces, `.npz` outputs and the night proxy PCM (it predates `nohw/wer-e8f9/`,
  `nohw/idfbuild/` and `nohw/tests-final/`, which are in the worktree copy only).

The copied scripts still contain the 12-13 Nov scratch, drive and main-checkout paths; set their
`S=`/`W=`/`M=` variables before rerunning one.

- Board recordings: on the drive under `stt/sttraw/stt10-ab1/` (ee4c9cea, 076aff4a, e8f93103),
  `stt10-nb1/` (63c8c20c) and `stt10-close/` (7c9393cb). Manifests, sidecars and the stored judge and
  raw-only scores: `$S/harness/rec/` (`score_<id>.json`).
- Onset word scores: `$S/wer/<id>/score.{json,md}` and, with x8, `$S/wer-x8/<id>/`; e8f93103:
  `$S/nohw/wer-e8f9/` (`run.sh`, `e8f93103/score.json`, `table.txt`). The scorer the first runs wrapped is
  `$S/scorer/score.py` with `score-exponent-fix.diff`.
- Gate measurements: `$S/nohw/gatemeas/`, `$S/nohw/gate/`, `$S/nohw/wer/` and `$S/nohw/wer-hold/`.
- Host test logs: `$S/nohw/tests-final/`. ESP-IDF build: `$S/nohw/idfbuild/` (`build.sh`, `log-*.txt`,
  `size-*.txt`, `sizecomp-*.txt`, `SHA256SUMS.bins`; the `.bin` images were not kept).
- Guard scoring: `$S/guard/` (night 1 and confirm-1 in `guard/score/`, the noise-file check in
  `guard/noise/`, packing in `guard/repack/`).
- The package READMEs are on the drive, under `/Volumes/Audiodrive/stt/work/release/`.
