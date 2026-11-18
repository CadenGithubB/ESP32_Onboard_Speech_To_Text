#pragma once
#include <cstddef>
#include <cstdint>

// Automatic onboard-mic gain for STT sessions (PDM hardware multiplier, 1..15).
//
// The PDM channel is opened per capture, so the gain is chosen when a session
// starts and fixed for its length (changing it mid-session would restart the
// channel and drop audio). Each session measures what STT actually received
// (AutoGainMeter) and, if it heard speech, records the gain that session
// would have wanted (AutoGainHistory):
//   * sustained clipping wants half the gain;
//   * otherwise the gain that puts its loud speech (the third loudest 10 ms
//     frame, so one knock or tap does not decide) near kAutoGainTargetRms.
// The gain changes only when the last kAutoGainAgree recorded sessions all
// want a change in the same direction, and then only to the most cautious of
// their wishes, at most doubling or halving. The history is cleared after a
// change, so the next one again needs that many sessions. One loud or quiet
// session, or a session without speech, never moves it.
// The speech detector adapts to the room on its own within each session
// (noise percentile, 2 x room onset, pre-emphasis calibration), so only the
// resolution is chosen here. Bench basis 2026-11-07/08 (P4X-EYE, speaker at
// 1 ft): x8 was the best measured gain with no false captions; up close x1..x8
// scored the same.
namespace hw1::stt {

inline constexpr uint8_t kAutoGainMin = 1;
inline constexpr uint8_t kAutoGainMax = 8;
inline constexpr uint8_t kAutoGainStart = 8;
inline constexpr uint16_t kAutoGainTargetRms = 4096;   // -18 dBFS frame RMS: ~18 dB of crest headroom
inline constexpr int16_t kAutoGainClipLevel = 32000;
inline constexpr size_t kAutoGainFrameSamples = 160;   // 10 ms at 16 kHz
inline constexpr uint64_t kAutoGainMinSamples = 32000; // 2 s

struct AutoGainStats {
    uint64_t samples = 0;
    uint32_t clipped = 0;        // samples at or beyond kAutoGainClipLevel
    uint16_t loud[3] = {};       // three loudest frame RMS values, descending
};

// Accumulates AutoGainStats over a session, one block at a time.
class AutoGainMeter {
public:
    void reset() { stats_ = {}; count_ = 0; square_ = 0; }
    void push(const int16_t* pcm, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const int32_t x = pcm[i];
            if (x >= kAutoGainClipLevel || x <= -kAutoGainClipLevel) ++stats_.clipped;
            square_ += static_cast<uint64_t>(static_cast<int64_t>(x) * x);
            if (++count_ == kAutoGainFrameSamples) close_frame();
        }
        stats_.samples += n;
    }
    const AutoGainStats& stats() const { return stats_; }

private:
    // floor(sqrt(x)), bit by bit: 16 iterations for a 32-bit result.
    static uint32_t isqrt(uint64_t x) {
        uint64_t r = 0, bit = uint64_t(1) << 62;
        while (bit > x) bit >>= 2;
        while (bit) {
            if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; }
            else r >>= 1;
            bit >>= 2;
        }
        return static_cast<uint32_t>(r);
    }
    void close_frame() {
        const uint32_t v = isqrt(square_ / count_);  // frames are <= 32768 RMS
        const uint16_t rms = static_cast<uint16_t>(v > 65535 ? 65535 : v);
        for (int k = 0; k < 3; ++k) {
            if (rms > stats_.loud[k]) {
                for (int j = 2; j > k; --j) stats_.loud[j] = stats_.loud[j - 1];
                stats_.loud[k] = rms;
                break;
            }
        }
        count_ = 0;
        square_ = 0;
    }
    AutoGainStats stats_;
    size_t count_ = 0;
    uint64_t square_ = 0;
};

inline uint8_t clampAutoGain(uint32_t g) {
    return static_cast<uint8_t>(g < kAutoGainMin ? kAutoGainMin : g > kAutoGainMax ? kAutoGainMax : g);
}

// The gain one session would have wanted, given the gain `used` and what it
// measured; 0 when it says nothing (too short).
inline uint8_t wantedAutoGain(uint8_t used, const AutoGainStats& s) {
    used = clampAutoGain(used);
    if (s.samples < kAutoGainMinSamples) return 0;
    // Clipping on more than 1 in 20,000 samples (~3 ms per minute): half.
    if (static_cast<uint64_t>(s.clipped) * 20000 > s.samples) return clampAutoGain(used / 2u);
    const uint32_t loud = s.loud[2] ? s.loud[2] : 1;
    // Nearest step: at the target the wish is the gain in use, so it settles
    // instead of creeping down (bench 2026-11-08, close: x5 measured 4130 -> x5).
    return clampAutoGain((static_cast<uint32_t>(used) * kAutoGainTargetRms + loud / 2) / loud);
}

inline constexpr size_t kAutoGainHistory = 4;  // sessions kept
inline constexpr size_t kAutoGainAgree = 3;    // newest sessions that must agree before a change

// The last few sessions' wishes and the gain in force. Not thread-safe; the
// owner serialises access (System_STT: gSttMux).
class AutoGainHistory {
public:
    uint8_t gain() const { return gain_; }
    size_t size() const { return count_; }
    uint8_t wish(size_t newest_first) const {
        return wish_[(head_ + kAutoGainHistory - 1 - newest_first) % kAutoGainHistory];
    }
    // Records one session (heardSpeech: it produced at least one phrase) and
    // returns the gain for the next session.
    uint8_t record(uint8_t used, const AutoGainStats& s, bool heardSpeech) {
        const uint8_t w = heardSpeech ? wantedAutoGain(used, s) : 0;
        if (!w) return gain_;
        wish_[head_] = w;
        head_ = (head_ + 1) % kAutoGainHistory;
        if (count_ < kAutoGainHistory) ++count_;
        if (count_ < kAutoGainAgree) return gain_;
        bool up = true, down = true;
        uint8_t lowest = 255, highest = 0;
        for (size_t k = 0; k < kAutoGainAgree; ++k) {
            const uint8_t v = wish(k);
            up = up && v > gain_;
            down = down && v < gain_;
            if (v < lowest) lowest = v;
            if (v > highest) highest = v;
        }
        uint8_t next = gain_;
        if (up) next = clampAutoGain(lowest < gain_ * 2u ? lowest : gain_ * 2u);
        else if (down) next = clampAutoGain(highest > gain_ / 2u ? highest : gain_ / 2u);
        if (next != gain_) {
            gain_ = next;
            count_ = 0;  // the next change needs fresh agreement
        }
        return gain_;
    }

private:
    uint8_t gain_ = kAutoGainStart;
    uint8_t wish_[kAutoGainHistory] = {};
    size_t head_ = 0, count_ = 0;
};

}  // namespace hw1::stt
