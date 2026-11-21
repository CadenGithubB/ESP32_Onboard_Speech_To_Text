# Models

Everything the apps need, ready to copy to a microSD card. Copy the whole `STT Models` folder to
the top level of the card.

| File | Size | What |
|---|---|---|
| `citrinet384-2000h-c90.p4.stt` | 22.7 MB | The recognizer: Citrinet-384 as an int8 graph for the ESP32-P4. The firmware is pinned to this exact file and refuses any other. |
| `meeting.lm` | 2.5 MB | The language model: word trigrams that steer the recognizer's word pieces towards likely words. Optional; captions are less accurate without it. |
| `speaker.spk` | 2.0 MB | The speaker model: tells voices apart so captions can be numbered by speaker. Optional. |
| `user_vocab.example.tsv` | | An example of your own word list. Rename it to `user_vocab.tsv` and edit it; see below. |

Check the files after copying with `shasum -a 256 -c ../SHA256SUMS` from inside `STT Models`.

## Your own words

`user_vocab.tsv` holds up to 256 words the recognizer should lean towards, one per line, written
the way they should appear in captions. When a word is said differently from how it is spelled,
add the spoken form after a tab:

```
Kubernetes
ESP32	esp
```

Spoken forms are lowercase letters and apostrophes. It is a preference, not a guarantee.

## How good the recognizer is

Word error rate on 600 held-out clips, 100 per set, without the language model. "int8" is this
file; "float" is the same model before it was shrunk for the chip.

| Speech | float | int8 |
|---|---|---|
| Read aloud (LibriSpeech) | 3.4% | 4.5% |
| Technical vocabulary (synthetic) | 7.8% | 8.5% |
| Varied recordings (People's Speech) | 9.9% | 11.3% |
| Parliament speeches (VoxPopuli) | 11.2% | 12.1% |
| Meetings, headset microphone (AMI) | 24.1% | 24.7% |
| Meetings, microphone across the room (AMI) | 37.7% | 40.9% |
| All 600 clips | 13.9% | 15.2% |

Measured on a computer, not on the board. Speech across a room is hard for it.

## Where they come from

**Recognizer.** NVIDIA's Citrinet-384 (the LibriSpeech checkpoint, `stt_en_citrinet_384_ls`),
fine-tuned for 2,000 hours of training on People's Speech, Common Voice English, AMI, LibriSpeech,
VoxPopuli and synthetic speech of technical vocabulary, with MUSAN and DEMAND noise and simulated
room echo mixed in. The result is blended 90/10 with the original NVIDIA weights, which recovers
accuracy on read speech, and exported to int8.

**Language model.** Built with `tools/stt_train/lm` from the AMI and LibriSpeech training
transcripts.

**Speaker model.** Clova AI's pretrained Fast ResNet-34 (`baseline_lite_ap`, trained on VoxCeleb2)
from [clovaai/voxceleb_trainer](https://github.com/clovaai/voxceleb_trainer), converted with
`tools/stt_p4/speaker` to 8-bit weights and 16-bit activations. It was not retrained here.

## Licences of the sources

| Source | Licence |
|---|---|
| NVIDIA Citrinet-384, AMI, LibriSpeech | CC BY 4.0 |
| VoxPopuli, Common Voice | CC0 |
| People's Speech | CC BY and CC BY-SA |
| clovaai/voxceleb_trainer | MIT, copyright NAVER Corp. |

The noise and room-echo sets used for augmentation, and the voices used for the synthetic
speech, have their own terms; see their publishers.

## Using a different model

`tools/stt_p4/package_citrinet.py` packages an int8 export of a Citrinet-384 checkpoint: it writes
the card file and a new `stt_model_identity_citrinet384.h` for
`firmware/components/hw1_stt/stt/`. Set `kModelPath` in the app to the new file name and rebuild.
