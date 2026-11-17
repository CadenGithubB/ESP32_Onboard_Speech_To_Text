// Speech to text on the ESP32-P4-EYE, and nothing else.
//
//   PDM microphone -> phrase segmenter -> Citrinet-384 (int8) + word language model -> serial console
//
// Put two files in a folder named "STT Models" on a FAT microSD card:
//   citrinet384-2000h.p4.stt          the model this build is pinned to
//   meeting.lm                        the language model (optional; less accurate without it)
// then build, flash, open the serial monitor and talk. Live drafts appear while you speak
// and the final caption replaces them when you pause.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

#include "stt/citrinet_runtime.h"
#include "stt/quartznet_runtime.h"  // the runtime API both models share
#include "stt/stt_lm.h"
#include "stt/stt_lm_wordpiece.h"
#include "stt/stt_model_selection.h"
#include "stt/stt_stream.h"

namespace {
// Files on the card. The engine checks the model's size and SHA-256 against
// components/hw1_stt/stt/stt_model_identity_citrinet384.h and refuses anything else.
constexpr const char* kMountPoint = "/sd";
constexpr const char* kModelPath = "/sd/STT Models/citrinet384-2000h.p4.stt";
constexpr const char* kLmPath = "/sd/STT Models/meeting.lm";

// ESP32-P4-EYE wiring. Another P4 board: change these.
// microSD: SDMMC slot 0, 4-bit; on-chip LDO channel 4; GPIO46 switches card power (low = on).
constexpr gpio_num_t kSdClk = GPIO_NUM_43, kSdCmd = GPIO_NUM_44;
constexpr gpio_num_t kSdD0 = GPIO_NUM_39, kSdD1 = GPIO_NUM_40, kSdD2 = GPIO_NUM_41, kSdD3 = GPIO_NUM_42;
constexpr gpio_num_t kSdDetect = GPIO_NUM_45;  // low = card present
constexpr gpio_num_t kSdPower = GPIO_NUM_46;   // low = on
constexpr int kSdLdoChannel = 4;
// PDM microphone on I2S0. GPIO12 powers the rail the microphone shares with the camera and LCD.
constexpr gpio_num_t kMicClk = GPIO_NUM_22, kMicData = GPIO_NUM_21, kMicPower = GPIO_NUM_12;

constexpr uint32_t kSampleRate = 16000;
constexpr size_t kPhraseSamples = 10 * kSampleRate;  // a phrase is at most 10 s
constexpr size_t kReadSamples = 1024;                // one PDM DMA block

hw1::stt::StreamPipeline gPipeline;
SemaphoreHandle_t gPipelineMutex = nullptr;
void streamLock(void*) { xSemaphoreTake(gPipelineMutex, portMAX_DELAY); }
void streamUnlock(void*) { xSemaphoreGive(gPipelineMutex); }
uint32_t nowMs() { return uint32_t(esp_timer_get_time() / 1000); }

[[noreturn]] void halt(const char* why) {
  for (;;) {  // repeated, so it is still there when the monitor is opened late
    printf("STOPPED: %s\n", why);
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// ---------------------------------------------------------------- microSD
const char* mountCard() {
  gpio_config_t pins = {};
  pins.pin_bit_mask = 1ULL << kSdPower;
  pins.mode = GPIO_MODE_OUTPUT;
  gpio_set_level(kSdPower, 1);  // off until the LDO is up
  gpio_config(&pins);
  pins = {};
  pins.pin_bit_mask = 1ULL << kSdDetect;
  pins.mode = GPIO_MODE_INPUT;
  pins.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&pins);

  sd_pwr_ctrl_handle_t power = nullptr;
  sd_pwr_ctrl_ldo_config_t ldo = {};
  ldo.ldo_chan_id = kSdLdoChannel;
  if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &power) != ESP_OK) return "microSD power supply did not start";
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(kSdPower, 0);
  vTaskDelay(pdMS_TO_TICKS(20));
  if (gpio_get_level(kSdDetect) != 0) return "no microSD card detected; insert the card and press reset";

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = SDMMC_HOST_SLOT_0;
  host.max_freq_khz = 40000;
  host.pwr_ctrl_handle = power;
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.clk = kSdClk;
  slot.cmd = kSdCmd;
  slot.d0 = kSdD0;
  slot.d1 = kSdD1;
  slot.d2 = kSdD2;
  slot.d3 = kSdD3;
  slot.cd = kSdDetect;
  slot.width = 4;
  slot.flags = 0;  // the board has bus pull-ups

  esp_vfs_fat_sdmmc_mount_config_t mount = {};
  mount.format_if_mount_failed = false;
  mount.max_files = 4;
  sdmmc_card_t* card = nullptr;
  if (esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot, &mount, &card) != ESP_OK)
    return "microSD card did not mount; it must be FAT32 formatted";
  return nullptr;
}

// ---------------------------------------------------------------- microphone
i2s_chan_handle_t gMic = nullptr;

bool startMic() {
  gpio_set_level(kMicPower, 1);  // latch first, then drive, so the shared rail never dips
  if (gpio_set_direction(kMicPower, GPIO_MODE_OUTPUT) != ESP_OK) return false;

  i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan.dma_desc_num = 4;
  chan.dma_frame_num = kReadSamples;
  if (i2s_new_channel(&chan, nullptr, &gMic) != ESP_OK) return false;

  i2s_pdm_rx_slot_config_t slot =
#ifdef I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG
      I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
#else
      I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
#endif
  i2s_pdm_rx_config_t cfg = {
      .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(kSampleRate),
      .slot_cfg = slot,
      .gpio_cfg = {.clk = kMicClk, .din = kMicData, .invert_flags = {.clk_inv = false}},
  };
  // 16 kHz x 64 = 1.024 MHz falls between the microphone's low-power and standard clock
  // bands; DSR16 doubles the PDM clock and keeps the 16 kHz PCM rate.
  cfg.clk_cfg.dn_sample_mode = I2S_PDM_DSR_16S;
  if (i2s_channel_init_pdm_rx_mode(gMic, &cfg) != ESP_OK) return false;
  if (i2s_channel_enable(gMic) != ESP_OK) return false;

  static int16_t warm[kReadSamples];  // let the microphone settle: discard three blocks
  for (int i = 0; i < 3; ++i) {
    size_t bytes = 0;
    i2s_channel_read(gMic, warm, sizeof(warm), &bytes, 500);
  }
  return true;
}

// Microphone -> pipeline. The pipeline cuts the audio into phrases.
void captureTask(void*) {
  static int16_t block[kReadSamples];
  for (;;) {
    size_t bytes = 0;
    const esp_err_t err = i2s_channel_read(gMic, block, sizeof(block), &bytes, 200);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (bytes && !gPipeline.push(block, bytes / sizeof(int16_t), nowMs())) {
      printf("Capture stopped: the pipeline rejected audio\n");
      vTaskDelete(nullptr);
    }
  }
}

// ---------------------------------------------------------------- language model
struct LanguageModel {
  uint8_t* blob = nullptr;
  hw1::stt::lm::Lm view;
  bool tried = false;
};

void loadLanguageModel(LanguageModel& m) {
  namespace lm = hw1::stt::lm;
  struct stat st = {};
  if (stat(kLmPath, &st) != 0) {
    printf("No %s: decoding without a language model\n", kLmPath);
    return;
  }
  const size_t bytes = size_t(st.st_size);
  constexpr size_t kReserve = 3u << 20;  // PSRAM a 10 s phrase needs beside the language model
  if (bytes < 96 || heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < bytes ||
      heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < bytes + kReserve) {
    printf("Language model skipped (%u bytes; not enough PSRAM)\n", unsigned(bytes));
    return;
  }
  m.blob = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  FILE* f = m.blob ? fopen(kLmPath, "rb") : nullptr;
  bool ok = f != nullptr;
  for (size_t done = 0; ok && done < bytes;) {
    const size_t n = fread(m.blob + done, 1, std::min<size_t>(bytes - done, 64 * 1024), f);
    if (!n) ok = false;
    done += n;
  }
  if (f) fclose(f);
  const lm::LoadStatus status = ok ? m.view.open(m.blob, bytes) : lm::LoadStatus::InvalidArgument;
  if (status != lm::LoadStatus::Ok) {
    printf("Language model rejected (%s): decoding without it\n", lm::describe(status));
    m.view = lm::Lm{};
    heap_caps_free(m.blob);
    m.blob = nullptr;
    return;
  }
  printf("Language model: %u words (%u bytes)\n", unsigned(m.view.vocab()), unsigned(bytes));
}

hw1::stt::DecodeResult decodeWithLanguageModel(void* context, const int8_t* logits, size_t frames,
                                               int exponent, char* text, size_t capacity,
                                               const STTLocalControl& control,
                                               const hw1::stt::DecodeScratch& scratch) {
  namespace lm = hw1::stt::lm;
  auto& m = *static_cast<LanguageModel*>(context);
  if (!m.tried) {  // loaded at the first decode, when the model's own memory already exists
    m.tried = true;
    loadLanguageModel(m);
  }
  if (!m.view.valid()) return hw1::stt::DecodeResult::Fallback;
  // Citrinet writes word pieces, not letters: the search joins pieces into words and scores
  // each finished word with the language model. Weight 0.2 and word bonus 0.75 are the values
  // tuned for this model; the runtime lends its idle activation memory as workspace.
  const lm::wordpiece::Pieces* pieces = hw1::stt::citrinetPieces();
  if (!pieces) return hw1::stt::DecodeResult::Fallback;
  const double strength = 1.0;
  lm::wordpiece::Params params = lm::wordpiece::fileParams(m.view);
  params.alpha = 0.2 * strength;
  params.beta = 0.75 * strength;
  params.hotwordBonus *= strength;
  const lm::DecodeStatus status = lm::wordpiece::decode(
      logits, frames, size_t(hw1::stt::identity::kOutputClasses), exponent, m.view, *pieces, params, nullptr, text,
      capacity, {control.context, control.cancelled}, nullptr, scratch.data, scratch.bytes);
  if (status == lm::DecodeStatus::Ok) return hw1::stt::DecodeResult::Decoded;
  if (status == lm::DecodeStatus::Cancelled) return hw1::stt::DecodeResult::Cancelled;
  return hw1::stt::DecodeResult::Fallback;
}

// The engine frees the language model through this if a phrase needs the memory.
bool releaseLanguageModel(void* context) {
  auto& m = *static_cast<LanguageModel*>(context);
  if (!m.blob) return false;
  m.view = hw1::stt::lm::Lm{};
  heap_caps_free(m.blob);
  m.blob = nullptr;
  printf("Language model released to make room\n");
  return true;
}

// ---------------------------------------------------------------- transcription
hw1::stt::ModelCache gWeights;  // the verified model stays in memory between phrases
LanguageModel gLm;
const hw1::stt::Decoder gDecoder{&gLm, decodeWithLanguageModel, releaseLanguageModel};

size_t readModel(void* file, void* dest, size_t bytes) { return fread(dest, 1, bytes, static_cast<FILE*>(file)); }

bool transcribe(const int16_t* pcm, size_t samples, char* text, size_t capacity, STTLocalStats& stats,
                char* error, size_t errorCapacity, const STTLocalControl& control = {},
                const STTFeaturePrefix* prefix = nullptr) {
  stats = {};
  FILE* f = fopen(kModelPath, "rb");
  struct stat st = {};
  if (!f || fstat(fileno(f), &st) != 0) {
    if (f) fclose(f);
    snprintf(error, errorCapacity, "cannot open %s", kModelPath);
    return false;
  }
  const hw1::stt::ModelReader reader{f, size_t(st.st_size), readModel};
  const bool ok = hw1::stt::transcribe(reader, pcm, samples, text, capacity, control, stats, error,
                                       errorCapacity, nullptr, &gWeights, &gDecoder, prefix);
  fclose(f);
  return ok;
}

bool cancelWork(void* work) { return gPipeline.cancelled(*static_cast<const hw1::stt::StreamWork*>(work)); }
}  // namespace

extern "C" void app_main() {
  printf("\nESP32-P4-EYE speech to text\n");
  if (const char* failure = mountCard()) halt(failure);
  struct stat st = {};
  if (stat(kModelPath, &st) != 0)
    halt("model file missing: the card needs \"STT Models/citrinet384-2000h.p4.stt\"");

  vTaskPrioritySet(nullptr, 2);
  gPipelineMutex = xSemaphoreCreateMutex();
  hw1::stt::StreamBuffers buffers;
  buffers.capacity = kPhraseSamples;
  auto phraseBuffer = [] {
    return static_cast<int16_t*>(heap_caps_malloc(kPhraseSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  };
  buffers.capture = phraseBuffer();
  for (auto& slot : buffers.slots) slot = phraseBuffer();
  buffers.snapshot = phraseBuffer();  // for live drafts

  hw1::stt::StreamConfig cfg;
  cfg.segmentation.adaptive = true;
  cfg.segmentation.min_segment_samples = 4 * kSampleRate;
  cfg.segmentation.max_segment_samples = kPhraseSamples;
  cfg.segmentation.short_close_silence_samples = kSampleRate;
  cfg.segmentation.noise_percentile = 25;
  cfg.segmentation.silence_blip_samples = 3 * hw1::stt::kSegmentAnalysisSamples;
  cfg.segmentation.hard_cut_search_samples = 2 * kSampleRate;
  // Level-robust phrase onset: quiet or distant speech also opens a phrase.
  cfg.segmentation.decision_pre_emphasis = 97;
  cfg.segmentation.decision_gain_pct = 150;
  cfg.drafts = true;
  cfg.speculative = false;
  if (!gPipelineMutex || !gPipeline.begin(cfg, buffers, {nullptr, streamLock, streamUnlock}))
    halt("not enough memory for the audio buffers");

  // Load and verify the model now, so the first phrase is not delayed by the 20 MB read.
  static char text[hw1::stt::kStreamText + 1];
  static char error[128];
  STTLocalStats stats;
  {
    constexpr size_t kWarmSamples = kSampleRate / 2;
    auto* silence = static_cast<int16_t*>(heap_caps_calloc(kWarmSamples, sizeof(int16_t), MALLOC_CAP_SPIRAM));
    const int64_t t0 = esp_timer_get_time();
    if (!silence || !transcribe(silence, kWarmSamples, text, sizeof(text), stats, error, sizeof(error))) {
      printf("Model load failed: %s\n", error);
      halt("the card must hold the exact model file this build is pinned to");
    }
    heap_caps_free(silence);
    printf("Model ready in %lld ms\n", (esp_timer_get_time() - t0) / 1000);
  }

  if (!startMic()) halt("microphone did not start");
  if (xTaskCreatePinnedToCore(captureTask, "capture", 8192, nullptr, 3, nullptr, 1) != pdPASS)
    halt("could not start the capture task");
  printf("Measuring room noise, stay quiet for a moment...\n");

  bool announced = false;
  uint32_t dropped = 0;
  for (;;) {
    hw1::stt::StreamWork work;
    if (!gPipeline.take(work, nowMs())) {
      if (!announced && gPipeline.calibrated()) {
        announced = true;
        printf("Listening. Speak now.\n");
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    const STTLocalControl control{&work, cancelWork, nullptr};
    const int64_t t0 = esp_timer_get_time();
    const bool ok = transcribe(work.pcm, work.samples, text, sizeof(text), stats, error, sizeof(error), control,
                               &work.features);
    const uint32_t ms = uint32_t((esp_timer_get_time() - t0) / 1000);
    const bool valid = ok && !gPipeline.cancelled(work);
    if (work.kind == hw1::stt::StreamWorkKind::Draft) {
      if (valid && text[0]) printf("            ... %s\n", text);
    } else if (valid && text[0]) {
      printf("[%7.1f s] %s\n", double(work.start_sample) / kSampleRate, text);
      printf("            %.1f s of audio in %u ms: features %u, model %u, decode %u ms%s\n",
             double(work.samples) / kSampleRate, unsigned(ms), unsigned(stats.frontendMs),
             unsigned(stats.inferenceMs), unsigned(stats.decodeMs), stats.lmUsed ? ", language model" : "");
    } else if (!ok) {
      printf("Transcription failed: %s\n", error);
    }
    gPipeline.complete(work, valid, text, stats, ms);
    const uint32_t nowDropped = gPipeline.counters().dropped;
    if (nowDropped != dropped) {
      printf("%u phrase(s) dropped while transcribing\n", unsigned(nowDropped - dropped));
      dropped = nowDropped;
    }
  }
}
