#pragma once
#include <atomic>
#include <cstdint>

// What the wheel can change. Everything is saved on the board and restored at power-up.
namespace settings {
// Applied immediately.
extern std::atomic<bool> listening;      // not saved: the board always starts listening
extern std::atomic<uint8_t> gain;        // index into kGainChoices: automatic, or a fixed gain
extern std::atomic<uint8_t> gainNow;     // not saved: the microphone gain in use right now
extern std::atomic<uint8_t> lmStrength;  // 0..4 = 0, 25, 50, 75, 100 %
extern std::atomic<bool> speakers;       // show who is talking
// Read once at start-up: changing one restarts the board.
extern std::atomic<bool> drafts;         // live drafts while you speak
extern std::atomic<bool> noisyRoom;      // stricter phrase onset for fans and hiss
extern std::atomic<uint8_t> endPause;    // index into kEndPauseMs

// The microphone's own gain, applied before the audio is measured. 0 = chosen automatically.
inline constexpr uint8_t kGainChoices[] = {0, 1, 2, 4, 8};
inline constexpr uint16_t kEndPauseMs[] = {400, 600, 1000};

void load();
void save();
}  // namespace settings
