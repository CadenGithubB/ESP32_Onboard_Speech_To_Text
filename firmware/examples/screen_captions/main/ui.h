#pragma once
#include <cstdint>

// The screen and the wheel. Safe to call from any task.
namespace ui {
bool begin();                         // false: no screen (captions still go to serial)
void status(const char* text);        // top line
void level(uint16_t level, uint16_t noise);
uint32_t caption(const char* text);   // adds a final caption; returns its id
void speaker(uint32_t captionId, int speaker);
void draft(const char* text);         // "" clears it
void fatal(const char* text);
}  // namespace ui
