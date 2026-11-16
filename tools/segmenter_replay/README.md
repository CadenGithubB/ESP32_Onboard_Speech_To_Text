# Segmenter replay on saved raw recordings

Host tools that run this repository's speech segmenter
(`firmware/components/hw1_stt/stt/stt_segmenter.h`) over a continuous raw microphone recording and
measure how much of the played speech ends up inside a phrase. Nothing is decoded: a ten-minute
recording replays in about 0.2 s, so a detector setting can be checked before any caption scoring.

| File | Purpose |
|---|---|
| `segmenter_replay.cpp` | One-file driver: the example's segmenter settings, any field overridable, JSON phrases out. |
| `coverage.py` | Speech outside every phrase, noise/silence captured, phrases per minute, hard-limit cuts; differences between arms. Standard library only. |
| `make_proxy.py` | Attenuates a scored recording to a chosen speech level (the night-level proxy). Needs a Python with numpy. |
| `../../tests/test_segmenter_replay.py` | Checks the driver against `main.cpp`, block-size invariance and the coverage arithmetic. |

Recordings are `sttraw` files: s16le mono 16 kHz, exactly what the segmenter received, with a
`raw_<id>.json` sidecar. Each scored recording has a `score-*/score.json` (written by
`tools/sttraw_ab/score.py`) that names the playlist manifest and holds the alignment and
`played_seconds`; the coverage tool reads those three and nothing else from it. Keep recordings and
outputs outside Git (`private/` or the audio drive).

## Build

From the repository root (about 2 s, one file):

```bash
mkdir -p private/segmenter_replay
clang++ -std=c++17 -O2 -Wall -Wextra -Werror -Ifirmware/components/hw1_stt \
  tools/segmenter_replay/segmenter_replay.cpp -o private/segmenter_replay/segmenter_replay
```

## Replay

```bash
R=/Volumes/Audiodrive/stt/sttraw/stt10-ab1
B=private/segmenter_replay/segmenter_replay
$B $R/raw_ee4c9cea.pcm --sidecar $R/raw_ee4c9cea.json > private/segmenter_replay/ee4c9cea.base.json
$B $R/raw_ee4c9cea.pcm --set adaptive_speech_floor=24 --set adaptive_silence_floor=8 > private/segmenter_replay/ee4c9cea.f24.json
$B --print-config      # settings in effect, struct defaults, block, inner, tail
$B --list-fields       # every name --set accepts
```

- **Settings.** `exampleConfig()` repeats the `cfg.segmentation.*` lines of
  `firmware/examples/p4_eye_captions/main/main.cpp` (adaptive, 4 s minimum, 10 s limit, 1 s short close,
  noise percentile 25, 30 ms blip, 2 s quiet-cut search; floors 45/16 from the struct defaults) and the
  level-robust onset that `CONFIG_CAPTIONS_DECISION_PRE_EMPHASIS` (default y) adds:
  `decision_pre_emphasis=97`, `decision_gain_pct=150`. The test parses `main.cpp` (including the lines
  inside that `#if`) and fails if the two differ, so a change in the example must be repeated there.
- **The production arm (the detector before 12 Nov) is now `--set decision_pre_emphasis=0`.** With it
  the driver's phrases, JSON (apart from the config blocks) and `--trace` file are identical to those of
  the header at commit 1a51eac; `--check production` needs it. The option requires adaptive mode, so a
  fixed-threshold experiment needs both: `--set adaptive=0 --set decision_pre_emphasis=0` (`--set
  adaptive=0` alone is rejected by `valid()`).
- **Noise controls (opt-in).** `decision_modulation_ratio_pct` (0 off, 101..1000) and
  `decision_pre_hold_requires_raw` (0/1) are off in `exampleConfig()` (their Kconfig
  `CAPTIONS_DECISION_MODULATION_GATE` defaults to n; the test ignores `main.cpp` lines under default-n
  options). Arm: `--set decision_modulation_ratio_pct=300 --set decision_pre_hold_requires_raw=1`. With
  either on, `--trace` adds `modulated,raw_heard` columns: whether the pre-emphasised level votes in that
  frame (1 on every frame while the gate is off) and whether raw votes count (the hold's latch; 0 on every
  frame while the hold is off). See docs/STT_LEVEL_ROBUST_ONSET.md.
- **`--set FIELD=VALUE`** takes any `SegmenterConfig` field by its C++ name. Values are integers
  (`0/1/true/false` for flags); `600ms` and `2s` are converted to samples. A new field needs one more
  `X(name)` line in `SEGMENTER_CONFIG_FIELDS`; the build fails until it is added.
- **Blocks.** `--block N` is the microphone read size (default 1024, `kReadSamples`). Each block is
  pushed in pieces of at most `--inner` samples (default 160, as `StreamPipeline::push` does); `--inner 0`
  pushes whole blocks. Segments must not depend on either.
- **`--tail N`** (default 8000, the latency profile's `CONFIG_CAPTIONS_TRIM_SILENCE`; 0 for the lean
  profile) only sets each phrase's `decode_end`, the end of the recognizer's input by
  `StreamPipeline::inputSamples`: voiced end + tail for a pause close, the whole phrase for a hard-limit
  cut or a phrase with no voiced end.
- **`--trace frames.csv`** writes `sample,rms,noise,threshold,open,active,peak,ready` per 10 ms frame, as
  the segmenter's getters report them after that frame and before a ready phrase is released. With
  `decision_pre_emphasis` on, six more columns follow: `dec_e,dec_noise_e,dec_thr_e,raw_e,raw_thr_e`
  (energies in 10^-4 count^2, the units the decision compares) and `gain_q16` (the learned ambient energy
  gain, 65536 = 1.0). `noise`, `threshold` and `peak` are then square roots of those energies (display
  only); `rms` stays the raw floor-sqrt level.
- **`--cut-dir DIR`** writes each phrase as `seg_<n>.pcm`.
- **Self-check.** Every phrase is compared with the file's samples at `[start, end)` and must not
  redeliver audio; a violation exits with status 3.

The JSON has the keys of HardwareOne's `stt_raw_replay` (`file`, `samples`, `seconds`,
`sidecar_dropped`, `config`, `segments_covered_samples`, and per phrase `n`, `start`, `end`, `end_kind`,
`continuation`, `voiced_end`, `new_voiced`) plus `segmenter_config`, `block`, `inner`, `tail_samples`,
`hard_limit_cuts` and per phrase `samples`, `fresh_start`, `decode_end`. HardwareOne's option
spellings `--speech-floor`, `--silence-floor`, `--noise-percentile`, `--blip-ms`, `--no-short-close`
and `--chunk` are accepted.

## Coverage

```bash
S=$R/score-base/score.json          # the score.json that belongs to raw_ee4c9cea.pcm
$B $R/raw_ee4c9cea.pcm --sidecar $R/raw_ee4c9cea.json --set decision_pre_emphasis=0 > private/segmenter_replay/ee4c9cea.prod.json
python3 tools/segmenter_replay/coverage.py --score $S private/segmenter_replay/ee4c9cea.prod.json --check production
python3 tools/segmenter_replay/coverage.py --score $S \
  private/segmenter_replay/ee4c9cea.base.json private/segmenter_replay/ee4c9cea.f24.json --name base,floor24
# or let it run the driver, one arm per --arm (the raw file and manifest come from the score.json):
python3 tools/segmenter_replay/coverage.py --score $S --replay $B \
  --arm prod="--set decision_pre_emphasis=0" --arm example="" --arm gain100="--set decision_gain_pct=100" \
  --json private/segmenter_replay/ee4c9cea.cov.json
```

Per arm it prints playlist speech outside every phrase (seconds and percent, total and per
category; the same sum as `score.py`'s `outside_s`), speech clips with no phrase and clips under 90%
covered, seconds of the noise and silence items inside phrases, phrases owned by a noise item, a
silence item or no item (`score.py`'s `assign()`: where text would be a false caption), phrases,
phrases per minute, hard-limit cuts, seconds inside phrases and seconds sent to the recognizer. Every
later arm is also printed as a difference from the first. `--check ARM` compares the first arm with
that arm of the score.json (phrase count and outside seconds per category) and exits 1 on a mismatch.
`--raw` and `--manifest` override the paths stored in the score.json (a local copy, or the proxy).

## Night-level proxy

No continuous raw recording exists from a night-quiet room. Until one does, the close-range x1
recording attenuated by 12 dB stands in: its speech-frame 90th percentile drops from 177 to 44 counts,
the level of the night-1 segments.

```bash
C=/Volumes/Audiodrive/stt/sttraw/stt10-close
python3 tools/segmenter_replay/make_proxy.py --score $C/score-fixedx1/score.json --db -12 \
  --out private/segmenter_replay/raw_7c9393cb_night12db.pcm
python3 tools/segmenter_replay/coverage.py --score $C/score-fixedx1/score.json --replay $B \
  --raw private/segmenter_replay/raw_7c9393cb_night12db.pcm --arm base=""
```

`out = round(in x 10^(-12/20))`, nothing else; positions are unchanged, so the original score.json
still applies. The room is scaled down with the speech (silence items: median 8 counts before, 2 after),
so this room is quieter than a real one and says nothing about noise pickup at night.

## Reference numbers (12 Nov 2026, `--set decision_pre_emphasis=0`, i.e. the header at commit 1a51eac)

| Recording | Playlist, capture | Phrases | Per minute | Hard-limit cuts | Speech outside | Noise / silence items inside |
|---|---|---|---|---|---|---|
| `stt10-ab1/raw_ee4c9cea` | ab1, x1 36 Hz, 1 ft | 40 | 3.78 | 1 | 201.62 s of 346.34 (58.2%) | 0.00 / 0.00 s of 60 / 60 |
| `stt10-ab1/raw_076aff4a` | ab1, x1 36 Hz, 1 ft | 43 | 4.06 | 0 | 173.95 s (50.2%) | 5.09 / 0.00 s |
| `stt10-ab1/raw_e8f93103` | ab1, x8 120 Hz, 1 ft | 75 | 7.08 | 0 | 74.58 s (21.5%) | 7.26 / 1.41 s |
| `stt10-nb1/raw_63c8c20c` | nb1, x1 36 Hz | 26 | 3.33 | 3 | 4.10 s of 108.84 (3.8%) | 32.06 / 0.00 s of 190 / 60 |
| `stt10-close/raw_7c9393cb` | ab1 first 240 s, x1, close | 33 | 7.75 | 5 | 0.66 s of 139.07 (0.5%) | 20.00 / 3.62 s of 20 / 20 |
| the same, -12 dB (proxy) | | 26 | 6.10 | 2 | 30.30 s of 139.07 (21.8%) | 6.96 / 0.00 s |

All five recordings give the phrase count and the per-category outside seconds stored for the
`production` arm of their score.json, and phrase lists identical field by field to HardwareOne's
`stt_raw_replay` without options (the two headers differ only in opt-in fields that default to off).

### With the level-robust onset (example settings, `decision_pre_emphasis=97`, `decision_gain_pct=150`)

| Recording | Phrases | Per minute | Hard-limit cuts | Speech outside | Noise / silence items inside | HardwareOne judge (`--decision-pre 97`): outside, noise |
|---|---|---|---|---|---|---|
| `raw_ee4c9cea` | 80 | 7.55 | 7 | 10.63 s (3.07%) | 30.78 / 0.57 s | 11.56 s, 33.28 s |
| `raw_076aff4a` | 75 | 7.09 | 10 | 6.12 s (1.77%) | 36.11 / 0.31 s | 10.89 s, 28.72 s |
| `raw_e8f93103` | 80 | 7.55 | 11 | 2.58 s (0.75%) | 46.20 / 1.47 s | 4.62 s, 46.19 s |
| `raw_63c8c20c` | 58 | 7.44 | 21 | 0.69 s (0.64%) | 183.94 / 0.07 s | 0.85 s, 185.52 s |
| `raw_7c9393cb` | 33 | 7.75 | 5 | 0.12 s (0.09%) | 20.00 / 3.65 s | 0.65 s, 20.00 s |
| proxy (-12 dB) | 31 | 7.28 | 5 | 1.76 s (1.26%) | 20.00 / 1.97 s | 2.38 s, 20.00 s |

Seconds sent to the recognizer rise with it (ee4c9cea 141.65 -> 383.62 s with the default `--tail 8000`,
the latency profile; 155.53 -> 401.28 s with `--tail 0`, the example default), and most noise items are
captured (nb1 32.06 -> 183.94 of 190 s). Coverage is not word accuracy; see the handoff document.

## Caption scoring later

`tools/sttraw_ab/score.py --replay private/segmenter_replay/segmenter_replay --arm NAME="--set field=value"`
runs this driver in place of HardwareOne's binary (it reads `segments[].start/end/voiced_end` and
`config`). It decodes `start .. min(end, voiced_end + 500 ms)` for every phrase, which is shorter than
the device's input for a phrase with no voiced end; `decode_end` is the device's rule. This
repository's `score.py` has the 10 Nov exponent-sign fix (`-rec.exponent`) since 13 Nov, and records
each arm's `segmenter_config`. Its recognizer (`tools/overlap_gate/g0_extract.py`) still needs a private
`2^-24` frontend binary that only the main checkout has, so put that checkout's `tools` first on
`sys.path` and point the decoder cache outside the repository; the command block in
`docs/STT_LEVEL_ROBUST_ONSET.md` "Word error rate" shows the wrapper. Word scores of the recordings
below are in that document.

## Limits

- Coverage is not accuracy: more audio inside phrases does not guarantee fewer word errors, and the
  noise/silence seconds only bound where false captions can arise.
- The alignment and playlist come from the score.json; a recording never scored with `score.py` has none.
- The driver is the segmenter alone: no queue, no drafts, no recognizer timing. Dropped phrases under
  load are a device measurement.
- All recordings are Mac-speaker playback captured by HardwareOne firmware; none is from the standalone
  example or a live talker.
