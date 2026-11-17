# ESP32 Onboard Speech To Text

Speech to text that runs entirely on an ESP32-P4. No network, no phone, no cloud: you talk, the
board prints what you said on its serial console.

```
Listening. Speak now.
            ... okay so the next thing
            ... okay so the next thing we need to look at
[   12.4 s] okay so the next thing we need to look at is the schedule
            4.6 s of audio in 1180 ms: features 14, model 1011, decode 92 ms, language model
```

## What you need

- An **ESP32-P4-EYE** board with a v3 chip (sold as ESP32-P4X-EYE) and a USB-C cable.
- A **microSD card**, FAT32 formatted. It holds the 20 MB model.
- A Mac or Linux computer.

## Get it running

Run these in order. Steps 1 and 2 are one-time.

**1. Install ESP-IDF 5.5.5** (skip if you already have it):

```bash
git clone -b v5.5.5 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32p4
```

**2. Get this repository and the model files:**

```bash
git clone https://github.com/CadenGithubB/ESP32_Onboard_Speech_To_Text.git
cd ESP32_Onboard_Speech_To_Text
mkdir "STT Models"
curl -L -o "STT Models/quartznet15x5-gen2-25397.p4.stt" https://github.com/CadenGithubB/ESP32_Onboard_Speech_To_Text/releases/download/model-gen2-25397/quartznet15x5-gen2-25397.p4.stt
curl -L -o "STT Models/meeting.lm" https://github.com/CadenGithubB/ESP32_Onboard_Speech_To_Text/releases/download/model-gen2-25397/meeting.lm
```

**3. Copy the `STT Models` folder onto the microSD card**, at the top level, keeping the folder
name. On a Mac, with the card mounted as `NO NAME`:

```bash
cp -R "STT Models" "/Volumes/NO NAME/"
```

Eject the card and put it in the board.

**4. Build:**

```bash
. ~/esp/esp-idf/export.sh
cd firmware/examples/simple_captions
python3 ../../tools/prepare_esp_dl.py
idf.py set-target esp32p4
idf.py build
```

**5. Plug the board in, then flash and watch.** This replaces whatever firmware is on the board.

```bash
idf.py flash monitor
```

Wait for `Listening. Speak now.` and talk. Lines starting with `...` are live drafts; the
timestamped line is the final caption. Press `Ctrl+]` to leave the monitor.

## With the screen and wheel

`screen_captions` is the same engine with the board's own screen and wheel, so it needs no
computer once flashed. Build and flash it exactly as in steps 4 and 5, from its own folder:

```bash
cd firmware/examples/screen_captions
```

| Control | What it does |
|---|---|
| Press the wheel | Pause and resume listening. |
| Turn the wheel | Open the settings. Turn to move, press to change, choose `Back` to leave. |

The settings are microphone gain, language model strength, speaker labels, live drafts,
noisy-room mode and how long a pause ends a phrase. They are saved on the board. The last
three restart the board to take effect.

**Speaker labels** number each caption by who said it. They need a third file on the card,
`STT Models/speaker.spk`; without it the app runs with no labels.

## If it does not work

The board prints `STOPPED:` and the reason every five seconds.

| Message | Fix |
|---|---|
| `no microSD card detected` | Reseat the card and press reset. |
| `microSD card did not mount` | Reformat the card as FAT32 (not exFAT). |
| `model file missing` | The card needs `STT Models/quartznet15x5-gen2-25397.p4.stt`, spelled exactly. |
| `Model load failed` | The model file is damaged or is a different model. Download it again. |
| `No /sd/STT Models/meeting.lm` | Captions still work, less accurately. Copy `meeting.lm` to the card. |
| `idf.py: command not found` | Run `. ~/esp/esp-idf/export.sh` in this terminal first. |
| Flash cannot find the board | Use the USB-C port marked for USB-Serial/JTAG, or pass the port: `idf.py -p PORT flash monitor`. |

## What is in the repository

| Folder | What |
|---|---|
| [`firmware/examples/simple_captions`](firmware/examples/simple_captions) | The app above: one source file, microphone to captions. Start here. |
| [`firmware/examples/screen_captions`](firmware/examples/screen_captions) | The same, on the board's screen, with settings on the wheel and speaker labels. |
| [`firmware/components/hw1_stt`](firmware/components/hw1_stt) | The engine as an ESP-IDF component: audio features, the int8 QuartzNet 15x5 model runtime, the phrase segmenter, live drafts, and the word language model. |
| [`firmware/examples/p4_eye_captions`](firmware/examples/p4_eye_captions) | A fuller app: editable vocabulary, saved recordings, speaker labels, a browser viewer. |
| [`tools/`](tools) | Training, model export, packing, and device test tooling. |
| [`docs/`](docs) | Measurements and investigation notes. |

To use the engine in your own program, see [`firmware/README.md`](firmware/README.md). How speed
and accuracy changed across versions is in the
[version report](https://cadengithubb.github.io/ESP32_Onboard_Speech_To_Text/).

The model is NVIDIA's QuartzNet 15x5 fine-tuned on AMI, LibriSpeech, VoxPopuli and People's Speech;
sources and licences are listed on the
[model release](https://github.com/CadenGithubB/ESP32_Onboard_Speech_To_Text/releases/tag/model-gen2-25397).
