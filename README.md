# ESP32 Onboard Speech To Text

Speech to text that runs entirely on an ESP32-P4. No network, no phone, no cloud: you talk, the
board shows what you said.

```
Listening. Speak now.
            ... okay so the next thing
            ... okay so the next thing we need to look at
[   12.4 s] okay so the next thing we need to look at is the schedule
            4.6 s of audio in 1180 ms: features 14, model 1011, decode 92 ms, language model
```

## What you need

- An **ESP32-P4-EYE** board with a v3 chip (sold as ESP32-P4X-EYE) and a USB-C cable.
- A **microSD card**, FAT32 formatted. It holds the models (27 MB).
- A Mac or Linux computer.

## Get it running

Run these in order. Steps 1 to 3 are one-time.

**1. Install ESP-IDF 5.5.5** (skip if you already have it):

```bash
git clone -b v5.5.5 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32p4
```

**2. Get this repository.** The trained models come with it, in [`Pretrained Models/`](<Pretrained Models>):

```bash
git clone https://github.com/CadenGithubB/ESP32_Onboard_Speech_To_Text.git
cd ESP32_Onboard_Speech_To_Text
```

**3. Copy the `STT Models` folder onto the microSD card**, at the top level, keeping the folder
name. On a Mac, with the card mounted as `NO NAME`:

```bash
cp -R "Pretrained Models/STT Models" "/Volumes/NO NAME/"
```

Eject the card and put it in the board.

**4. Build:**

```bash
. ~/esp/esp-idf/export.sh
cd firmware/examples/screen_captions
python3 ../../tools/prepare_esp_dl.py
idf.py set-target esp32p4
idf.py build
```

**5. Plug the board in, then flash and watch.** This replaces whatever firmware is on the board.

```bash
idf.py flash monitor
```

Wait for `Listening` on the screen and talk. Grey text is a live draft; white text is the final
caption. The same captions print in the terminal. Press `Ctrl+]` to leave the monitor; the board
keeps working without the computer.

## Using it

| Control | What it does |
|---|---|
| Press the wheel | Pause and resume listening. |
| Turn the wheel | Open the settings. Turn to move, press to change, choose `Back` to leave. |

| Setting | What it changes |
|---|---|
| Mic gain | `auto` lets the board choose the microphone gain from what it hears. Or fix it at x1, x2, x4 or x8. |
| Language model | How strongly word likelihood steers the result, 0 to 100%. |
| Speaker labels | Number and colour each caption by who said it. |
| Live drafts | Show text while you are still speaking. |
| Noisy room | Stricter about what counts as the start of speech, for fans and hiss. |
| End pause | How long a silence ends a phrase: 0.4, 0.6 or 1.0 seconds. |

Settings are saved on the board. The last three restart it to take effect.

**Your own words.** Names and jargon the recognizer should prefer go in a file on the card,
`STT Models/user_vocab.tsv`. [`Pretrained Models/README.md`](<Pretrained Models/README.md>) shows the format.

## The simplest version

`simple_captions` is one source file: microphone to captions in the terminal, no screen, no
settings. It is the place to start reading the code. Build and flash it the same way from
`firmware/examples/simple_captions`.

## If it does not work

The board shows `Stopped` and the reason, and prints it every five seconds.

| Message | Fix |
|---|---|
| `no microSD card detected` | Reseat the card and press reset. |
| `microSD card did not mount` | Reformat the card as FAT32 (not exFAT). |
| `model file missing` | The card needs the `STT Models` folder at its top level. Copy it from `Pretrained Models/` again. |
| `Model load failed` | The model file is damaged or is from another version of this repository. Copy it from `Pretrained Models/` again. |
| `No /sd/STT Models/meeting.lm` | Captions still work, less accurately. Copy `meeting.lm` to the card. |
| `No /sd/STT Models/speaker.spk` | Captions still work, without speaker numbers. Copy `speaker.spk` to the card. |
| `idf.py: command not found` | Run `. ~/esp/esp-idf/export.sh` in this terminal first. |
| Flash cannot find the board | Use the USB-C port marked for USB-Serial/JTAG, or pass the port: `idf.py -p PORT flash monitor`. |

## What is in the repository

| Folder | What |
|---|---|
| [`firmware/examples/screen_captions`](firmware/examples/screen_captions) | The app above: captions on the screen, settings on the wheel, speaker labels. |
| [`firmware/examples/simple_captions`](firmware/examples/simple_captions) | The same engine in one source file, captions in the terminal. |
| [`firmware/components/hw1_stt`](firmware/components/hw1_stt) | The engine as an ESP-IDF component: audio features, the int8 Citrinet-384 runtime, the phrase segmenter, live drafts, the language model search, the speaker model, automatic microphone gain. |
| [`Pretrained Models/`](<Pretrained Models>) | The trained models, ready to copy to the card, with their accuracy and sources. |
| [`firmware/examples/p4_eye_captions`](firmware/examples/p4_eye_captions) | An older, fuller app on the earlier QuartzNet 15x5 model: saved recordings and a browser viewer. Its model is on the [releases page](https://github.com/CadenGithubB/ESP32_Onboard_Speech_To_Text/releases/tag/model-gen2-25397). |
| [`tools/`](tools) | Training, model export, packaging, and device test tooling. |
| [`docs/`](docs) | Measurements and investigation notes. |

To use the engine in your own program, see [`firmware/README.md`](firmware/README.md).
