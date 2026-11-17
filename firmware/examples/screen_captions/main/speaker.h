#pragma once
#include <cstddef>
#include <cstdint>

// Who is talking: a small model that runs in the background after each caption and
// numbers the voices it has heard. It never delays captions. Needs the optional file
// "STT Models/speaker.spk" on the card; without it this does nothing.
namespace speaker {
bool begin(size_t captionReserve, size_t contiguous);  // false: labels unavailable
void submit(uint32_t captionId, const int16_t* pcm, size_t samples);
// Calls back once per finished label (speaker 0..7, or -1 for unknown).
void poll(void (*labelled)(uint32_t captionId, int speaker));
}  // namespace speaker
