#include "speaker.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "stt/speaker_cluster.h"
#include "stt/speaker_model.h"
#include "stt/stt_dl_metadata.h"

namespace speaker {
namespace {
constexpr const char* kPath = "/sd/STT Models/speaker.spk";
constexpr int kJobs = 4;
constexpr size_t kMaxSamples = 10 * 16000;

struct Job {
  uint32_t caption = 0;
  size_t samples = 0, phraseSamples = 0;
};
struct Result {
  uint32_t caption = 0;
  int8_t speaker = -1;
};
struct State {
  hw1::stt::speaker::Model model;
  hw1::stt::speaker::Clusters clusters;
  hw1::stt::speaker::ClusterConfig config;
  uint8_t* blob = nullptr;
  float* centroids = nullptr;
  float* embedding = nullptr;
  int16_t* pcm = nullptr;  // kJobs phrase windows
  size_t span = 0;         // samples per window
  Job jobs[kJobs];
  QueueHandle_t ready = nullptr, free = nullptr, results = nullptr;
};
State* gState = nullptr;

// One phrase at a time, single-core and below caption priority.
void worker(void* pointer) {
  auto& s = *static_cast<State*>(pointer);
  for (;;) {
    uint8_t slot = 0;
    if (xQueueReceive(s.ready, &slot, portMAX_DELAY) != pdTRUE) continue;
    const Job job = s.jobs[slot];
    Result result{job.caption, -1};
    if (s.model.embed(s.pcm + size_t(slot) * s.span, job.samples, s.embedding))
      result.speaker = s.clusters.assign(s.embedding, job.phraseSamples).speaker;
    xQueueSend(s.results, &result, 0);
    xQueueSend(s.free, &slot, 0);
    vTaskDelay(1);
  }
}
}  // namespace

bool begin(size_t reserve, size_t contiguous) {
  if (gState) return true;
  struct stat info {};
  if (stat(kPath, &info) != 0) {
    printf("No %s: captions without speaker labels\n", kPath);
    return false;
  }
  const size_t bytes = size_t(info.st_size);
  if (bytes < 192 || bytes > (16u << 20) ||
      heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < bytes + (1u << 20) + reserve) {
    printf("Speaker labels off: not enough PSRAM beside the caption model\n");
    return false;
  }
  void* memory = heap_caps_malloc(sizeof(State), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!memory) return false;
  auto* s = new (memory) State;
  s->blob = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  FILE* f = s->blob ? fopen(kPath, "rb") : nullptr;
  bool ok = f != nullptr;
  for (size_t read = 0; ok && read < bytes;) {
    const size_t n = fread(s->blob + read, 1, std::min<size_t>(bytes - read, 65536), f);
    if (!n) ok = false;
    read += n;
  }
  if (f) fclose(f);
  if (ok) {
    const auto status =
        s->model.load(s->blob, bytes, kMaxSamples, hw1::stt::sttPsramAlloc, hw1::stt::sttPsramFree);
    ok = status == hw1::stt::speaker::LoadStatus::Ok;
    if (!ok) printf("Speaker model rejected: %s\n", hw1::stt::speaker::describe(status));
  }
  if (ok) {
    const auto& m = s->model.info();
    s->span = std::min<size_t>(kMaxSamples, size_t(m.features.window_frames - 1) * m.features.hop);
    s->centroids = static_cast<float*>(heap_caps_malloc(8 * m.dim * sizeof(float), MALLOC_CAP_SPIRAM));
    s->embedding = static_cast<float*>(heap_caps_malloc(m.dim * sizeof(float), MALLOC_CAP_SPIRAM));
    s->pcm = static_cast<int16_t*>(heap_caps_malloc(kJobs * s->span * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    s->config.threshold = m.threshold;
    s->config.min_new_samples = size_t(m.minNewMs) * 16;
    s->config.max_speakers = 8;
    s->ready = xQueueCreate(kJobs, sizeof(uint8_t));
    s->free = xQueueCreate(kJobs, sizeof(uint8_t));
    s->results = xQueueCreate(2 * kJobs + 4, sizeof(Result));
    ok = s->span && s->centroids && s->embedding && s->pcm && s->ready && s->free && s->results &&
         s->clusters.reset(s->centroids, m.dim, s->config);
  }
  // Captions come first: keep their memory free, or run without labels.
  if (ok)
    ok = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= reserve &&
         heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= 140 * 1024 &&
         heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= contiguous;
  if (ok) {
    for (uint8_t i = 0; i < kJobs; ++i) xQueueSend(s->free, &i, 0);
    s->model.setMultiCore(false);
    ok = xTaskCreatePinnedToCore(worker, "stt_speaker", 8192, s, 1, nullptr, 1) == pdPASS;
  }
  if (!ok) {
    printf("Speaker labels off: the model did not load or memory is short\n");
    s->model.reset();
    heap_caps_free(s->pcm);
    heap_caps_free(s->embedding);
    heap_caps_free(s->centroids);
    heap_caps_free(s->blob);
    if (s->ready) vQueueDelete(s->ready);
    if (s->free) vQueueDelete(s->free);
    if (s->results) vQueueDelete(s->results);
    s->~State();
    heap_caps_free(s);
    return false;
  }
  gState = s;
  printf("Speaker labels ready\n");
  return true;
}

void submit(uint32_t caption, const int16_t* pcm, size_t samples) {
  if (!gState || !pcm || !samples) return;
  uint8_t slot = 0;
  if (xQueueReceive(gState->free, &slot, 0) != pdTRUE) return;  // busy: this caption stays unlabelled
  const size_t n = std::min(samples, gState->span), offset = (samples - n) / 2;
  std::memcpy(gState->pcm + size_t(slot) * gState->span, pcm + offset, n * sizeof(int16_t));
  gState->jobs[slot] = {caption, n, samples};
  if (xQueueSend(gState->ready, &slot, 0) != pdTRUE) xQueueSend(gState->free, &slot, 0);
}

void poll(void (*labelled)(uint32_t, int)) {
  if (!gState) return;
  Result result;
  while (xQueueReceive(gState->results, &result, 0) == pdTRUE) labelled(result.caption, result.speaker);
}
}  // namespace speaker
