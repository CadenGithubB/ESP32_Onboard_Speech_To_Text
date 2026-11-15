# Level-robust phrase onset (`decision_pre_emphasis`)

12 November 2026; noise controls and word scores 13 November. Committed in version 3.1 (written in the
`mic-level-fix` worktree, base commit 1a51eac). Host replay only: nothing here has run on a P4.

## Why

Through the P4 microphone, far speech arrives with loud 10 ms frames of about 43 counts. The segmenter
opens a phrase only on a frame whose mean-removed RMS is at least 45 counts (the fixed floor) and at
least the adaptive rule. So in the 1 ft day-room recordings 50-58% of the speech time fell outside every
phrase, and the recognizer never saw it. The engine applies no gain. Room tone through this microphone
is mostly low-frequency, while speech carries its energy higher up. A first difference (pre-emphasis)
removes most of the room, so speech stands out from it, but only if the floors are converted into
that domain correctly.

## What changed

- `firmware/components/hw1_stt/stt/stt_segmenter.h`
  - Two `SegmenterConfig` fields, appended after `short_close_silence_samples`:
    - `decision_pre_emphasis` (default 0 = the previous detector, bit for bit; C in 1..100, adaptive
      mode only);
    - `decision_gain_pct` (25..400, default 100; ignored when off).
  - `valid()` checks both. Every new statement runs only when `decision_pre_emphasis != 0`.
- Example: `CONFIG_CAPTIONS_DECISION_PRE_EMPHASIS` (`main/Kconfig.projbuild`, **default y**) sets
  C = 97 and gain 150 in `main.cpp` beside the other `cfg.segmentation` lines.
  `sdkconfig.prev_onset` is a one-line overlay that turns it off.
- `tools/segmenter_replay/segmenter_replay.cpp` exposes both fields to `--set` and uses 97/150 in
  `exampleConfig()`; with the option on the trace has six energy columns.
- Tests: `tests/segmenter_decision_test.cpp` + `tests/test_segmenter_decision.py` (new),
  boundary modes 6-9 in `tests/segmenter_boundary_test.cpp`, and a pipeline case
  (`decision_onset_pipeline`) in `tests/stt_stream_test.cpp`.
- Unchanged: raw PCM given to the recognizer, the 45/16 floors, `Audio_VadPolicy.h`, `stt_stream.cpp`,
  the frontend (`quartznet_*`) and the speaker code.

## Arithmetic (option on)

Per sample: `d = 100*x[n] - C*x[n-1]` (int32, |d| < 2^23), summed and squared per 10 ms frame (n = 160).
`pre_prev_` is stream state, cleared only by `reset_session()`.

Per frame, two exact uint64 energies in 10^-4 count^2 (no square root in any decision):
- `raw_e = (n*sum(x^2) - sum(x)^2) * 10^4 / n^2`
- `dec_e = (n*sum(d^2) - sum(d)^2) / n^2`

Windows (500 frames) hold a monotone 16-bit code of each energy (exact below 2048, else within 2^-10).
Percentiles are exact order statistics (two 256-bin radix passes, 512 B of stack).
- Ambients `amb_d`, `amb_r`: `noise_percentile` of the windows; while a phrase is open they may only fall.
- Gain G (Q16): lower quartile of idle raw energies / lower quartile of idle decision energies, times
  `decision_gain_pct`, clamped to [0.25, 4096]. Re-learned on every calibration and idle frame, never
  while a phrase is open. The idle windows skip constant frames (`raw_e == 0`: digital silence, mute, DC),
  and a zero decision quartile keeps the previous gain.
- Floors in the decision domain: `onset_d = 45^2*10^4 / G`, `voiced_d = 16^2*10^4 / G`.
- The adaptive rule in energy units (2x amplitude = 4x energy, peak/8 = peak/64):
  `trim = max(silence floor, 4*amb)`, `stop = max(trim, peak/64)` per domain.
- Onset: `(dec_e >= stop_d && dec_e >= onset_d) || (raw_e >= stop_r && raw_e >= 45^2*10^4)`.
- Voiced: `dec_e >= trim_d || raw_e >= trim_r`. Silent only when both are below their stop level.
- Quiet-cut search ranks frames by decision energy (latest wins ties); the carry compares each domain
  with its own threshold.
- Re-seed: after 2 hard-limit releases in a row with no raw frame at the raw onset rule
  (`kDecisionReseedCuts`), both ambients are re-seeded from the windows (as a pause would), keeping
  the gain and the open phrase, so a steady high-frequency source can end the phrase.
- Diagnostics: `current_rms` stays the raw floor-sqrt. `noise_rms`/`peak_rms`/`threshold_rms` become
  square roots of the energies (log meaning changes when the option is on).

Defects of the HardwareOne 8 Nov judge that are not copied: (a) decision level and gain from integer
floor(sqrt) RMS; (b) gain frozen from the first 500 ms after boot; (c) quiet-cut carry comparing a raw
tail with a decision threshold. Each fix has a unit case that fails on a mutant reverting only that fix.

## Review fixes (12 Nov)

| Finding | Status | Evidence after the fix |
|---|---|---|
| Zero decision quartile mapped to the maximum gain (x64 amplitude): 150 ms+ of digital zero at boot or a 2 s mute let room tone open and hold phrases (ee4c +38.7 s inside) | Fixed: constant frames skip the gain windows; `if(!d)return;` | 8000/16000-sample zero prefixes on ee4c/63c8/076 and the 63c8 30-32 s mute give segments identical to the unmodified files (pre-fix: 81/63/78 segments). U11 + 2 mutants |
| Steady HF noise just above the room held the phrase open for its whole length (120 of 120 s) | Fixed for stationary noise (re-seed) | white -40 dBFS 18.80 s, white -32 18.98 s, pink -36 19.04 s of 120 s inside (production 0). musan 4 kHz (bursty, frame energy p10..p90 = 0.9..28x ambient) still 120.00 s: not fixed. U12 + mutant |
| No pipeline test with the option on | Fixed | `decision_onset_pipeline`: 5 finals (3 hard-limit), 5 drafts, 1328 cached feature rows equal to the batch frontend, finals equal to the source; option off: 0 finals |
| Per-frame cost | Partly: each energy coded once per frame; gain recomputed only when it changes | P4 cycles not measured |
| Noise gates at ceiling; coverage pass only at the recorded level; gain tuned on ee4c | Not a code fix: recorded below | |
| Quiet-cut ranking by decision energy can cut in a raw-loud frame (e8f93103 #40: raw 73.4 vs window minimum 34.7) | Kept (spec choice; raw ranking was coverage-neutral). A raw-ranked variant is a candidate WER arm (needs a build) | |
| Default y changes every profile, including the lean control | Kept default y; added `sdkconfig.prev_onset` | |
| `--set adaptive=0` alone rejected by the driver | Documented in the replay README | |

## Host results (option on = example: C = 97, gain 150)

Speech seconds outside every phrase; noise / silence items inside; production = option 0; judge =
HardwareOne snapshot `--decision-pre 97` (stored arm).

| Recording | Production | On | Judge | Noise / silence inside, on (judge) | Phrases | Hard cuts |
|---|---|---|---|---|---|---|
| ab1 `raw_ee4c9cea` (x1, 1 ft) | 201.62 s (58.21%) | 10.63 s (3.07%) | 11.56 s | 30.78 (33.28) / 0.57 (0.31) | 40 -> 80 | 1 -> 7 |
| ab1 `raw_076aff4a` | 173.95 s | 6.12 s | 10.89 s | 36.11 (28.72) / 0.31 | 43 -> 75 | 0 -> 10 |
| ab1 `raw_e8f93103` (x8) | 74.58 s | 2.58 s | 4.62 s | 46.20 (46.19) / 1.47 (0.62) | 75 -> 80 | 0 -> 11 |
| nb1 `raw_63c8c20c` | 4.10 s | 0.69 s | 0.85 s | 183.94 of 190 (185.52) / 0.068567 (0.068567) | 26 -> 58 | 3 -> 21 |
| close `raw_7c9393cb` | 0.66 s | 0.12 s | 0.65 s | 20.00 (20.00) / 3.65 | 33 -> 33 | 5 -> 5 |
| close at -12 dB (proxy) | 30.30 s (21.79%) | 1.76 s (1.26%) | 2.38 s | 20.00 (20.00) / 1.97 (1.71) | 26 -> 31 | 2 -> 5 |

- Worst per-clip loss against production: 0.020 s (076aff4a clip 35).
- Seconds sent to the recognizer, ee4c: 141.65 -> 383.62 s with `--tail 8000` (latency profile);
  155.53 -> 401.28 s with `--tail 0` (example default). 076: 192.64 -> 415.20; nb1: 144.58 -> 325.64 (tail 0).
- Gain sweep, gain 140 / 150 / 160 / 200: ee4c outside 13.28 / 10.63 / 10.29 / 9.36 s; nb1 silence inside
  0.07 / 0.07 / 0.10 / 0.30 s, noise inside 183.94 / 183.94 / 183.94 / 184.04 s.
- Attenuation (held-out level; on / production, speech outside): ee4c -3 dB 21.59 s / 238.34 s,
  -6 dB 61.67 / 292.41 s, -12 dB 226.34 (65.35%) / 346.34 s; 076 -3 dB 13.98, -6 dB 54.83, -12 dB 232.55 s.
  The 12 s ee4c bound fails at -3 dB. At -12 dB the 45-count floor (mapped through G) binds, not the
  ambient: the option matches spectral tilt; it is not a gain stage.
- Boot: the loudest 0.5 s copied over samples 0..7999 gives 10.63 / 6.12 / 1.76 s (ee4c / 076 / proxy),
  the same as the unmodified files; the HW1 judge gives 159.78 / 156.06 / 30.35 s. Speech spliced over
  the first 3 s or 8 s leaves coverage unchanged on all six inputs.
- Synthetic streams (design2-robustness-first `synth_eval.py`): speech outside at or below production on
  every stream with target speech (live_night_30 15.44% vs 48.21%; live_mid_35 9.95% vs 36.11%).
  babble_only_day 59 phrases / 287.1 s (production 0); babble_only_night 25 / 119.1 s (production 16 / 42.5 s).
- Cost: `sizeof(Segmenter)` on 12 Nov host 1360 -> 4496 B, RV32 1240 -> 4360 B (riscv32-esp-elf-g++
  14.2, also when off; the global `gPipeline` is in internal RAM). With the 13 Nov noise controls, which
  are in the struct whether or not they are on: host 4,600 B, RV32 **4,472 B** (`$S/nohw/gate/szrv.cpp`).
  The 13 Nov ESP-IDF build confirms it: `gPipeline` in internal .data 2,440 -> 5,680 B against 1a51eac,
  the same with the onset on, off (`sdkconfig.prev_onset`) or with the gate (`$S/nohw/idfbuild/`).
  RV32 -Os stack: `percentile_energy` 544 B; deepest segmenter chain about 320 -> 770 B on the 8192 B
  capture task (+100 B with the gate on). RV32 -O2: 13 `__udivdi3` call sites. Host: 5 ee4c runs 0.14 s
  user off, 0.25 s on.

## Commands

Evidence for this document is in `$S`: the git-ignored `private/evidence-2026-11-13/` of the worktree
(text: scripts, logs, scores, manifests, sidecars), with a full copy including caption caches, traces and
the night proxy PCM at `stt/work/engine-evidence-2026-11-13/` on the Audiodrive. Its `INDEX.md` maps each
file to the claim it supports. The board recordings themselves are on the drive under
`stt/sttraw/stt10-ab1/` (ee4c9cea, 076aff4a, e8f93103), `stt10-nb1/` (63c8c20c) and `stt10-close/`
(7c9393cb); link them into `$REC` before running. The copied scripts still name the 12-13 Nov scratch
location inside; set their `S=`/`W=`/`M=` variables first. The late 13 Nov folders (`nohw/wer-e8f9/`,
`nohw/idfbuild/`, `nohw/tests-final/`) are in the worktree copy only.

```sh
W=<the mic-level-fix worktree>; cd $W
S=private/evidence-2026-11-13             # or the drive copy, stt/work/engine-evidence-2026-11-13
REC=$S/harness/rec; P=$S/proxy/raw_7c9393cb_night12db.pcm   # the proxy .pcm: drive copy, or make_proxy.py
PY="<any Python 3 with numpy> -B"
B=private/segmenter_replay/segmenter_replay      # B0 = private/segmenter_replay/segmenter_replay.base (before)
# G0 build (and the same at -O1 with -fsanitize=address,undefined -fno-sanitize-recover=all)
clang++ -std=c++17 -O2 -Wall -Wextra -Werror -Ifirmware/components/hw1_stt tools/segmenter_replay/segmenter_replay.cpp -o $B
# G1 option 0 == B0 and stored production (6 inputs x 6 block settings x {gain 100, 400}, + --trace): 150 comparisons, 0 mismatches
$PY $S/gates/g1.py
# G2 stored production scores (exit 0 on all five; 63c8c20c uses nb1.json)
$PY tools/segmenter_replay/coverage.py --score $REC/score_ee4c9cea.json --manifest $REC/ab1.json --replay $B \
  --raw $REC/raw_ee4c9cea.pcm --arm base="--set decision_pre_emphasis=0" --check production
# G3/G4 option on vs stored arms (table above), G5 chunk invariance (24 comparisons, 0 mismatches)
$S/final/g34.sh; $PY $S/final/summ.py cov; $PY $S/gates/g5.py $B
# G7 host tests
python3 tests/test_segmenter_boundaries.py; python3 tests/test_segmenter_noise_freeze.py
python3 tests/test_stt_stream.py; python3 tests/test_stt_stream.py --thread-sanitize
$PY tests/test_segmenter_replay.py; python3 tests/test_segmenter_decision.py
# Review adverse inputs (zero prefix, mute, persistent noise, attenuation, splices) vs the pre-fix driver
$PY $S/final/adv.py
```

Current results (13 Nov, after the noise controls and their review fixes; logs in `$S/nohw/tests-final/`):
`test_segmenter_boundaries.py`: 1,440 chunk comparisons, 2,346 reference segments (boundary modes
0-11). `test_segmenter_decision.py`: U1-U16 under ASan/UBSan, 0 failed checks; option-0 diagnostics hash
10559851243095114257 equals 1a51eac's; option-on hash with the noise controls off 3709073732580116122
equals the pre-gate header's; 11 mutants each detected by their case (floor-sqrt U3, frozen gain U4,
unit-mixed carry U6, gain percentile U9, zero-quartile gain U11 (twice), no re-seed U12, gate ignored
U14, hold ignored U15, latch kept through the hard-cut chain U15, accessor reading the ring state U14).
On 12 Nov, before the noise controls, the same tests gave 1,200 comparisons / 1,955 segments, U1-U12 and
7 mutants. The scripts under `$S` are not in the repository; the replay README has the driver usage.

## Noise controls (opt-in, 13 Nov; default off)

**Recommendation: keep both controls opt-in (Kconfig default n).** The setting the Kconfig option uses
(gate 300 + hold) costs 99 errors over the four scored recordings (ee4c +8.75 points) and does not reduce
false words on noise; gate 135 is WER-neutral in-sample (-4 errors of 2,614 words) and removes only 6-11 s
of noise per recording (39 s on nb1 with the hold); the hold alone costs 8 errors on 076a. Numbers below.

Port of the "robustness-first" design's two noise controls onto this energy-domain onset. Both are
`SegmenterConfig` fields, ignored while `decision_pre_emphasis` is 0; with both at 0 the option is the
12 Nov onset bit for bit (U13 hash over every option-on fixture; replay `--trace` files of all six
board recordings and the night proxy byte-identical to the pre-gate binary, production and option 0).

- `decision_modulation_ratio_pct` (0 = off, 101..1000; reference 300). Every frame (calibration, idle
  and active) puts the pre-emphasised energy's 16-bit code into a 50-frame ring. With the ring full,
  p10/p90 are ranks 5/45 (`nth_element` on a 100-byte stack copy) and the gate is open when
  `p90 x 10^4 > R^2 x max(p10, 1)` (an RMS ratio R/100, exact 64-bit integers, overflow-guarded). While
  it is closed the pre-emphasised level does not vote: no onset, not voiced, counted silent, no
  quiet-cut carry. Fewer than 50 frames: closed.
- `decision_pre_hold_requires_raw` (0/1). `raw_heard_` latches when a frame meets the raw onset rule
  (seeded, `raw_e >= stop_r` and `>= speech floor^2`) and is cleared when a phrase ends. At a hard limit
  it carries into the continuation only if the released part itself reached the raw rule, so after a
  talker who reached the raw floor, steady noise keeps raw votes for at most one more continuation
  (review fix, 13 Nov; before it, one raw latch anywhere in a hard-cut chain kept raw votes for the
  whole chain). Until it latches, raw energy does not vote voiced/not-silent and does not count as
  quiet-cut carry (the reference left the raw carry ungated; here it follows the same rule).
- `Segmenter::decision_modulated()` (and the replay trace's `modulated` column) is whether the
  pre-emphasised level votes in the frame: 1 on every frame while the gate is off (review fix, 13 Nov;
  it used to read 0 everywhere in a hold-only arm). `decision_raw_heard()` / `raw_heard` is the latch.
- Kconfig `CAPTIONS_DECISION_MODULATION_GATE` (default n, depends on the onset) sets 300 and the hold in
  `main.cpp`: a noisy-room mode that trades far and quiet speech for less noise (word scores below).
  Replay arms: `--set decision_modulation_ratio_pct=300 --set decision_pre_hold_requires_raw=1`.
- RAM: +112 bytes per `Segmenter` on RV32 (`riscv32-esp-elf-g++` 14.2, sizeof 4360 -> 4472; config
  60 -> 64), +104 on the host (4496 -> 4600); 100 bytes of extra stack in `track_modulation`. No float.
  Per frame with the gate on: a 100-byte copy, two `nth_element` over 50 and one 64-bit divide.

Unit cases: U13 (production hash unchanged with the controls off), U14 (steady HF noise faded in after
20 s: production 20.2 s inside, gate 0 segments), U15 (abrupt steady HF noise, and a quiet talker into
steady white noise: gate alone 20.7 / 17.6 s inside, gate + hold 1.4 / 1.2 s, talker covered; and a
talker who reaches the raw floor (raw p90 68) into the same noise: production and gate 26.5 s inside,
gate + hold 16.9 s, at most two 10 s phrases; 26.5 s before the latch fix), U14 also checks the
accessor (pre path votes on 7000/7000 frames with the hold alone, 40/7000 with gate 300 in steady HF
noise), U16 (quiet syllabic speech, day and night room: gate and gate + hold cover at least what
production covers, chunk invariant). Mutants: gate ignored -> U14 fails; hold ignored -> U15 fails;
latch kept through the hard-cut chain -> U15 fails; accessor reading the ring state -> U14 fails.
Boundary audit modes 10-11 add the controls (1440 chunk comparisons).

Host coverage (`coverage.py --replay`, no decoding; speech outside s / noise items inside s):

| Recording | production 97/150 | gate 300 | gate 300 + hold | gate 250 + hold | option 0 |
|---|---|---|---|---|---|
| ee4c9cea | 10.63 / 30.78 | 70.49 / 3.66 | 74.27 / 3.66 | 46.63 / 4.17 | 201.62 / 0.00 |
| 076aff4a | 6.12 / 36.11 | 27.46 / 4.26 | 30.91 / 2.76 | 26.29 / 6.09 | 173.95 / 5.09 |
| 63c8c20c (nb1) | 0.69 / 183.94 | 1.28 / 135.47 | 1.28 / 65.53 | 1.10 / 72.23 | 4.10 / 32.06 |
| 7c9393cb (close) | 0.12 / 20.00 | 0.42 / 20.00 | 0.42 / 20.00 | 0.42 / 20.00 | 0.66 / 20.00 |

The gate gives back much of the far-speech gain on ee4c (ami-one 0.76 -> 28.35 s, ls-quiet 8.04 ->
33.50 s outside at 300 + hold) while removing most noise. The latch fix changes no segment of any board
recording or the night proxy for any hold arm (hold alone, 110..400 + hold; replay JSON compared), so
the coverage above and the word scores below stand. Not run on the device.

### Measurements (13 Nov, host replay; deployed gen2-25397 + meeting.lm)

Coverage sweep: production, option 0, hold alone and ratios 110..400 with and without the hold on the five
board recordings and the night proxy. Limit used: at most 1.0 s more speech outside than production on
any recording. Ratios 110 and 120 are inert (identical to production everywhere); 125, 130 and 135 meet
the limit. 135 is the largest ratio within it, chosen on these same recordings, 5 ratio points from a
cliff: at 140 ee4c loses +4.97 s, at 145 076a +4.87 s.

Word error rate (%; S/D/I errors; scorer `tools/sttraw_ab/score.py` with the 10 Nov exponent-sign fix,
caption cache copied outside the repository):

| Arm | ee4c9cea (960 w) | 076aff4a (960 w) | 63c8c20c nb1 (321 w) | 7c9393cb close, 240 s (373 w) | errors, all four |
|---|---|---|---|---|---|
| option 0 | 74.58 | 71.25 | 28.35 | 20.64 | 1568 |
| production 97/150 | 52.50 | 45.21 | 26.79 | 21.18 | 1103 |
| hold alone | 52.50 | 46.04 | 26.79 | 21.18 | 1111 |
| gate 135 | 52.19 | 45.10 | 26.48 | 21.45 | 1099 |
| gate 135 + hold | 52.19 | 45.42 | 25.55 | 21.45 | 1099 |
| gate 300 + hold (Kconfig) | 61.25 | 46.56 | 27.41 | 21.18 | 1202 |

False words on nb1 noise items (segments / words): production 4 / 8, gate 135 3 / 9, gate 135 + hold
3 / 9, gate 300 + hold 6 / 8, option 0 2 / 4. Noise inside segments (s), ee4c / 076a / e8f9 / nb1:
production 30.78 / 36.11 / 46.20 / 183.94; gate 135 20.96 / 30.24 / 35.83 / 173.11; gate 135 + hold
20.92 / 30.19 / 35.83 / 144.52; gate 300 + hold 3.66 / 2.76 / 9.13 / 65.53.

- The hold alone changes no word on ee4c, nb1 or close and costs 8 errors on 076a (clips 2, 11, 55, 56;
  clip 55 is an ls-quiet clip production already gets only 3 of 14 words of). Gate 135 + hold equals
  gate 135 on ee4c and close, is +3 errors on 076a and -3 on nb1, and removes another 29 s of nb1 noise.
  All of gate 135 / 135 + hold's differences are a few errors per recording, in-sample.
- Synthetic steady noise over real P4 room tone (48 streams, `gatemeas/steady*`, not decoded): at RMS 10
  white or HF noise every ratio cuts noise inside from 19-29 s to 1-2 s, but with a talker under the noise
  every ratio, 135 included, reverts toward option 0 (day white10: speech outside 4.0 s production, 45.1 s
  gate 135, 54.5 s option 0; night white10 45.0 -> 67.2 s). There the talker is 2-4 dB BELOW the added
  noise after pre-emphasis (total power over its speech frames; median frame 8-14 dB below) although
  5-8 dB above it raw. RMS 25 is not arm-invariant. Steady noise present from boot opens nothing in
  any arm (except night LF: 31.5 s, 26.8 s at gate 300), and with the day talker every arm keeps the
  whole stream open (speech outside 3.95-6.55 s, noise inside 53.0 s). But abrupt white or HF noise
  without a talker drops from 18.7-19.7 s inside to 1.4 s (day) and from 28.8 s to 19.1-19.7 s (night)
  only with gate + hold (gate alone or hold alone: unchanged), and abrupt LF noise from 199.7 to 55.9 s
  (day) and 75.1 to 37.7-40.1 s (night) with the hold, with or without the gate. Since the latch fix, a
  quiet night talker under abrupt RMS-25 white or HF noise loses 70-86 s of its 114 s under the noise with
  gate + hold (production 8.2 s): the hold now ends phrases the noise alone was holding open, and the
  talker is 10-12 dB below the noise after pre-emphasis. Bursty 4 kHz noise: gate 135 does nothing;
  300 + hold cuts it to about 114-132 s (production 197 s), but a talker under it loses +13.8 to +18.2 s
  more speech outside phrases than production by day and +14.0 to +20.8 s by night
  (`$S/nohw/gatemeas/steady_fix.txt`; day RMS-10 bursts: 4.19 -> 22.36 s outside).
- Babble: day babble-only 59 phrases / 287 s at production and gate 135, 0 at gate 300 (with or without
  the hold), at a cost of about 13 points of talker coverage in day babble; night babble-only 25 phrases
  / 119.1 s at every arm.
- e8f93103 (PDM x8, 13 Nov, see below): option 0 56.67, production 43.44, gate 135 44.17, gate 300 + hold
  44.79 (HW1 judge 97 43.65); no false words on noise or silence in any arm.
- Not done: no device run; the night proxy not word-scored; synthetic streams not decoded.

## Word error rate (13 Nov, host replay)

Deployed recognizer gen2-25397 (int8, `2^-24` frontend) with `meeting.lm`, CTC prefix beam search, the
scorer `tools/sttraw_ab/score.py` from the main checkout (with its 10 Nov exponent-sign fix), segments from
`private/segmenter_replay/segmenter_replay`. All four recordings were captured at PDM x1 with the 36 Hz
high-pass (`pdm_amplify` 1 in each sidecar). "HW1 judge 97" is the stored arm from the same scorer and
decoder (HardwareOne snapshot replay `--decision-pre 97`, re-decoded 10 Nov): it is what HardwareOne runs
today with its default `sttdecisionpre` 97. WER % (substitutions / deletions / insertions):

| Recording (words) | option 0 | production 97/150 | gain 100 | HW1 judge 97 |
|---|---|---|---|---|
| ab1 `ee4c9cea` (960) | 74.58 (65/649/2) | **52.50** (210/288/6) | 53.65 (210/299/6) | 52.60 (210/288/7) |
| ab1 `076aff4a` (960) | 71.25 (82/597/5) | **45.21** (189/231/14) | 45.83 (188/237/15) | 46.98 (196/240/15) |
| nb1 `63c8c20c` (321) | 28.35 (63/26/2) | **26.79** (55/28/3) | 26.79 (53/29/4) | 28.35 (56/32/3) |
| close `7c9393cb`, first 240 s (373) | 20.64 (43/31/3) | 21.18 (46/30/3) | not run | 21.45 (41/35/4) |
| errors, all four (2,614 words) | 1,568 | **1,103** | | 1,127 |

- Against option 0 (the raw-only detector) production removes 465 of 1,568 errors (-29.7%), almost all of
  them deletions: ee4c 649 -> 288, 076a 597 -> 231. Clips with no caption at all: ee4c 23 -> 2, 076a 19 -> 2.
  Substitutions rise (ee4c 65 -> 210) because the recognizer now hears far speech it transcribes poorly
  (ee4c ami-one 79.3%, ls-quiet 53.9%, syn-quiet 55.6% at production).
- Close range costs +2 errors (20.64 -> 21.18), within noise for 373 words.
- Gain 150 against 100: 17 fewer errors on the three recordings scored with both (ee4c 515 -> 504,
  076a 440 -> 434, nb1 86 -> 86). Gain 150 was picked on ee4c coverage (with an nb1 silence check); 076a and
  close were not used to pick it, and 076a carries the largest gain (-250 errors).
- Against HardwareOne's own judge at 97 the gain is small: -24 errors over the four (ee4c -1, 076a -17,
  nb1 -5, close -1). The value of this design over the judge is that it does not keep the judge's three
  defects (above): with the loudest 0.5 s copied over the start of each recording, the judge leaves
  159.78 / 156.06 s of ee4c / 076a speech outside every phrase, this design 10.63 / 6.12 s, unchanged.
- False words on nb1 noise items (segments / words): option 0 2 / 4, production 4 / 8, HW1 judge 5 / 9.
  No arm put words on silence items; ab1 recordings have no false words on noise in any arm.
- Audio sent to the recognizer (s, scorer tail 8000 samples, option 0 -> production): ee4c 141.7 -> 383.6 (x2.7), 076a 180.0 ->
  402.8 (x2.2), nb1 138.8 -> 314.1 (x2.3), close 179.7 -> 187.5 (x1.04).
- `e8f93103` (PDM x8, 120 Hz high-pass, the configuration HardwareOne's automatic gain starts at; ab1,
  960 words), word-scored 13 Nov with the worktree's fixed `score.py` (`$S/nohw/wer-e8f9/`), WER % (S/D/I;
  speech outside segments s; audio sent to the recognizer s):

  | Arm | WER | S/D/I | errors | outside | audio |
  |---|---|---|---|---|---|
  | option 0 | 56.67 | 165/375/4 | 544 | 74.58 | 279.8 |
  | production 97/150 | 43.44 | 200/209/8 | 417 | 2.58 | 418.6 |
  | gate 135 | 44.17 | 200/215/9 | 424 | 2.69 | 404.7 |
  | gate 300 + hold (Kconfig) | 44.79 | 191/234/5 | 430 | 16.29 | 351.2 |
  | HW1 judge 97 (stored) | 43.65 | 189/220/10 | 419 | 4.62 | |

  Option 0 reproduces the stored raw-only score exactly (same 75 caption texts, same alignment), which
  also confirms the worktree scorer decodes like the main checkout's. Production equals the judge within
  2 errors (fewer deletions, more substitutions); gate 135 is +7 and gate 300 + hold +13 errors. No arm
  puts words on noise or silence items. One recording, PDM x8 without HardwareOne's within-phrase leveler.

### With the recognizer log guard at `2^-30` (x8 make-up gain, `wer-x8`)

Same segments, but the audio handed to the recognizer only is multiplied by 8 (saturating; 0 clipped samples
on all four recordings). After per-bin CMVN that equals the `2^-30` log guard (`docs/STT_LOG_GUARD.md`) on
the deployed gen2-25397 calibration. It is not the `voice-model-v3-blend-g30` package, which has not been
scored on these recordings.

| Recording | production 97/150 | production, x8 | option 0 | option 0, x8 |
|---|---|---|---|---|
| ee4c9cea | 52.50 | 52.71 (195/304/7) | 74.58 | 75.21 |
| 076aff4a | 45.21 | **43.65** (184/222/13) | 71.25 | 71.35 |
| 63c8c20c nb1 | 26.79 | **25.55** (51/29/2) | 28.35 | 28.04 |
| 7c9393cb close, 240 s | 21.18 | **19.57** (41/29/3) | 20.64 | 20.64 |
| errors, all four | 1,103 | **1,080** | 1,568 | 1,574 |

The guard helps only together with the onset (-23 errors), not with option 0 (+6): the extra quiet speech
the onset admits is what sits on the old guard. False words on noise at x8: nb1 4 / 7 (production 4 / 8),
close 1 segment / 1 word (none at x1).

How the scores were run (copies in the evidence folder `$S`, not in the repository): `$S/wer/run.sh`
(arms production, off, gain100) and `$S/wer-x8/run.sh` with `score_x8.py` (production, off); results in
`$S/wer/<id>/score.json|md` and `$S/wer-x8/<id>/`. The gate arms: `$S/nohw/wer/run.sh` and
`$S/nohw/wer-hold/run.sh`; e8f93103: `$S/nohw/wer-e8f9/run.sh`. The first four wrap the main checkout's
`score.py` (copy: `$S/scorer/score.py`, with its exponent fix as `$S/scorer/score-exponent-fix.diff`) so
that its logit cache is a copy outside the repository; the e8f9 run uses the worktree's `score.py`. The
caption caches are on the drive copy only:

```sh
M=<a checkout of the main repository with its private export venv and 2^-24 frontend binary>; O=$S/wer
EV=$M/tools/stt_p4/private/export-venv/bin/python
export OMP_NUM_THREADS=1 VECLIB_MAXIMUM_THREADS=1 OPENBLAS_NUM_THREADS=1
for id in ee4c9cea:ab1 076aff4a:ab1 63c8c20c:nb1; do r=${id%%:*}; m=${id##*:}
nice -n 10 $EV -B -c "import runpy,sys; from pathlib import Path; sys.path.insert(0,'$M/tools'); import overlap_gate.g0_extract as g; C=g.Recognizer; g.Recognizer=lambda cache: C(Path('$O/caption-cache')); sys.argv=['score.py']+sys.argv[1:]; runpy.run_path('$M/tools/sttraw_ab/score.py', run_name='__main__')" \
  --raw $REC/raw_$r.pcm --sidecar $REC/raw_$r.json --manifest $REC/$m.json --out $O/$r --replay $W/$B \
  --arm off="--set decision_pre_emphasis=0" --arm gain100="--set decision_gain_pct=100"
done   # built-in arm "production" = the driver default 97/150; close adds --played-seconds 240 (ab1 manifest)
```

The worktree's own `tools/sttraw_ab/score.py` now records each arm's full `segmenter_config` and, since
13 Nov, carries the same 10 Nov exponent-sign fix (`-rec.exponent`; also in `tools/overlap_gate/caption_wer.py`),
so it decodes identically to the main checkout's scorer used for the tables above (that copy was fixed on
10 Nov, before any run here; the stored HW1 judge arms were re-decoded with the fix the same day). The
recognizer still loads the main checkout's private `2^-24` frontend binary through
`tools/overlap_gate/g0_extract.py`, which the worktree does not have, so run the worktree scorer with
`$M/tools` first on `sys.path` (as `$S/nohw/wer-e8f9/run.sh` does).

## Not proven

- **Word scores are four recordings, in-sample for the gain.** 2,614 words, all Mac-speaker playback at PDM
  x1; gain 150 was picked on ee4c. `e8f93103` (x8, HardwareOne's starting gain) is one recording
  (production 43.44 vs judge 43.65, without the leveler); the night proxy is not word-scored. The onset and
  the `voice-model-v3-blend-g30` package have never been scored together.
- **Background speech and noise.** nb1 noise items 183.94 of 190 s inside (option 0 32.06 s); +30.8 to
  +38.9 s of noise inside on the three ab1 recordings; false words on nb1 noise 2 / 4 -> 4 / 8. Babble-only
  day stream 59 phrases / 287.1 s (option 0 none). Bursty HF noise (musan 4 kHz) still holds phrases open for
  as long as it lasts. The opt-in noise controls above do not fix this without giving back far speech.
- **No real night-quiet continuous recording.** The night proxy scales the room down with the speech.
- **No live talker.** All recordings are Mac-speaker playback; live-talker evidence is synthetic.
- **Level margin.** Speech 3 dB quieter than ee4c fails the 12 s bound (21.59 s).
- **HardwareOne's gain chain.** Only `e8f93103` went through PDM x8 with the 120 Hz high-pass (scored above);
  none went through the within-phrase leveler that HardwareOne applies before its segmenter, and no glasses
  recording has been replayed with the onset. Steps: `docs/STT_BOARD_TEST_PLAN.md` section 6 (H1 needs no
  board: two more x8 recordings and three glasses recordings already on the drive).
- **No on-device run.** CPU per frame, `counters.dropped`, capture-task stack high-water mark, the decode
  queue under x2.2-2.7 the audio and 3-10x the hard cuts are unmeasured on the P4. The tree has been built
  with ESP-IDF 5.5.5 (13 Nov, `docs/STT_ENGINE_INTEGRATION.md` section 10) but not flashed.
- Host tests do not prove SD/FAT behaviour, model accuracy or P4 real-time performance.
- `docs/stt-source-parity*.json` hash for `stt_segmenter.h` was already stale and is not regenerated
  (needs a HardwareOne checkout).

Porting into HardwareOne (which already ships a different pre-emphasis judge): `docs/STT_ENGINE_INTEGRATION.md`.

## Switching it off

- Device: `CONFIG_CAPTIONS_DECISION_PRE_EMPHASIS=n` (menuconfig), or append `sdkconfig.prev_onset` to
  `SDKCONFIG_DEFAULTS`. Use this to reproduce device results recorded before 12 Nov 2026.
- Engine: leave `SegmenterConfig::decision_pre_emphasis` at 0 (the default); behaviour is then the
  1a51eac header's, bit for bit (G1, U1 hash).
- Replay: `--set decision_pre_emphasis=0`.
