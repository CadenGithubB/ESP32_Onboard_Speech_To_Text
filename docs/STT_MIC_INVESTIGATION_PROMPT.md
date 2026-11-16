# Prompt: investigate and improve the microphone path on the P4

Give this to the agent working in the HardwareOne checkout.

---

You are investigating how to get better speech audio out of the ESP32-P4's microphone path for
on-device captions. The goal is measurable: fewer caption errors, especially fewer dropped words
and fewer quiet talkers missed entirely. Do not change the recognition model. Everything in front
of it is in scope: hardware, clocking, the PDM conversion, the speech detector and endpointing,
any signal processing, placement and enclosure.

## What has already been measured (standalone speech project, 6 Nov 2026)

The evidence lives in the standalone STT project, on this Mac at
`~/Documents/ChatGPT/HardwareOne_Speech_To_Text`. It is also on GitHub at
`https://github.com/CadenGithubB/ESP32_Onboard_Speech_To_Text`, branch
`claude/spkeval-diagnostics`. Clone it read-only next to your checkout if you need it; don't work
inside the user's copy. Its `private/` results and the audio on `/Volumes/Audiodrive` are local
only, not on GitHub. Read these:
- `docs/STT_OVERLAP_GATE_PHASE4_PLAN.md` sections 13–17;
- `docs/STT_PRODUCT_HANDOFF.md`, the end of the file;
- the caption scoring in `tools/overlap_gate/caption_wer.py`.
Read them before you start. Do not copy that project's code into HardwareOne automatically. Port
only what you need, and say what you ported.

**The microphone path in the standalone example** (`firmware/examples/p4_eye_captions/main/main.cpp`,
around line 160):
- a PDM microphone on I2S0 PDM RX, mono, 16-bit PCM at 16 kHz;
- `dn_sample_mode = I2S_PDM_DSR_16S`, which doubles the PDM clock to 2.048 MHz. A comment there
  says 1.024 MHz falls in the mic's gap between its low-power and standard bands;
- mic power switched by a GPIO latch on a shared rail;
- the default PDM RX slot config: no explicit high-pass filter or amplify settings;
- the first three blocks discarded at start.

Check what HardwareOne's own firmware actually does. It may differ.

**Levels at the P4:**
- Normal speech at about 1 m (live and through a speaker) arrives at about −60 dBFS. The P4's
  earlier "p4-ordinary" recordings are the same.
- The quietest 20 ms frames inside speech segments sit around −78 dBFS, so speech is only about
  18 dB above the floor.
- For comparison, the AMI far-field corpus is about −43 dBFS and a close recording about −26 dBFS.

**Digital gain does not help, and the reason matters.** Raising the samples by +9 to +18 dB
changed nothing for the overlap classifier. Gain applied after the floor raises noise and speech
together; only the signal-to-noise ratio at capture matters. The recognizer also normalizes each
utterance's level itself. So "auto gain" in the plain digital sense is not the lever. Raising the
SNR, or catching quiet speech that is currently thrown away, is.

**Caption error rates through the P4** (Mac mini speaker about 1 ft from the P4, playlist audio):

| Played audio | WER | Words dropped | Clips with no caption at all |
|---|---|---|---|
| Clean read speech | 12% | 2% | 0% |
| Ordinary single speaker | 18% | 5% | 0% |
| The same, 18 dB quieter | 88% | 83% | 66% |
| AMI meeting speech, no overlap | 60% | 35% | 6% |
| AMI meeting speech, overlapped | 71% | 42% | 10% |

- Dropped words are the dominant error everywhere.
- Quiet talkers mostly never become a segment at all.

**What the P4 audio path costs.** The same host decoder (the deployed int8 model plus the P4's
`meeting.lm` word LM, the HW1LM1 host reference) was run on the original clip files and on the P4's
own saved segment audio. The only difference is the audio. On the P4 audio, the host decoder
reproduces the device's own captions: 2,525 of 2,540 night-1 clips have identical text.

| Played audio | Original file | Through the P4 | Words dropped: file → P4 | Dropped in clips with no segment |
|---|---|---|---|---|
| Clean read speech | 4.7% | 12.3% | 0.3 → 1.6% | 0% |
| Ordinary single speaker | 9.2% | 18.4% | 1.9 → 4.7% | 0% |
| The same, 18 dB quieter | 9.4% | 89.6% | 1.6 → 84.7% | 64.1% |
| AMI meeting, no overlap | 32.8% | 60.1% | 13.2 → 35.4% | 3.2% |
| AMI meeting, overlapped | 52.9% | 71.4% | 20.2 → 42.2% | 7.2% |

Re-scored 11 November 2026 with `meeting.lm` decoded at the correct exponent sign. Before the fix, the host LM saw logits 4× too large (`tools/overlap_gate/caption_wer.py` passed −1 where `hw1lm.decode_int8` expects +1). The pre-fix WERs differed by −0.2 to +1.8 points, mostly higher. With the correct scale the LM deletes more words, so the dropped-words column rose, for example from 28.1% to 35.4% on single-speaker AMI through the P4. The conclusions below are unchanged. The "no segment" column does not depend on the decoder.

- About 27 WER points on single-speaker meeting speech are lost between the file and the P4.
  Clean speech more than doubles its error.
- Quiet speech is fine in the file (9%) and nearly lost on the P4 (90%). Most of that loss is
  clips the detector never segmented.
- On meeting speech, most extra deletions are not whole missed clips. They are inside segments
  (quiet stretches, clipped word starts and ends) or the recognizer dropping words in noisy audio.
  The next diagnostic: measure how much of each clip's reference speech time lies outside every
  device segment.
- Data: `private/training-track/overlap-gate/phase4/caption-wer.json` in that repo, with fields
  `clean_lm` and `p4pcm_lm` per clip.

The device's own rawText matches an offline host decode of the saved PCM. With the exponent sign
fixed, 200 of 200 checked segments are identical, and 2,525 of 2,540 clips agree. The earlier
61-of-200 mismatch came from the host-side sign bug, not from streaming or speculative drafts.
Compare A/B arms with one decoder anyway, so detector changes are not confused with decoder changes.

## What to investigate, in order of expected payoff

1. **The noise floor at capture.** Record with the mic port sealed, with it open in a quiet room,
   and with Wi-Fi, display, camera and SD writes on and off.
   - Find out whether the −78 dBFS floor is acoustic (room, fans) or electrical: rail noise on the
     shared mic supply, clock jitter, digital coupling, DC offset, hum.
   - Look at the spectrum, not just the RMS.
   - An electrical floor is fixable in hardware or layout and is a direct SNR win.
2. **The mic's operating point.** Identify the exact mic part from the schematic or BOM and read
   its datasheet.
   - Confirm that the 2.048 MHz clock puts it in its full-performance mode, not a low-power mode
     with worse SNR.
   - Check the decimation path: the PDM RX high-pass filter (`hp_en`, cut-off), `amplify_num`,
     DC removal, and whether any bits are lost in the 16-bit conversion.
   - Check the sensitivity and acoustic overload point, and whether the port and gasket are
     sound.
3. **Speech detection and endpointing.** Quiet speech fails here first: 66% of quiet clips never
   produced a segment.
   - Find out how the detector sets its threshold (the log prints "noise floor N, level M").
   - Test whether a lower or better-adapted threshold, a hangover, or pre-roll catches quiet
     talkers and word starts and ends without flooding captions with noise.
   - This is pure firmware and probably the biggest single win for dropped words.
4. **Front-end signal processing**, only if it measures better: a high-pass filter, light
   stationary noise suppression, or ESP-SR's AFE for the P4 (NS/AGC; beamforming needs two mics).
   - The recognizer was trained on unprocessed audio, so processing can easily hurt. Every change
     must be A/B tested on caption WER, not judged by ear.
5. **Physical and hardware options.**
   - Mic port position and orientation, enclosure resonance, distance to the talker.
   - A higher-SNR mic part, around 70 dB or more.
   - A second mic for beamforming or noise reference.
   - Estimate the cost and size of each.

## How to measure: required for every change

- Use the same playback bench and score captions. In the standalone repo:
  - `tools/overlap_gate/night_driver.py` plays playlist blocks through the Mac mini speaker at
    volume 70 into the P4 about 1 ft away, saves every segment's PCM to the card, records full
    serial logs and restores production;
  - `tools/overlap_gate/live_session.py` runs a cue-screen session with a live talker;
  - `tools/overlap_gate/caption_wer.py` scores captions against the playlist text.
  - Port or reuse what you need. Keep the playback level, distance and room fixed between A and B,
    run A and B interleaved, and score both on the same clips.
- Report for each condition:
  - WER, split into substitutions, deletions and insertions;
  - the share of clips with no caption at all;
  - the noise floor and speech level at capture;
  - and, for detector changes, false captions on noise-only clips.
- Also record live, two-person speech before claiming a real-world win; speaker replay is not the
  same as a person in the room.

## Rules

- Follow the HardwareOne checkout's own AGENTS.md and handoff rules.
- Before any flash, inspect the actual partition table and keep a private rollback image. Flash
  only the app at the verified address.
- Keep audio, logs and flash backups in ignored private directories or on
  `/Volumes/Audiodrive`, and never commit them.
- Do not change the recognition model or the speaker algorithms. Do not add a punctuation model.
- Large data goes on `/Volumes/Audiodrive`, not the internal disk.
- Write results, including what did not work, into that project's handoff document.
