#include "settings.h"

#include "nvs.h"
#include "nvs_flash.h"

namespace settings {
std::atomic<bool> listening{true};
std::atomic<uint8_t> gain{0};
std::atomic<uint8_t> gainNow{8};
std::atomic<uint8_t> lmStrength{4};
std::atomic<bool> speakers{true};
std::atomic<bool> drafts{true};
std::atomic<bool> noisyRoom{false};
std::atomic<uint8_t> endPause{1};

namespace {
constexpr const char* kNamespace = "captions";
constexpr const char* kKey = "settings2";
struct Stored {
  uint8_t gain, lmStrength, speakers, drafts, noisyRoom, endPause;
};
bool gReady = false;
}  // namespace

void load() {
  // Never erase: on a board that has run other firmware the partition may hold its data.
  gReady = nvs_flash_init() == ESP_OK;
  nvs_handle_t handle;
  if (!gReady || nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return;
  Stored s;
  size_t bytes = sizeof(s);
  if (nvs_get_blob(handle, kKey, &s, &bytes) == ESP_OK && bytes == sizeof(s) && s.gain < 5 &&
      s.lmStrength < 5 && s.endPause < 3) {
    gain = s.gain;
    lmStrength = s.lmStrength;
    speakers = s.speakers != 0;
    drafts = s.drafts != 0;
    noisyRoom = s.noisyRoom != 0;
    endPause = s.endPause;
  }
  nvs_close(handle);
}

void save() {
  nvs_handle_t handle;
  if (!gReady || nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
  const Stored s{gain, lmStrength, speakers, drafts, noisyRoom, endPause};
  if (nvs_set_blob(handle, kKey, &s, sizeof(s)) == ESP_OK) nvs_commit(handle);
  nvs_close(handle);
}
}  // namespace settings
