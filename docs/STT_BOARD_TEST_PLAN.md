# Board test plan: level-robust onset, opt-in noise gate, 2^-30 front end + blend-g30

> **Where the pieces are (15 Nov 2026).** Version 3.1 on `main` has the onset and the opt-in noise controls
> and builds with gen2-25397. The log guard `2^-30`, the interlock, the frontend-tool fixes (`contract.py`,
> `frontend-contract.json`, `quartznet_frontend_contract.h`), `docs/STT_LOG_GUARD.md` and
> `docs/STT_FRONTEND_GUARD.md` are on branch `voice-model-3-frontend`. Steps here that use them, `contract.py` or
> blend-g30 apply to that branch; on `main`, `contract.py` still points at HardwareOne's path.

Written 13 November 2026 for the `mic-level-fix` worktree (base 1a51eac). Onset and gate are committed in version 3.1;
the `2^-30` front end and blend-g30 steps need branch `voice-model-3-frontend`. No board
step has run yet. What has run: ESP-IDF 5.5.5 builds (13 Nov, on a copy of the tree, nothing flashed) of the
worktree with the blend-g30 identity in its default, gate and `sdkconfig.prev_onset` configurations and of
1a51eac, plus the two interlock refusals (v2 and v3 identity); see section 1. Those builds used plain
`sdkconfig.defaults` without the metrics overlay or speaker labels, so they are not the images below. It is
one board session (about 1.5 days on the bench plus one day of host scoring) that measures what the host
tests cannot prove:

- `decision_pre_emphasis` 97 / gain 150 (Kconfig `CAPTIONS_DECISION_PRE_EMPHASIS`, default y);
  `docs/STT_LEVEL_ROBUST_ONSET.md` "Not proven".
- The opt-in noisy-room mode (Kconfig `CAPTIONS_DECISION_MODULATION_GATE`, default n: modulation ratio 300
  plus the raw-latch hold); the same document's "Noise controls".
- The recognizer log guard 2^-30 with `release/voice-model-v3-blend-g30`; `docs/STT_LOG_GUARD.md` and
  `docs/STT_FRONTEND_GUARD.md` "Not proven".

What each part answers:

| Open question | Measured in |
|---|---|
| CPU per frame on the capture task, `counters.dropped`, capture-task stack, decode queue and close latency under 2.2-2.7x more recognizer audio, hard cuts, RAM | S2 (device, every build) |
| Quiet-room false captions (words per hour), with and without the gate | S3 raw hour (host, every arm) and S4 night chain (device, B and C) |
| Live talker at 1 ft and 3 ft | S1 raw takes (host, every arm on the same audio) and S2 device takes |
| The onset on real quiet-room continuous audio (night-1 style replay) | S3 raw capture of ab1 + six night-1 blocks, scored with `tools/sttraw_ab/score.py` |
| Steady noise (fan) and bystander babble | S1 raw takes and S2 device takes |
| blend-g30 loads, runs at speed and is not worse on the board | S2 (device) and A3 (host decode of the S3 raw audio with both models) |
| The onset alone on the current model (gen2-25397, `2^-24`), which HardwareOne ships first (`docs/STT_ENGINE_INTEGRATION.md` section 6, step 2) | S2 build G vs A (device), A1/A2 `production` vs `off` (host, same decoder) |
| HardwareOne's real gain chain (PDM x8, 120 Hz high-pass, within-phrase leveler) and the glasses with `sttg2decisionpre` 0 and 97 | Section 6: H1 (host, recordings already on the drive), H2 (HardwareOne raw takes), H3 (HardwareOne device after the port) |

Not covered: SD/FAT power-loss behaviour, the overlap gate's `to_gate_log_mel` timing (optional build O,
section 1) and the 2000h-g30 package. HardwareOne's own STT path is covered only by section 6.

## Preconditions and approvals

Ask the user for these before starting; none of them is implied by this plan.

1. Flashing test images to the bench P4 and restoring production afterwards.
2. Running `voice-model-v3-blend-g30` on the board. This is a model switch for testing
   (`docs/STT_FRONTEND_GUARD.md` "How to integrate"); shipping it is a separate decision (section 4).
3. Temporarily changing HardwareOne's saved mic settings on the board for the raw recorder (S1, S3); they
   are restored in S5.
4. Pausing other automation that opens serial ports on the bench Mac (Codex sessions, bench chains) for
   the whole session.
5. The thresholds in sections 2 and 6. Fix them before the session; do not move them after seeing results.
6. For section 6: flashing a HardwareOne image with the leveler and `sttraw` (H2, only if R lacks the leveler)
   and, later, the ported HardwareOne branch (H3). Each is a separate approval.

Host preflight, on the analysis Mac, from the worktree (single-threaded, `nice -n 10`): the five segmenter
tests listed in `docs/STT_LEVEL_ROBUST_ONSET.md`, `python3 -I -B tools/stt_p4/frontend/contract.py --check`
(prints `b21a8eec…`), and `shasum -a 256 -c SHA256SUMS` in the blend-g30 package folder (11 OK).

## Names used below

| Name | Meaning |
|---|---|
| bench Mac | The Mac the P4 is plugged into (today the Mac mini; its speaker at volume 70 is the reference for every earlier 1 ft recording). Keep it as the bench so playlists stay comparable. |
| analysis Mac | Where builds and scoring run. If it also runs a training job: no MPS/GPU, CPU jobs under `nice -n 10` with at most 3 threads, one scoring job at a time. |
| `$W` | The `mic-level-fix` worktree (analysis Mac). |
| `$M` | A checkout whose `tools/sttraw_ab/score.py` has the 10 Nov exponent-sign fix (`-rec.exponent`) and whose private files the recognizer needs exist: the export venv (`tools/stt_p4/private/export-venv`) and the `2^-24` frontend binary that `tools/overlap_gate/g0_extract.py` asserts (`private/training-track/compact16k-loss-round3/runtime-build/frontend`). The worktree's own `score.py` has the fix since 13 Nov, but the worktree has no such frontend binary, so put `$M/tools` first on `sys.path` (import `overlap_gate` from `$M`) when running `$W`'s scorer, as the 13 Nov e8f93103 run did. |
| `$BENCH` | A plain copy of the worktree's `tools/` on the bench Mac (not a git checkout): `rsync -a --exclude private $W/tools $BENCH/`. `night_driver.py` resolves its private files under `$BENCH/private/`. |
| `$P` | This session's private folder: `$BENCH/private/board-test/<YYYYMMDD>` on the bench Mac, copied to `$W/private/board-test/<YYYYMMDD>` for scoring. `private/` is git-ignored everywhere in this repository. |
| `$IDFPY` | The ESP-IDF 5.5 Python on the bench Mac (has esptool and pyserial; `night_driver.py` hard-codes `~/.espressif/python_env/idf5.5_py3.9_env/bin/python`). |
| `$PY` | The export venv Python (numpy, torch) used by every scorer: `$M/tools/stt_p4/private/export-venv/bin/python`. |
| `$PORT` | The P4's port, from `HW1_STT_P4=<dir with private/unit.env> $IDFPY $BENCH/tools/device_tests/find_p4_port.py` (USB descriptor only; never opens the port). Never guess it. |
| `$APP_OFF`, `$APP_SIZE` | The running app partition's offset and size, read from the board in S0. Expected `0x10000` / `0x715000`, but only the board's own table counts. |
| `G30` | `/Volumes/Audiodrive/stt/work/release/voice-model-v3-blend-g30` |
| R | The raw recorder: a HardwareOne image with the `sttraw` command (continuous raw capture to `/sd/STT Raw/raw_<id>.pcm` + `.json`, "exactly what the segmenter is fed"). The standalone example has no continuous raw tap (`pcm save` keeps only finished segments). |

The board MAC stays in the ignored `private/bench-board-mac.txt` (or `HW1_BENCH_MAC`); the HardwareOne
console login stays in its mode-600 file. Neither goes into a command line that is logged, a note or a doc.

## 0. Safety first (S0, about 60 min, attended)

### 0.1 Serial port ownership (before every step that touches the board)

Another tool on the bench Mac can reset the P4: on 2 Nov an `esptool chip_id` from a Codex session reset it
into download mode mid-soak. macOS lets two processes open the same tty, and pyserial's `exclusive=True`
is only an advisory lock that esptool ignores.

```sh
lsof -n /dev/cu.usbmodem* 2>/dev/null                 # must be empty before you open the port
pgrep -fl 'esptool|idf_monitor|idf\.py|miniterm|screen /dev|night_driver|heaptw|run_session|serial_capture'
ls ~/claude-heaptw/busy.lock 2>/dev/null               # another bench chain owns the board: stop
ls -t ~/.codex/sessions/ 2>/dev/null | head -3         # recent Codex sessions; grep the day's *.jsonl for usbmodem
```

An unexplained reset that shows `rst:0x17 (CHIP_USB_UART_RESET)` or `boot:0x304 (DOWNLOAD…)` / "waiting for
download" with no panic before it was caused by the host toggling DTR/RTS. Grep that day's Codex logs and
the commands above for that minute before blaming firmware; record it as a host incident and repeat the run.
`night_driver.py` creates and removes `~/claude-heaptw/busy.lock` itself (the folder must exist) and refuses to
start while the file exists. For manual steps create the same lock by hand
(`echo "board-test $(date)" > ~/claude-heaptw/busy.lock`), and remove it before every `night_driver.py` launch.

### 0.2 Identify the board and read the partition table

```sh
mkdir -p -m 700 $P/flash
$IDFPY -m esptool --chip esp32p4 -p $PORT -b 921600 --before default_reset --after no_reset chip_id > $P/flash/chipid.log 2>&1
grep -qi "MAC: *$(head -1 $BENCH/private/bench-board-mac.txt)" $P/flash/chipid.log && echo "test P4" || echo "WRONG BOARD: stop"
$IDFPY -m esptool --chip esp32p4 -p $PORT -b 921600 --before no_reset --after no_reset flash_id >> $P/flash/chipid.log 2>&1   # 16 MB expected
$IDFPY -m esptool --chip esp32p4 -p $PORT -b 921600 --before no_reset --after no_reset read_flash 0x8000 0xc00 $P/flash/pt.bin
$IDFPY $IDF_PATH/components/partition_table/gen_esp32part.py $P/flash/pt.bin | tee $P/flash/pt.csv
```

Check in `pt.csv`:
- Which app partitions exist. If there is an `otadata` partition, the app that runs is the one the boot log
  names (`boot: Loaded app from partition at offset 0x…`, seen on the next console open); flash that slot
  only. With only `factory` it is the factory slot.
- Set `APP_OFF` / `APP_SIZE` from that row. Every image below must be at most `APP_SIZE` bytes
  (`stat -f %z img.bin`). The example's own `partitions.csv` (factory `0x10000` size `0x715000`) is not
  evidence; the 31 Oct note says the board's table matched it, but read it again.
- No secure boot or flash encryption (`$IDFPY -m espefuse --chip esp32p4 -p $PORT summary` if in doubt). If
  either is on, stop.

### 0.3 Private rollback images

```sh
$IDFPY -m esptool ... --before no_reset --after no_reset read_flash $APP_OFF $APP_SIZE $P/flash/app-slot-backup.bin    # ~2-4 min
$IDFPY -m esptool ... --before no_reset --after no_reset read_flash 0 0x1000000 $P/flash/full-16MB.bin                  # ~5-10 min
$IDFPY -m esptool ... --before no_reset --after no_reset verify_flash $APP_OFF $P/flash/app-slot-backup.bin            # must say verified
$IDFPY -m esptool image_info --version 2 $P/flash/app-slot-backup.bin > $P/flash/app-slot-backup.info
shasum -a 256 $P/flash/*.bin > $P/flash/SHA256SUMS; chmod 600 $P/flash/*
```

(`...` = `--chip esp32p4 -p $PORT -b 921600`.) The full image also covers NVS: the standalone app writes
its boot counter to namespace `hw1_stt_demo` in the board's NVS (it never erases NVS), and `littlefs`.

Then hard-reset into production (`--before no_reset --after hard_reset chip_id`), open the console without
reset (HardwareOne `tools/p4/serial_capture.py open_port`, or `night_driver.py`'s `Console`), log in from the
mode-600 file (five bad logins lock the console for 30 s) and record in `$P/session.md`: the app version
line, `help sttraw` (is the recorder in this image?), and `sttdetector` (the saved `sttautogain`,
`pdmamplify`, `pdmhpf`, floors, `sttdecisionpre`, to restore in S5).

`night_driver.py` reuses these files. On the bench Mac:

```sh
B=$BENCH/private/training-track/overlap-gate/phase4/board; mkdir -p -m 700 $B
cp $P/flash/pt.bin $B/partition-table.bin
cp $P/flash/app-slot-backup.bin $B/; shasum -a 256 $B/app-slot-backup.bin | cut -c1-64 > $B/app-slot-backup.sha256
```

`night_driver.py` writes test apps and the restore at `0x10000` and opens `/dev/cu.usbmodem101`, both hard-coded.
If `$APP_OFF` is not `0x10000`, do not use it; use the manual flash below. If `$PORT` differs, make a copy with
only `PORT` changed next to the original, so its `ROOT` (two folders up) is still `$BENCH`:
`sed "s#/dev/cu.usbmodem101#$PORT#" $BENCH/tools/overlap_gate/night_driver.py > $BENCH/tools/overlap_gate/night_driver_port.py`
(the copy is outside git), and use that name below.
Its restore expects the boot line `App version: 0.99.99`; if production is another version the restore is
written and verified but reports `expected_boot_seen=false`; check the boot by hand.

### 0.4 Flashing one app (the only flash write the operator makes)

```sh
f=$P/img/B.bin; [ $(stat -f %z $f) -le $((APP_SIZE)) ] || echo "does not fit: stop"
$IDFPY -m esptool ... --before default_reset --after no_reset chip_id | grep -qi "MAC: *<mac>"     # same board
$IDFPY -m esptool ... --before no_reset --after no_reset read_flash 0x8000 0xc00 $P/flash/pt-now.bin
cmp $P/flash/pt-now.bin $P/flash/pt.bin || echo "partition table changed: stop"
$IDFPY -m esptool ... --before no_reset --after hard_reset write_flash $APP_OFF $f | tee -a $P/flash/writes.log   # needs "Hash of data verified"
```

Verify: the console shows `P4X-EYE captions: /sd/STT Models/<the build's model>`, `Model ready in … ms` and
`Listening (noise floor …)`. Never use `idf.py flash` (it also writes the bootloader and partition table).
`night_driver.py --flash-app IMG --flash-sha256 SHA` makes the same MAC, partition-table and image-hash checks
before its write (not the size check: do that once per image in 1.1).
Rollback at any time: the same command with `$P/flash/app-slot-backup.bin`, then the version line. If the
board stops answering (no console, esptool cannot sync), unplug and replug it before anything else.

### 0.5 SD card

Power the board down (unplug USB) before taking the card out. On the Mac, under `STT Models/`, keep every
existing file (HardwareOne needs `quartznet15x5-gen2-25397.p4.stt`, `meeting.lm`, `speaker.spk`) and add
`voice-model-v3-blend-g30.p4.stt` from `$G30` (20,112,224 bytes). Check it on the card:
`(cd "$G30" && grep 'blend-g30.p4.stt' SHA256SUMS) | (cd "/Volumes/<card>/STT Models" && shasum -a 256 -c)`.
Leave at least 2 GB free (raw recordings, `STT PCM/`). Reinsert, plug in, confirm production boots and loads.

## 1. Build matrix

Each image is built from a private source snapshot, so the worktree stays untouched and the exact source is
recorded. All five use the same profile: `sdkconfig.defaults` + metrics, `-DHW1_STT_SPEAKER_LABELS=ON`
(speaker task running, the heavier case; same as night 1), and the test-only stack report (1.3).

| Build | Source | Model on the card (`kModelPath`) | Onset | Gate | Purpose |
|---|---|---|---|---|---|
| A | 1a51eac (`git archive`) | `quartznet15x5-gen2-25397.p4.stt` (2^-24) | old detector | n/a | Current firmware: the baseline. |
| G | 1a51eac + the onset files only (worktree `stt_segmenter.h`, `main.cpp`, `Kconfig.projbuild`) | gen2-25397 (2^-24) | 97 / gain 150 | off | The onset on the current model and front end: what HardwareOne ships first (integration order step 2). |
| F | worktree + `sdkconfig.prev_onset` | `voice-model-v3-blend-g30.p4.stt` | old detector (bit-for-bit) | off | Front end + model change alone; the control for B. |
| B | worktree | blend-g30 | 97 / gain 150 (Kconfig default y) | off (default n) | Candidate default. |
| C | worktree + gate overlay | blend-g30 | 97 / 150 | ratio 300 + hold (Kconfig y) | Noisy-room mode. |
| O (optional) | worktree + `sdkconfig.overlap`, `-DHW1_OVERLAP_GATE=ON` | blend-g30 | 97 / 150 | off | Only to time the gate's `to_gate_log_mel` (one `expf` + `logf` per cell); needs `overlap_gate.espdl` on the card. |

A vs G = the onset alone on gen2-25397 (the configuration HardwareOne would ship first); A vs F = model +
guard; F vs B = the onset alone on blend-g30; B vs C = the gate alone. Gate ratio 135 (the largest ratio
within the 1 s speech-loss limit on the host, chosen in-sample) is not a build: it is scored on the raw
recordings (A2), and gets its own device build only if section 4 says so.

**Why build G exists.** `docs/STT_ENGINE_INTEGRATION.md` ships the onset first on the current model (step 2)
and the guard and model later (step 3), so each can be rolled back alone. The worktree tree cannot run that
configuration (interlock, below). The alternative, shipping onset, front end and blend-g30 together, needs
no extra build but would leave a regression unattributable and make the onset wait for the model go-ahead.
This plan keeps the staged order and adds G. G's source is 1a51eac with three worktree files copied in:
`stt_segmenter.h` (the onset and noise controls; it includes only the standard library and
`Audio_VadPolicy.h`, which is unchanged), `main.cpp` (its only change against 1a51eac is the two
`#if CONFIG_CAPTIONS_DECISION_*` blocks; `kModelPath` stays gen2-25397) and `Kconfig.projbuild`. It keeps
1a51eac's front end, `overlap_adapter.cpp`, CMake list and v2 identity, so it has no interlock. G's source set is the 3.1 tree on main,
built on 15 Nov with plain `sdkconfig.defaults` (onset on and `sdkconfig.prev_onset`); the board image below has not been built; it compiles the same segmenter header as the 13 Nov worktree builds and the same remaining
sources as the 13 Nov 1a51eac build.

**Interlock.** `stt_frontend_interlock.cpp` `static_assert`s that `stt_model_identity.h` pins the frontend
contract in the generated `quartznet_frontend_contract.h` (`b21a8eec…`, guard 2^-30). So:
- F, B, C (and O) build only after the blend-g30 `stt_model_identity.h` is installed in the snapshot and
  `kModelPath` points at the blend-g30 file. With the v2 or v3 header the build fails at compile time
  (intended; confirmed on 13 Nov).
- A and G (1a51eac sources, no interlock) keep the v2 header, so their runtime identity check accepts only
  gen2-25397.
- The onset's device effect is measured twice: A vs G on gen2-25397 and F vs B on blend-g30. Its word-level
  effect with gen2-25397 is also measured on the host (A1/A2), where every arm sees the same audio.
- Each image opens only its own model file; the wrong file halts at boot with "Check that the card holds the
  exact model this build is pinned to". Both files stay on the card.
- If blend-g30 is rejected (section 4), the onset can still ship from G's source set (1a51eac + the onset
  files), which this session has already run on the device.

### 1.1 Snapshots and builds (P1, about 90 min, the day before; analysis Mac or bench Mac)

ESP-IDF 5.5.5 exported (`idf.py --version`). On the analysis Mac while training runs, configure with
`idf.py … reconfigure` and build with `nice -n 10 ninja -C <build dir> -j 3`.

Record which ESP-IDF builds the images: the 13 Nov builds used a local v5.5.5 with edits in
`components/freertos/FreeRTOS-Kernel/portable/riscv/portasm.S` and
`components/riscv/include/riscv/rvruntime-frames.h` (the PIE context-save fix; `idf.py --version` says
`v5.5.5-dirty`). A pristine v5.5.5 lacks that fix and may differ slightly in size. Save `git -C $IDF_PATH
describe --dirty` and `git -C $IDF_PATH diff` into `$S`. The example has no `dependencies.lock`: the component
manager solves dependencies on first configure (13 Nov: `dl_fft` 0.7.0 and `esp_new_jpeg` 1.0.2 from the local
cache; a clean Mac downloads them). Keep each build folder's `dependencies.lock` with the images.
`prepare_esp_dl.py` finds the pristine ESP-DL 3.3.12 in the component-manager cache or downloads it; `--source
<folder>` names it explicitly.

```sh
S=$P/src; mkdir -p $S/cfg
git -C $W archive --prefix=A/ 1a51eac firmware | tar -x -C $S
git -C $W archive --prefix=G/ 1a51eac firmware | tar -x -C $S          # G = 1a51eac + the onset files only
for f in components/hw1_stt/stt/stt_segmenter.h examples/p4_eye_captions/main/main.cpp examples/p4_eye_captions/main/Kconfig.projbuild; do
  cp $W/firmware/$f $S/G/firmware/$f; done
grep -n 'quartznet15x5-gen2-25397' $S/G/firmware/examples/p4_eye_captions/main/main.cpp   # kModelPath unchanged
diff -r $S/A/firmware $S/G/firmware | grep -c '^diff\|^Only'                            # 3 files, nothing else
mkdir -p $S/W/tools/stt_p4; rsync -a --exclude build --exclude .sdk-overrides $W/firmware $S/W/
rsync -a $W/tools/stt_p4/frontend $S/W/tools/stt_p4/
git -C $W rev-parse HEAD > $S/W.base; git -C $W diff > $S/W.diff; git -C $W status --short > $S/W.status
(cd $S/W && find firmware tools -type f -print0 | sort -z | xargs -0 shasum -a 256) > $S/W.files.sha256
# blend-g30 into the worktree snapshot only
cp "$G30/stt_model_identity.h" $S/W/firmware/components/hw1_stt/stt/
sed -i '' 's#quartznet15x5-gen2-25397\.p4\.stt#voice-model-v3-blend-g30.p4.stt#' $S/W/firmware/examples/p4_eye_captions/main/main.cpp
(cd $S/W && python3 -I -B tools/stt_p4/frontend/contract.py --check --identity)    # must print b21a8eec…, exit 0
printf 'CONFIG_CAPTIONS_STREAM_METRICS=y\n' > $S/cfg/metrics
printf 'CONFIG_CAPTIONS_DECISION_MODULATION_GATE=y\n' > $S/cfg/gate
# apply 1.3 to the main.cpp of $S/A, $S/G and $S/W, then stage ESP-DL in each snapshot:
for s in A G W; do (cd $S/$s/firmware && python3 tools/prepare_esp_dl.py); done
build() { # name snapshot overlays...
  local n=$1 proj=$S/$2/firmware/examples/p4_eye_captions d=$S/build-$1; shift 2
  local defs="$proj/sdkconfig.defaults"; for o in "$@"; do defs="$defs;$o"; done
  idf.py -C $proj -B $d -DSDKCONFIG=$d/sdkconfig -DSDKCONFIG_DEFAULTS="$defs" -DIDF_TARGET=esp32p4 -DHW1_STT_SPEAKER_LABELS=ON build \
    && mkdir -p $P/img && cp $d/p4_eye_captions.bin $P/img/$n.bin; }
build A A $S/cfg/metrics
build G G $S/cfg/metrics
build F W $S/W/firmware/examples/p4_eye_captions/sdkconfig.prev_onset $S/cfg/metrics
build B W $S/cfg/metrics
build C W $S/cfg/metrics $S/cfg/gate
(cd $P/img && shasum -a 256 *.bin > SHA256SUMS)
grep -H 'CAPTIONS_DECISION\|CAPTIONS_STREAM_METRICS\|CAPTIONS_DRAFTS=\|SPIRAM_SPEED' $S/build-*/sdkconfig
```

Expected in the `sdkconfig`s: A has no `CAPTIONS_DECISION_*`; G and B `PRE_EMPHASIS=y`, gate not set; F
`PRE_EMPHASIS` not set; C both `y`; all five `STREAM_METRICS=y`. Record image sizes against `APP_SIZE`. The
metrics build changes timing (`Kconfig.projbuild`), equally for all five; comparisons are between builds.

Reference from the 13 Nov builds (plain `sdkconfig.defaults`, speaker labels off, so the plan's images will
be somewhat larger; evidence `nohw/idfbuild/`):

| 13 Nov build | App image | Flash .text / .rodata | Internal DIRAM (of 655,360) |
|---|---|---|---|
| 1a51eac | 650,288 B | 448,650 / 112,696 | 135,526 B |
| worktree + blend-g30, onset on (B's settings) | 658,576 B | 453,694 / 112,680 | 138,790 B |
| the same with the gate (C) | 658,592 B | 453,706 / 112,680 | 138,790 B |
| the same with `sdkconfig.prev_onset` (F) | 658,560 B | 453,678 / 112,680 | 138,790 B |

Every image leaves about 91% of a `0x715000` app slot free. The worktree adds 3,264 B of internal .data,
3,240 B of it `main.cpp`'s statically initialised `gPipeline` (2,440 -> 5,680 B, the larger `Segmenter`);
onset on, off and gate are byte-identical in RAM and differ only in a few bytes of `main` .text. Each build
had one warning, in third-party ESP-DL (`dl_image_jpeg.cpp:349`). G is expected to land near the worktree's
RAM figures (same segmenter header) and near A's flash.

### 1.2 Raw recorder R

Use the production image if `help sttraw` answered in S0 (no flash). Otherwise flash the HardwareOne image
that recorded the 8 Nov `stt10-*` sets (sidecar `"firmware":"0.99.99"`; HardwareOne branches
`claude/p4-sttraw-capture` / `claude/stt-1.0`) with 0.4, and record its SHA-256. Set the mic path to match the
standalone example and the earlier recordings, and check every sidecar:

```text
sttautogain 0      (also turns the phrase leveler off on the onboard mic; no "TL phrasegain" lines may appear)
pdmamplify 1
pdmhpf 36
sttdetector        (confirm)
```

Sidecar requirements: `pdm_amplify` 1, `pdm_hpf_hz` 36, `samples_dropped` 0, `gaps_lost` 0, `write_error`
false, `drain_complete` true, `samples_written` = file size / 2. The standalone example opens the PDM mic
with ESP-IDF's default PDM RX slot (high-pass on near 35.5 Hz, amplify x1, DSR16); confirm that in the IDF
header once, and compare the noise floor of an R quiet minute with build F's `Listening (noise floor N…)` at
the same spot (expect agreement within 2 counts).

One take = one continuous session: `sttraw on` (arms the next session only), `stt start` (note the id),
the take, `stt stop <id>`, `sttraw status` (file name, samples, dropped). Sessions longer than the 10.3 min
`ab1` have not been recorded before: check `samples_written` against the wall time after the first long one.

### 1.3 Test-only capture-task stack report

The `state` page's `owner_stack_min_bytes` is the main task, not the 8192-byte capture task where the
segmenter runs. In the three snapshots' `main.cpp` (A, G, W; never in the worktree), and identically in all
builds:

```cpp
// in the anonymous namespace, above testCommand():
TaskHandle_t gCaptureTask = nullptr;  // board test: capture-task stack high-water mark
// app_main(): pass the handle
if (xTaskCreatePinnedToCore(captureTask, "capture", 8192, nullptr, 3, &gCaptureTask, 1) != pdPASS)
// app_main(): before its caption loop (the for (;;) after printf("Measuring room noise...\n"))
UBaseType_t stackReported = ~UBaseType_t(0);
uint32_t stackCheckMs = 0;
// first lines inside that loop
if (nowMs() - stackCheckMs >= 1000) {
  stackCheckMs = nowMs();
  const UBaseType_t freeBytes = uxTaskGetStackHighWaterMark(gCaptureTask);   // bytes on ESP-IDF
  if (freeBytes < stackReported) { stackReported = freeBytes; printf("STACK capture_min_free=%u\n", unsigned(freeBytes)); }
}
```

## 2. Session steps and measurements

### Prep (P2-P6, the day before, about 3 h)

- **P2 Manifests and WAVs** (bench Mac, under `$P/manifests` and `$P/wav`; copy WAVs locally, check SHA-256):
  - `rt/block91.json` = `ab1.json` + `"block": 91`, `rt/block92.json` = `nb1.json` + `"block": 92`
    (`jq '. + {block: 91}'`), with `wav` rewritten to the local copies of
    `/Volumes/Audiodrive/stt/sttraw/playlists/{ab1,nb1}.wav`.
  - `n1six.wav` / `n1six.json`: night-1 blocks 1, 2, 3, 4, 6 and 10 joined (Appendix C): 4,303.6 s, 544 clips,
    8,557 reference words (ami-val 253, ami-test 172, ls-test 47, syn-two 24, real32 18, syn-ord 15,
    syn-quiet 11, noise 4). These are overlap-gate training blocks; the gate's held-out blocks 5/7/8/9/12/14 stay
    unused. Source manifests: `$M/private/training-track/overlap-gate/phase4/night1/`.
  - Silent WAVs for `night_driver.py` timing runs (played at volume 0): `silence200.wav` (200 s, talker takes),
    `silence600.wav` (fan only), `silence3600.wav` (night chain), with `quiet/block95.json`; and
    `babble600.wav`, 600 s of `n1six.wav` from 1,800 s, the bystander bed for R-bab and the device babble runs
    (Appendix C). Make `n1six` and `babble600` on the analysis Mac (the block manifests live there) and copy them.
  - `script.txt`: the reading script (Appendix A).
- **P3 Builds** (1.1) and copy `$P/img` to the bench Mac.
- **P4 Helpers**: save Appendix B (`rawtake.py`, `devsum.py`) and C to `$W/private/board-test/bin/`. Both
  scripts were run on existing recordings and night-1 logs on 13 Nov.
- **P5 blend-g30 host decoder** (1-2 h): `score.py` and `rawtake.py` decode with the deployed gen2-25397 export
  and the 2^-24 C++ front end (`overlap_gate/g0_extract.py` `Recognizer`, `FRONTEND`). For A3, write a
  `Recognizer` variant that loads `int8/t2000-b70-mse-g30` with a 2^-30 front-end binary built from the worktree,
  asserts the export manifest's `frontend_contract_sha256` equals the worktree contract, and uses its own
  logit cache (the caches are keyed only by PCM hash). The 12 Nov scorer `guard/score/night1_g30.py` does
  exactly these checks; it is kept in the evidence folder (`$W/private/evidence-2026-11-13/guard/score/`,
  git-ignored, and `stt/work/engine-evidence-2026-11-13/guard/score/` on the Audiodrive). Copy it into
  `$W/private/board-test/` and set the scratch, worktree and main-checkout paths written inside it.
- **P6 Physical setup**, marked with tape: P4 on a stand, mic port facing the Mac mini speaker at 30 cm
  (1 ft); talker chair positions at 30 cm and 90 cm (3 ft) on the mic axis; a fan (or air purifier) on a fixed
  setting 1.5 m away, off the mic axis; babble position: P4 moved to 2.5 m from the Mac mini speaker, talker
  at 90 cm from the P4. Keep the case exactly as in the earlier recordings (it covers the mic) and photograph
  the setup into `$P`.

### S1 Raw takes on R (day, about 75 min, attended)

Board on R (1.2). Talker seated, conversational level, the same person throughout. Each line is one session
(1.2); note start/stop times and anything misread on the script sheet. Correct `script.txt` copies per take
to what was actually said before scoring (`$P/ref/<take>.txt`); device and raw scores of a take use the same
reference.

| Take | Condition | Length |
|---|---|---|
| R-t1a, R-t1b | Talker 1 ft, script, two readings | 2 × ~2.5 min |
| R-t3a, R-t3b | Talker 3 ft, script, two readings | 2 × ~2.5 min |
| R-fan | Fan only, nobody speaking | 10 min |
| R-fan-t3 | Fan on, talker 3 ft, script | ~2.5 min |
| R-bab | Babble only: `babble600.wav` at volume 70, P4 at 2.5 m from the Mac mini, nobody near | 10 min |
| R-bab-t3 | Same babble, talker 90 cm from the P4, script | ~2.5 min |

### S2 Device runs: realtime, resources and live takes (day, about 4.25 h, attended)

Order F, B, C, A, G (F and B adjacent, A and G adjacent; repeat F and B at the end if time allows). Per build:

```sh
D="$IDFPY $BENCH/tools/overlap_gate/night_driver.py --arms off --deadline 23:59"
export HW1_BENCH_MAC=$(head -1 $BENCH/private/bench-board-mac.txt)
img=B; sha=$(shasum -a 256 $P/img/$img.bin | cut -c1-64)
$D --flash-app $P/img/$img.bin --flash-sha256 $sha --manifests $P/manifests/rt --volume 70 --post 20 \
   --no-restore --out $P/runs/rt-$img                                        # ab1 then nb1, fresh boot each, ~25 min
for t in 1ft 3ft; do   # talker starts reading at the driver's "listening" event; 200 s window, nothing audible plays
  $D --dry-wav $P/wav/silence200.wav --volume 0 --post 15 --out $P/runs/talk-$img-$t; done
# B and C only, same positions as R-fan / R-bab:
$D --dry-wav $P/wav/silence600.wav --volume 0 --post 15 --out $P/runs/fan-$img    # fan on, nobody speaking
$D --dry-wav $P/wav/babble600.wav --volume 70 --post 15 --out $P/runs/bab-$img    # P4 at 2.5 m from the Mac mini
```

Each driver run resets the board through esptool (which re-checks the MAC), waits for `Listening`, sends
`pcm save on` and `perf dump`, sets the Mac's output volume to `--volume`, plays the WAV with `afplay`, waits
`--post` seconds, sends `perf dump` again, and records every console line with host time in
`runs/<run>/serial.log` and events in `events.jsonl`. `--dry-wav` runs one take on the image already flashed
and skips the final restore (its run folder is `runs/dry-on-v<volume>`; the `overlap on` it sends is answered
with "Overlap gate not loaded", which is harmless). Because the driver sets the system volume, never play a
second WAV by hand during a driver run.

Summarise each run at once (Appendix B `devsum.py`; numbers only, no caption text):

```sh
HW1_SCORE_ROOT=$M $PY -B $W/private/board-test/bin/devsum.py $P/runs/*/runs/*/serial.log > $P/devsum.jsonl
HW1_SCORE_ROOT=$M $PY -B …/devsum.py $P/runs/talk-B-3ft/runs/*/serial.log --ref $P/ref/talk-B-3ft.txt
```

Measurements and pass/fail (proposed; agree before the session). "Every run" means every S2 and S4 run of
that build.

| What | Source (devsum field) | Pass |
|---|---|---|
| Crashes | `failures` (panic, abort, watchdog, stack fault, model load), unexplained `resets` | 0 in every run |
| Microphone loss | `capture.overflow`, `capture.rejected` | 0 in every run |
| Dropped phrases (`counters.dropped`) | `dropped`, `state.drops` | 0 in every run |
| Capture CPU per frame | `capture.push_mean_us` / 6.4 (one push = 1,024 samples = 6.4 frames of 10 ms); night 1 baseline ~500 µs per push (78 µs/frame) | B ≤ F + 1,000 µs per push and ≤ 2,000 µs (3% of the 64 ms read); G ≤ A + 1,000 µs and ≤ 2,000 µs; C ≤ B + 500 µs |
| Capture worst case | `capture.push_max_us`, `capture.interval_max_us`, `push_lock_hold_max_us` (baseline ~10 ms / 75 ms / 10 ms) | push max ≤ 32 ms; interval max ≤ 192 ms (DMA ring is 4 × 64 ms) |
| Capture-task stack | `stack_min` (1.3), lowest over all runs | ≥ 1,024 B free of 8,192 (host estimate: segmenter chain ~770 B at -Os, +100 B with the gate) |
| Decode queue | `state.ready_high_water` (3 slots), `state.feature_overruns` (baseline 6-7 per 12 min) | high water ≤ 2; overruns ≤ F × 1.5 + 5 |
| Recognizer load | `duty` = Σ worker_ms / wall (night 1 baseline 0.54-0.57 on dense playback) | ≤ 0.85 on ab1 and nb1 |
| Audio sent vs F | `decode_audio_s` B / F and G / A on ab1 (host prediction 2.2-2.7x) | sanity: flag outside 1.8-3.5x |
| Caption latency | `close_ms.p90` / `max` of finals (night 1 baseline p90 2.5-2.6 s) | B p90 ≤ F p90 + 1.5 s and ≤ 4.0 s; G p90 ≤ A p90 + 1.5 s and ≤ 4.0 s |
| Hard cuts | `forced` per ab1 / nb1 run (host prediction ab1 7-10, nb1 21) | sanity: flag above 2x the host prediction |
| RAM | `state.internal_min_boot` (night 1 baseline 410 KB), `psram_min_boot` (~2.46 MB), `TL stream … internal/largest` | internal min ≥ A − 8 KiB (expect about −3.2 KiB for G, F, B and C alike: all compile the same `stt_segmenter.h`, RV32 `sizeof(Segmenter)` 1,240 → 4,472 B whether the onset or gate is on or off; the 13 Nov ESP-IDF builds put +3,264 B in internal .data, identical for onset on, off and gate, so B vs C and F vs B differ by 0 B); PSRAM min ≥ 1.5 MiB |
| Model | `model_ready` line | every boot; load time within 10% of A |
| Front end + model speed | `TL stream` `nn` ms per final audio ms, F vs A | ≤ A + 5% |

Live takes (device; each build reads the script once per distance, so readings differ between builds):
WER from `devsum.py --ref`. These are sanity checks of direction (B better than F and G better than A at
3 ft); the decisive
comparisons are the raw takes in A2, where every arm sees the same audio.

### S3 Evening raw capture on R: quiet-room playback and a quiet hour (about 2.5 h, attended start and stop)

Start after the house is quiet (about 21:00); windows closed, nobody in the room; note heating, fridge and
the Mac mini's fan state. Board on R at the 1 ft playback position, Mac mini volume 70.

| Take | What | Length |
|---|---|---|
| R-ab1n | `ab1.wav` (the playlist of ee4c/076a), night room | 10.3 min |
| R-n1six | `n1six.wav` (six night-1 blocks) | 72 min |
| R-quiet | Nothing plays, nobody in the room | 60 min |

For each: `sttraw on`, `stt start`, then on the bench Mac
`osascript -e 'set volume output volume 70 without output muted'; afplay $P/wav/<wav>` (or wait 60 min),
20 s, `stt stop <id>`, `sttraw status`. Use the same volume command before every manual playback in S1.
These replace the -12 dB night proxy: the first real night-quiet continuous recordings, and the first with the
night-1 material where speech outside every segment is still in the recording.

### S4 Night chain: quiet-room false captions on the device (unattended, about 7 h)

Launch before leaving (about 23:45). Six 60-minute silent runs, B and C alternating so drifts in the room
affect both, then the production restore:

```sh
D="$IDFPY $BENCH/tools/overlap_gate/night_driver.py --manifests $P/manifests/quiet --arms off --volume 0 --post 30 --deadline 07:30"
for img in B C B C B C; do
  $D --flash-app $P/img/$img.bin --flash-sha256 $(shasum -a 256 $P/img/$img.bin | cut -c1-64) --blocks 95 --no-restore \
     --out $P/runs/night-$img-$(date +%H%M)
done
$D --blocks 0 --out $P/runs/restore      # no block 0: runs nothing, then restores app-slot-backup.bin (MAC, table, hash checked)
```

Save it as `$P/night.sh` and run it detached (`nohup /bin/sh $P/night.sh > $P/night.log 2>&1 &`, or a
`start_new_session` Popen when launching through Remote Desktop) so a closed remote session does not kill it,
and do not start a second copy (`pgrep -f night_driver`). Started at 23:45 the six runs end near 06:40 (62 min
each plus about 3 min per flash); a run that would end after `--deadline` is skipped, and the restore still runs. Each run counts `pcm save` segments too, so every
false caption's audio is on the card.

| What | Pass (proposed) |
|---|---|
| Device B, words per hour (`devsum` `words_per_hour`, mean of the three hours) | ≤ 10 |
| Device C, same | reported; C ≤ B expected |
| All S2 hard limits (crashes, loss, drops, stack) | as in S2 |

### S5 Morning (about 60 min, attended)

1. Check `$P/runs/restore/events.jsonl` for `restore-written` with `verified: true`; open the console without
   reset and confirm the production version line (or restore by hand with 0.4).
2. Unplug, take the card, copy `STT Raw/` and `STT PCM/` to `$P/card/`, record file sizes and SHA-256s, leave
   the card contents in place, reinsert.
3. Boot production, log in, restore the S0 `sttdetector` values (`sttautogain`, `pdmamplify`, `pdmhpf`), run a
   short production STT check (model loads, one caption). Remove `busy.lock` if you created one.
4. Copy `$P` to the analysis Mac (`$W/private/board-test/<date>/`).

### Host scoring (A1-A4, analysis Mac, about one day of CPU, one job at a time)

```sh
export OMP_NUM_THREADS=3 MKL_NUM_THREADS=3 VECLIB_MAXIMUM_THREADS=3 OPENBLAS_NUM_THREADS=3
R=$W/private/segmenter_replay/segmenter_replay; Q=$W/private/board-test/<date>
ARMS=(--arm off="--set decision_pre_emphasis=0"
      --arm g135="--set decision_modulation_ratio_pct=135"
      --arm g300h="--set decision_modulation_ratio_pct=300 --set decision_pre_hold_requires_raw=1")
SCORE() { nice -n 10 $PY -B -c "import runpy,sys; from pathlib import Path; sys.path.insert(0,'$M/tools'); import overlap_gate.g0_extract as g; C=g.Recognizer; g.Recognizer=lambda cache: C(Path('$Q/caption-cache')); sys.argv=['score.py']+sys.argv[1:]; runpy.run_path('$M/tools/sttraw_ab/score.py', run_name='__main__')" "$@"; }
```

Arm names: `production` = G and B (97 / 150, the replay default), `off` = A and F (old detector), `g300h` = C,
`g135` = the in-sample-neutral candidate.

- **A1 playlists** (`score.py`, alignment against the playlist WAV):
  `SCORE --raw $Q/card/STT\ Raw/raw_<id>.pcm --sidecar …json --manifest <ab1.json | n1six.json> --out $Q/scores/<take> --replay $R "${ARMS[@]}"`
  for R-ab1n and R-n1six (2-4 h for n1six). Use the original Audiodrive manifests (or the joined
  `n1six.json`) so `wav` resolves on the analysis Mac.
- **A2 takes without a playlist** (`rawtake.py`, Appendix B): talker takes with `--ref $Q/ref/<take>.txt`,
  R-quiet, R-fan, R-bab without `--ref` (every word is false), R-fan-t3 and R-bab-t3 with `--ref`:
  `HW1_SCORE_ROOT=$M nice -n 10 $PY -B …/rawtake.py <raw.pcm> $Q/scores/<take>.json --sidecar <json> --replay $R --cache $Q/caption-cache [--ref …] "${ARMS[@]}"`
- **A3 blend-g30 on the same segments**: repeat A1 for R-n1six and R-ab1n, and A2 for the talker takes, with the
  P5 decoder. Compare gen2-25397 and blend-g30 arm by arm. Also rescore the 8 Nov `stt10-close` recordings with
  the P5 decoder (the guard documents list close-range board audio as not rescored).
- **A4 consistency**: B device (S2) vs `production` arm (A1/A2) on the same playlists and conditions: hard
  cuts, decode audio, words per hour should agree in direction and roughly in size (different takes). R-ab1n
  `production` vs the day recordings ee4c/076a (52.50 / 45.21 % WER): how much the quiet room changes things.
  R-n1six `off` vs night 1's own device captions on blocks 1-4, 6, 10 (`tools/overlap_gate/caption_wer.py`):
  the same playlist through the old detector on 6 Nov firmware.

Accuracy and noise pass rules (proposed). Host numbers compare arms on identical audio.

| What | Source | Pass for B (vs `off`) |
|---|---|---|
| Night-1-style quiet-room playback | A1 R-n1six, total WER (gen2-25397 decoder) | B ≤ off − 3.0 points; no category with ≥ 300 reference words worse by > 1.5 points (ls-test and syn-ord are the normal-level checks); noise-clip false words ≤ off + 4 |
| ab1 in the night room | A1 R-ab1n | B ≤ off − 5 points |
| Far talker | A2 R-t3a/b, mean | B ≤ off − 5 points |
| Near talker | A2 R-t1a/b, mean | B ≤ off + 1.5 points |
| Quiet room | A2 R-quiet `words_per_hour`; S4 device | ≤ 10 words per hour (host and device) |
| Steady noise | A2 R-fan; S2 fan-B | ≤ 5 words per 10 min |
| Talker under steady noise (the gate's untested risk: synthetic talker-under-white-noise lost 45 s at g135) | A2 R-fan-t3 | B ≤ off + 2 points; report g135 and g300h |
| Bystander babble | A2 R-bab; S2 bab-B / bab-C | reported (words per 10 min, segments, decode audio); the host babble-only stream gave B 59 segments / 287 s where off opened none. Product judgement, see section 4 |
| Talker in babble | A2 R-bab-t3 | B ≤ off + 2 points |

## 3. Data handling

- Everything recorded or derived stays in git-ignored `private/` folders: `$P` on the bench Mac,
  `$W/private/board-test/<date>/` on the analysis Mac. That covers raw PCM, `STT PCM/` segments, serial logs
  (they contain caption text), `events.jsonl`, flash images and backups, source snapshots and caption caches.
  Check with `git -C $W check-ignore -v private/board-test/x` and `git -C $W status --short` (no `private/`
  paths may appear).
- Tracked documents get numbers only: WER, counts, timings, sizes, hashes of images. No caption text, no audio,
  no board MAC, no host names or home paths, no console login.
- Folders created on the bench Mac are mode 700, logs and flash images 600. The console login file is never
  printed, copied into `$P` or passed on a command line.
- Optionally archive raw recordings next to the earlier ones under `/Volumes/Audiodrive/stt/sttraw/board-test-<date>/`
  (outside git; the drive is USB and can drop, so verify with `shasum` after copying).
- The SD card keeps its copies until the scores are written up; delete nothing from it during the session.

## 4. Decision rules

Apply in order. A build that fails a hard limit (crash, mic loss, dropped phrase, stack < 1 KiB, RAM floor)
cannot ship whatever its accuracy; fix and re-run that part.

1. **blend-g30 + 2^-30 (F vs A, A3).** Keep it if: the model loads on every boot of F, B and C; F's `nn` ms per
   audio ms is within 5% of A; A3 blend-g30 ≤ gen2-25397 − 1 point on R-n1six total and not worse by > 1 point
   on ls-test, syn-ord, the 1 ft talker takes and `stt10-close`; false words in R-quiet / R-fan not higher by
   more than 2 per hour. Otherwise reject it and keep gen2-25397; the onset then ships from G's source set
   (the interlock makes the worktree tree itself unusable with 2^-24 models).
2. **Onset default (G vs A and B vs F / off).** The onset may go into HardwareOne first (integration order
   step 2) only if G passes every S2 hard limit and the realtime rules against A, and the host rules below
   (computed with the gen2-25397 decoder, so they apply to G) pass. Ship `CAPTIONS_DECISION_PRE_EMPHASIS=y`
   with blend-g30 if B also passes every S2 limit and the far-speech rules (R-n1six, R-ab1n, R-t3) and the
   quiet-room, steady-noise and talker-in-noise rules. HardwareOne's own gain chain and the glasses are
   decided in section 6, not here.
   - Far speech passes but quiet room or steady noise fails: do not ship B as default. Go to 3.
   - Realtime fails (duty, latency, queue) while accuracy passes: B is not shippable on this firmware; record
     by how much and look at decode cost (drafts, hard-limit chains) before retesting.
   - Far speech does not improve (fewer than 3 points on R-n1six and 5 on R-t3): keep the old detector
     (`=n`); the host gain did not carry to quiet-room audio.
   - Bystander babble: B is expected to caption it. If the user finds R-bab / bab-B unacceptable for the
     product, that alone is a reason to consider C or the old detector, not a measurement failure.
3. **Noisy-room mode C (gate 300 + hold).** Make it the default only if B failed a noise rule in 2, C passes
   that rule, C keeps at least half of B's gain over `off` on R-n1six and R-t3, and C is within 2 points of B
   on R-t1 and R-fan-t3. Otherwise C stays opt-in (`default n`), documented as a noisy-room mode, which is the
   current recommendation.
4. **Gate 135 (host only here).** If A1/A2 show g135 within ±1 point of B everywhere, including R-fan-t3, and
   at least 20% less noise inside segments on R-fan / R-bab, propose a device build with ratio 135 (no hold) for
   a later session. Do not ship a ratio that has not run on the device.
5. Write the actual numbers and every unchecked item into `docs/STT_PRODUCT_HANDOFF.md` (a new dated section
   on top), and update "Not proven" in `docs/STT_LEVEL_ROBUST_ONSET.md`, `docs/STT_LOG_GUARD.md` and
   `docs/STT_FRONTEND_GUARD.md`. Shipping blend-g30 or changing a Kconfig default still needs the user's
   go-ahead after the results.

## 5. Time plan

| When | Step | Time |
|---|---|---|
| Day 0 | Approvals, thresholds, P1-P6 (five builds about 2 h, P5 1-2 h), H1 if not done | ~4.5 h |
| Day 1 08:30 | S0 safety, rollback images, SD card | 60 min |
| 09:30 | S1 raw takes on R (flash R first if needed: +10 min) | 75 min |
| 10:45 | S2 F, B, C, A, G (flash, ab1 + nb1, talker 1 ft / 3 ft; fan and babble for B and C) | ~4.25 h |
| 15:00 | `devsum.py` over S2; stop here if G, B or C fails a hard limit | 30 min |
| 21:00 | S3 R: ab1, n1six, quiet hour | ~2.5 h |
| 23:45 | S4 night chain, B/C alternating, then restore | ~7 h unattended |
| Day 2 07:30 | S5 restore check, card copy, HardwareOne settings | 60 min |
| Day 2 | A1-A4 host scoring (n1six with two decoders is the long part) | ~1 day CPU |

Abort the session (restore production, keep everything recorded) if: the board resets with a panic twice in
one build, the partition table or MAC check fails, a flash is not verified, or the Audiodrive drops while its
files are being read (re-copy the WAVs locally and continue only from local copies).

## 6. HardwareOne gain chain and glasses (H1-H3)

The builds in section 1 run the standalone example, whose PDM path is fixed at x1 with ESP-IDF's default
high-pass (about 35.5 Hz), with no leveler and no glasses input. HardwareOne differs:
- `sttautogain` (default on) opens the onboard PDM mic at a session gain chosen from its history; the 8 Nov
  captures with it on were at x8 with the 120 Hz high-pass.
- The within-phrase leveler (`sttPhraseLevel`, on whenever that mic's auto gain is on; no console command)
  multiplies the PCM by x1..x8, stepping at most x2 or x0.5 every 0.1 s from a meter of the levelled audio and
  the speech floor. Steps are logged as `TL phrasegain now=…`.
- The glasses mic (source 2) is LC3 after the glasses' AGC, with `g2SessionGain`, and has its own onset
  setting `sttg2decisionpre` (default 97, today HardwareOne's judge).

In HardwareOne 4bc86e67 `System_STT.cpp` the raw recorder's tap (`sttRawCapturePush`) comes after the session
gain and the leveler, and neither reads the segmenter's state. A `sttraw` file is therefore exactly what
HardwareOne's segmenter and model hear, and replaying it through any detector arm on the host is exact for the
segmenter input (the recognizer hears the leveler steps the device took). H1-H2 use that; H3 is the device
check after the port. All three are HardwareOne-side: they need HardwareOne's `stt_raw_replay` (for the judge
arm) and, for H2-H3, HardwareOne images, which this plan's builds do not provide.

### H1 Existing recordings, host only (no board; about 2 h of CPU, one job; can run before the session)

Captured 8 Nov on HardwareOne 0.99.99, which predates the leveler and the glasses session gain. Each has a
stored `score.json` (re-decoded 10 Nov with the exponent-sign fix) with HardwareOne's raw-only arm
(`production` there, identical to option 0 and to setting 0) and its judge at 97:

| Drive folder (`stt/sttraw/`) | Recording | Input | Playlist | Stored raw-only / judge 97 WER % |
|---|---|---|---|---|
| `stt10-auto/` (`score-auto`) | `fa73dca6` | onboard, auto gain, x8 120 Hz | ab1 | 59.27 / 43.23 |
| `stt10-auto/` (`score-auto2`) | `b0325933` | onboard, auto gain, x8 120 Hz | ab1 | 70.21 / 46.56 |
| `stt10-g2/` (`score-g2ab1`) | `395b722c` | glasses | ab1 | 29.27 / 28.44 |
| `stt10-g2ft/` (`score-g2ftab1`) | `834e8be1` | glasses | ab1 | 70.94 / 68.65 |
| `stt10-g2ft/` (`score-g2ftnb1`) | `ef8764fe` | glasses | nb1 | 37.69 / 35.83 |

(`e8f93103`, the fifth x8 120 Hz recording, is already scored: `docs/STT_LEVEL_ROBUST_ONSET.md`.) Run the
host scoring wrapper (`SCORE`, "Host scoring" above, gen2-25397 decoder) on each with
`--raw <folder>/raw_<id>.pcm --sidecar <folder>/raw_<id>.json --manifest stt/sttraw/playlists/<ab1|nb1>.json
--out $Q/h1/<id> --replay $R` and the arms `off`, `production` (default), and for the onboard two also `g135`
and `g300h`. `off` must reproduce the stored raw-only arm exactly (it did for e8f93103); if it does not, stop.
For the onboard two, also decode `production` with the P5 blend-g30 decoder.

Rules (proposed):
- Onboard x8: `production` ≤ judge 97 + 1 point on each recording, and false words on noise or silence not
  above the judge's. This is the evidence that the port does not lose against today's HardwareOne at its
  starting gain.
- Glasses (`sttg2decisionpre` after the port; the judge is removed by the port, so the choice is 0 or 97):
  default 97 if, on all three glasses recordings, `production` is not worse than `off` by more than 1 point
  and false words on nb1 noise are at most `off` + 4; otherwise default 0. Report `production` vs judge 97
  as the change against today's glasses captions.

### H2 New raw takes with the production gain chain (board, about 2 h, attended; with S1 and S3)

Needs a HardwareOne image with both `sttraw` and the leveler (4bc86e67 or later). If the R image from S0/1.2
is older (no `TL phrasegain` lines appear in a loud take with `sttautogain 1`), build and flash one with 0.4
(the user's approval covers it only if asked for explicitly). Set `sttautogain 1` and `sttg2autogain 1`
(production); `sttdecisionpre` does not affect the raw file. Takes, each one `sttraw` session (1.2), with
the console log kept to count leveler steps:

| Take | Condition | Length |
|---|---|---|
| H-ab1, H-nb1 | `ab1.wav` / `nb1.wav` at 1 ft, volume 70, onboard mic (day) | 10.3 / 7.4 min |
| H-t3 | Talker 3 ft, script, onboard mic | ~2.5 min |
| H-quiet | Nothing plays, onboard mic (evening, after S3) | 30 min |
| H-g2-ab1, H-g2-nb1 | The same playlists, glasses on a stand at the 1 ft position, glasses mic | 10.3 / 7.4 min |
| H-g2-talk, H-g2-quiet | Wearer reads the script; then 10 min wearing the glasses in the quiet room | ~2.5 / 10 min |

Sidecar checks as in 1.2, except `pdm_amplify` / `pdm_hpf_hz`: record them (expect 8 / 120 onboard), and
`source` 1 (onboard) or 2 (glasses). Score like H1: playlists with `SCORE` (arms `off`, `production`, `g135`,
`g300h`), talker and quiet takes with `rawtake.py`, then once more with `--replay` pointing at HardwareOne's
`stt_raw_replay` built from the same HardwareOne checkout and `--arm judge97="--decision-pre 97"`. Decode
the onboard playlist takes with the P5 blend-g30 decoder too. Same rules as H1, plus H-quiet `production`
≤ 10 words per hour, and H-t3 `production` ≤ `off` − 5 points. A leveler step inside a phrase is invisible to
the onset's gain (it re-learns only while idle); compare hard cuts and noise inside phrases with the x1
results to see whether that matters.

### H3 HardwareOne device after the port (integration order step 2)

On the HardwareOne branch with the ported onset and gen2-25397 (`docs/STT_ENGINE_INTEGRATION.md` section 6,
step 2), on the bench P4 with production gain settings. Repeat S0 for HardwareOne's own image (partition
table, MAC, rollback image) and flash the app only at the verified address.
- Sessions with `sttdecisionpre 0` and `97` (applies to the next session), each with `ab1.wav` and `nb1.wav`
  at 1 ft and `sttraw on`, so every session's audio can be replayed (H2 scoring).
- Glasses: one session each with `sttg2decisionpre 0` and `97` (whichever H1/H2 chose is the default).
- Measure: audio-loss stops (0), dropped phrases, the `stt` task's stack high-water mark from
  `taskStackRecord` (margin ≥ 4 KB; the `Segmenter` in `StreamRun` grows by about 3.2 KB), the
  `stt_capture` stack, caption latency from the log, false captions on nb1 noise items.
- Pass: every hard limit of S2 that HardwareOne exposes, and the `sttraw` replays of these sessions agree
  with H2 in direction.
- Rollback: `sttdecisionpre 0` / `sttg2decisionpre 0`, or the previous app.

## Appendix A: reading script (`script.txt`)

About 230 words, read at a natural pace with a short pause after each line, the same way every take. Lines
1-20 are the Harvard sentences (IEEE 1969 lists 1-2); 21-22 are the two lines from the 9 Nov speaker checks;
23-28 are meeting-style sentences in the decoder LM's domain. No digits, so text normalisation cannot add
errors.

```text
The birch canoe slid on the smooth planks.
Glue the sheet to the dark blue background.
It's easy to tell the depth of a well.
These days a chicken leg is a rare dish.
Rice is often served in round bowls.
The juice of lemons makes fine punch.
The box was thrown beside the parked truck.
The hogs were fed chopped corn and garbage.
Four hours of steady work faced us.
A large size in stockings is hard to sell.
The boy was there when the sun rose.
A rod is used to catch pink salmon.
The source of the huge river is the clear spring.
Kick the ball straight and follow through.
Help the woman get back to her feet.
A pot of tea helps to pass the evening.
Smoky fires lack flame and heat.
The soft cushion broke the man's fall.
The salt breeze came across from the sea.
The girl at the booth sold fifty bonds.
He put his shoes on the table.
It is the duty of member states to guarantee that this information is available and accurate.
Let's move the design review to Thursday afternoon.
I think the budget for the second quarter is still too high.
Could you send me the notes from this morning's meeting?
We should ask the team whether the new schedule works for everyone.
The prototype ran for three days without a single failure.
Before we finish, does anyone have questions about the next steps?
```

## Appendix B: helpers (save to `$W/private/board-test/bin/`)

`rawtake.py`: every detector arm on one raw take with no playlist; same decode as `score.py`.

```python
"""Detector arms on one raw P4 take without a playlist (live talker, quiet room, fan, babble).
  rawtake.py RAW OUT.json [--sidecar J] [--ref SCRIPT.txt] [--seconds S] --replay BIN --cache DIR [--arm NAME="OPTS"]...
Decodes like tools/sttraw_ab/score.py (deployed int8 + meeting.lm, raw[start : min(end, voiced_end + 0.5 s)]).
HW1_SCORE_ROOT: a checkout whose score.py has the 10 Nov exponent fix (-rec.exponent)."""
import argparse, json, os, runpy, sys
from pathlib import Path
import numpy as np
M = Path(os.environ['HW1_SCORE_ROOT'])
sys.path[:0] = [str(M / 'tools'), str(M / 'tools/stt_train/lm')]
import hw1lm  # noqa: E402
from overlap_gate.g0_extract import Recognizer  # noqa: E402
from overlap_gate.caption_wer import norm, edit_ops, LM_PATH  # noqa: E402
sc = runpy.run_path(str(M / 'tools/sttraw_ab/score.py'), run_name='score_mod')
ap = argparse.ArgumentParser()
ap.add_argument('raw', type=Path); ap.add_argument('out', type=Path)
ap.add_argument('--sidecar', type=Path); ap.add_argument('--ref', type=Path); ap.add_argument('--seconds', type=float)
ap.add_argument('--replay', type=Path, required=True); ap.add_argument('--cache', type=Path, required=True)
ap.add_argument('--arm', action='append', default=[])
a = ap.parse_args()
a.out.parent.mkdir(parents=True, exist_ok=True)
rec, lm = Recognizer(a.cache), hw1lm.read_lm(LM_PATH)
raw = np.fromfile(a.raw, '<i2')
if a.seconds:
    raw = raw[:int(a.seconds * 16000)]
src = a.raw
if a.seconds:  # replay exactly the scored part
    src = a.out.with_suffix('.cut.pcm'); raw.tofile(src)
ref = norm(a.ref.read_text()) if a.ref else None
hours = raw.size / 16000 / 3600
out = dict(raw=str(a.raw), seconds=raw.size / 16000, decoder='int8 gen2-25397 + meeting.lm', arms={})
for name, opts in [('production', '')] + [tuple(x.split('=', 1)) for x in a.arm]:
    r = sc['replay'](src, opts, a.sidecar, a.replay)
    segs = r['segments']
    cuts = [raw[g['start']:min(g['end'], g['voiced_end'] + sc['TAIL'])] for g in segs]
    texts = [hw1lm.decode_int8(rec.logits(x), -rec.exponent, lm) if x.size >= 320 else '' for x in cuts]
    hyp = [w for t in texts for w in norm(t)]
    res = dict(options=opts, segmenter_config=r.get('segmenter_config'), segments=len(segs),
               hard_limit_cuts=r.get('hard_limit_cuts'), decode_s=round(sum(x.size for x in cuts) / 16000, 2),
               inside_s=round(r['segments_covered_samples'] / 16000, 2), words=len(hyp),
               words_per_hour=round(len(hyp) / hours, 1), texts=texts)
    if ref is not None:
        s, d, i = edit_ops(ref, hyp)
        res.update(ref_words=len(ref), S=s, D=d, I=i, wer=round(100 * (s + d + i) / max(1, len(ref)), 2))
    out['arms'][name] = res
    print(name, {k: v for k, v in res.items() if k not in ('texts', 'segmenter_config')}, flush=True)
a.out.write_text(json.dumps(out) + '\n')
```

`devsum.py`: one JSON line per standalone serial log (realtime, resources, captions; never caption text).

```python
"""Summarise standalone serial logs (night_driver.py runs/<run>/serial.log): realtime, resources, captions.
  devsum.py SERIAL.log... [--ref SCRIPT.txt] > summary.jsonl     (needs numpy; HW1_SCORE_ROOT for norm/edit_ops)
Counts from the first 'Listening' line to the last line. Never prints caption text."""
import argparse, json, os, re, sys
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(os.environ['HW1_SCORE_ROOT']) / 'tools'))
from overlap_gate.caption_wer import norm, edit_ops  # noqa: E402
ap = argparse.ArgumentParser(); ap.add_argument('logs', nargs='+', type=Path); ap.add_argument('--ref', type=Path)
a = ap.parse_args()
ref = norm(a.ref.read_text()) if a.ref else None
FAIL = re.compile(r'Guru Meditation|abort\(\)|Model load failed|[Ss]tack (overflow|protection)|Task watchdog')
for log in a.logs:
    rows = [l.rstrip('\n').split('\t', 2) for l in log.open(errors='replace') if l.count('\t') >= 2]
    start = next((i for i, r in enumerate(rows) if r[2].startswith('Listening (noise floor')), None)
    if start is None:
        print(json.dumps(dict(log=str(log), error='no Listening line'))); continue
    body, perf, closes, finals = rows[start:], {}, [], []
    s = dict(log=str(log), listening=rows[start][2], hours=round((float(body[-1][1]) - float(body[0][1])) / 3600, 3),
             model_ready=next((r[2] for r in rows if r[2].startswith('Model ready')), None), worker_ms=0, final_samples=0,
             resets=sum('rst:0x' in r[2] for r in body), failures=sum(bool(FAIL.search(r[2])) for r in body if 'STT_EVENT' not in r[2]), stack_min=None)
    for _, _, t in body:
        if m := re.search(r'TL stream .*kind=(\d) .*samples=(\d+) .*worker_ms=(\d+) close_ms=(\d+)', t):
            s['worker_ms'] += int(m[3])
            if m[1] == '0':
                s['final_samples'] += int(m[2]); closes.append(int(m[4]))
        elif m := re.search(r'STT_EVENT (\{.*"type":"final".*\})', t):
            finals.append(json.loads(m[1]))
        elif m := re.search(r'STT_PERF (\{.*\})', t):
            e = json.loads(m[1]); perf.setdefault(e['type'], []).append(e)
        elif m := re.search(r'STACK capture_min_free=(\d+)', t):
            s['stack_min'] = int(m[1]) if s['stack_min'] is None else min(s['stack_min'], int(m[1]))
    words = [w for f in finals for w in norm(f['rawText'])]
    s.update(finals=len(finals), words=len(words), words_per_hour=round(len(words) / max(s['hours'], 1e-9), 1),
             forced=sum(f['forcedBoundary'] for f in finals), dropped=max([f['droppedPhrases'] for f in finals] or [0]),
             decode_audio_s=round(s['final_samples'] / 16000, 1),
             duty=round(s['worker_ms'] / 3.6e6 / max(s['hours'], 1e-9), 3),
             close_ms=dict(zip(('p50', 'p90', 'max'), np.percentile(closes, [50, 90, 100]).round().tolist())) if closes else None)
    if len(perf.get('capture', [])) >= 2:
        c0, c1 = perf['capture'][0], perf['capture'][-1]
        n = c1['push']['count'] - c0['push']['count']
        s['capture'] = dict(push_mean_us=round((c1['push']['sum_us'] - c0['push']['sum_us']) / max(1, n)),
                            push_max_us=c1['push']['max_us'], interval_max_us=c1['interval']['max_us'],
                            overflow=c1['overflow_count_u32'], rejected=c1['rejected'], reads=c1['reads'] - c0['reads'])
    if perf.get('state'):
        e = perf['state'][-1]
        s['state'] = {k: e[k] for k in ('drops', 'ready_high_water', 'feature_overruns', 'internal_min_boot', 'psram_min_boot',
                                        'internal_largest', 'owner_stack_min_bytes')}
    if perf.get('lock'):
        push = [e for e in perf['lock'] if e['op'] == 'push']
        s['push_lock_hold_max_us'] = push[-1]['hold']['max_us'] if push else None
    if ref is not None:
        S, D, I = edit_ops(ref, words)
        s.update(ref_words=len(ref), S=S, D=D, I=I, wer=round(100 * (S + D + I) / max(1, len(ref)), 2))
    print(json.dumps(s))
```

Checked on 13 Nov: `rawtake.py` on 60 s of `raw_7c9393cb` (two arms); `devsum.py` on night-1 runs `b01-off` /
`b02-on`, which gave the baselines quoted in S2 (push mean 499-511 µs, max 9.8-37 ms, interval max 74-101 ms,
duty 0.54-0.57, close p90 2.55-2.63 s, ready high water 1, internal min 410,068 B). `capture` and `state`
counters are cumulative since boot; `devsum.py` uses the first and last `perf dump` of a run.

## Appendix C: playlists and silent blocks

`join_blocks.py` (night-1 blocks into one WAV and one `score.py` manifest; checks each block WAV's SHA-256):

```python
"""Join night-1 blocks into one playback WAV and one score.py manifest (clip samples shifted).
  join_blocks.py MANIFEST_DIR OUT_DIR NAME 1 2 3 4 6 10"""
import hashlib, json, sys, wave
from pathlib import Path
src, out, name, blocks = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], [int(b) for b in sys.argv[4:]]
out.mkdir(parents=True, exist_ok=True)
wav, clips, at = out / f'{name}.wav', [], 0
with wave.open(str(wav), 'wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
    for b in blocks:
        m = json.loads((src / f'block{b:02d}.json').read_text())
        assert hashlib.sha256(Path(m['wav']).read_bytes()).hexdigest() == m['wav_sha256'], m['wav']
        with wave.open(m['wav']) as r:
            assert (r.getnchannels(), r.getsampwidth(), r.getframerate()) == (1, 2, 16000)
            n = r.getnframes(); w.writeframes(r.readframes(n))
        for c in m['clips']:
            clips.append(dict(c, block=b, start_sample=c['start_sample'] + at, end_sample=c['end_sample'] + at, index=len(clips)))
        at += n
man = dict(name=name, wav=str(wav), wav_sha256=hashlib.sha256(wav.read_bytes()).hexdigest(), sample_rate=16000,
           seconds=round(at / 16000, 2), blocks=blocks, clips=clips)
(out / f'{name}.json').write_text(json.dumps(man, indent=1) + '\n')
print(name, man['seconds'], 's', len(clips), 'clips', sum(len(c['reference'].split()) for c in clips), 'words')
```

Silent WAVs, the babble bed and the night block (`night_driver.py` needs `block`, `wav`, `wav_sha256`,
`seconds`). Run after `join_blocks.py … $P/wav n1six 1 2 3 4 6 10` has written `$P/wav/n1six.wav`; the paths
inside `block95.json` are absolute, so run it where the bench will read them (or rewrite `wav` after copying):

```sh
python3 - "$P" <<'EOF'
import hashlib, json, sys, wave
from pathlib import Path
P = Path(sys.argv[1]); (P / 'wav').mkdir(parents=True, exist_ok=True); (P / 'manifests/quiet').mkdir(parents=True, exist_ok=True)
for s in (200, 600, 3600):
    f = P / f'wav/silence{s}.wav'
    with wave.open(str(f), 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(b'\0\0' * 16000 * s)
with wave.open(str(P / 'wav/n1six.wav')) as r, wave.open(str(P / 'wav/babble600.wav'), 'wb') as w:
    r.setpos(1800 * 16000); w.setparams(r.getparams()); w.writeframes(r.readframes(600 * 16000))
blk = P / 'wav/silence3600.wav'
(P / 'manifests/quiet/block95.json').write_text(json.dumps(dict(block=95, wav=str(blk), seconds=3600.0,
    wav_sha256=hashlib.sha256(blk.read_bytes()).hexdigest(), clips=[])) + '\n')
EOF
```
