// Replay a continuous raw microphone recording (s16le mono 16 kHz, e.g. an `sttraw` raw_<id>.pcm)
// through THIS repository's speech segmenter (firmware/components/hw1_stt/stt/stt_segmenter.h) with the
// standalone example's settings (firmware/examples/p4_eye_captions/main/main.cpp, the
// `cfg.segmentation.*` block), and print the phrases as JSON. No recognizer runs here.
//
//   segmenter_replay raw.pcm [--sidecar raw.json] [--set FIELD=VALUE]... [--block N] [--inner N]
//                    [--tail N] [--trace frames.csv] [--cut-dir DIR]
//   segmenter_replay --print-config [--set FIELD=VALUE]...   (settings in effect and the struct defaults)
//   segmenter_replay --list-fields
//
// --set       any SegmenterConfig field by its C++ name, e.g. --set adaptive_speech_floor=24.
//             Values are plain integers (bool: 0/1/true/false); "600ms" and "2s" are converted at 16 kHz.
// --block N   samples per microphone read (default 1024, main.cpp kReadSamples).
// --inner N   each block is pushed to the segmenter in pieces of at most N samples (default 160, as
//             StreamPipeline::push does, stt_stream.cpp); 0 pushes each block in one call.
// --tail N    trailing-silence trim the recognizer applies (StreamConfig::tail_samples). Default 8000,
//             the latency profile (CONFIG_CAPTIONS_TRIM_SILENCE=y); 0 = full audio (lean/default
//             profile). It only sets each segment's reported "decode_end"; the segmenter ignores it.
// --trace     one CSV row per 10 ms frame: sample,rms,noise,threshold,open,active,peak,ready; with
//             decision_pre_emphasis also dec_e,dec_noise_e,dec_thr_e,raw_e,raw_thr_e,gain_q16, and with
//             a noise control on (decision_modulation_ratio_pct, decision_pre_hold_requires_raw)
//             also modulated,raw_heard: whether the pre-emphasised level votes in that frame (1 on
//             every frame while the gate is off) and whether raw votes count (the hold's latch; 0
//             on every frame while the hold is off). (Feeds frame by frame; --block/--inner are ignored.)
// --cut-dir   writes each phrase's PCM as seg_<n>.pcm.
//
// The output has the keys of HardwareOne's stt_raw_replay (file, samples, seconds, sidecar_dropped,
// config, segments_covered_samples, segments[n,start,end,end_kind,continuation,voiced_end,new_voiced])
// so tools/sttraw_ab/score.py --replay <this binary> can consume it, plus segmenter_config (every
// field), block, inner, tail_samples, hard_limit_cuts and per segment samples, fresh_start, decode_end.
// HardwareOne's option spellings --speech-floor, --silence-floor, --noise-percentile, --blip-ms,
// --no-short-close and --chunk are accepted as aliases.
//
// Every delivered segment is compared with the file's samples at [start, end): a mismatch, or a
// segment that redelivers audio, exits with status 3.
#include "stt/stt_segmenter.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

using namespace hw1::stt;

namespace {

constexpr size_t kRate = 16000;
constexpr size_t kPhraseSamples = 10 * kRate;  // main.cpp kPhraseSamples
constexpr size_t kReadSamples = 1024;          // main.cpp kReadSamples
constexpr size_t kPipelinePiece = 160;         // StreamPipeline::push piece size
constexpr size_t kLatencyTail = kRate / 2;     // cfg.tail_samples under CONFIG_CAPTIONS_TRIM_SILENCE

// Every SegmenterConfig field, by name. A field added to the struct needs ONE new X(...) entry here;
// the static_assert below fails the build until it is listed.
#define SEGMENTER_CONFIG_FIELDS(X) \
  X(max_segment_samples)           \
  X(min_segment_samples)           \
  X(pre_roll_samples)              \
  X(end_silence_samples)           \
  X(min_voice_samples)             \
  X(forced_overlap_samples)        \
  X(rms_threshold)                 \
  X(freeze_noise_during_speech)    \
  X(adaptive)                      \
  X(calibration_samples)           \
  X(noise_window_frames)           \
  X(adaptive_speech_floor)         \
  X(adaptive_silence_floor)        \
  X(noise_percentile)              \
  X(silence_blip_samples)          \
  X(hard_cut_search_samples)       \
  X(short_close_silence_samples)   \
  X(decision_pre_emphasis)         \
  X(decision_gain_pct)             \
  X(decision_modulation_ratio_pct) \
  X(decision_pre_hold_requires_raw)

// Number of members of an aggregate, so a new SegmenterConfig field cannot be missed silently.
struct AnyField {
  template <class T>
  constexpr operator T() const noexcept;
};
template <class T, class Enable, class... A>
struct Arity {
  static constexpr size_t value = sizeof...(A);
};
template <class T, class... A>
struct Arity<T, std::void_t<decltype(T{A{}..., AnyField{}})>, A...> {
  static constexpr size_t value = Arity<T, void, A..., AnyField>::value;
};
#define COUNT_FIELD(name) +1
static_assert(Arity<SegmenterConfig, void>::value == (0 SEGMENTER_CONFIG_FIELDS(COUNT_FIELD)),
              "SegmenterConfig changed: add the new field to SEGMENTER_CONFIG_FIELDS");
#undef COUNT_FIELD

// The standalone example's segmenter settings: firmware/examples/p4_eye_captions/main/main.cpp,
// the `cfg.segmentation.*` assignments in app_main. tests/test_segmenter_replay.py compares this
// function with those source lines, so a change there must be repeated here.
SegmenterConfig exampleConfig() {
  SegmenterConfig c;
  c.adaptive = true;
  c.min_segment_samples = 4 * kRate;
  c.max_segment_samples = kPhraseSamples;
  c.short_close_silence_samples = kRate;
  c.noise_percentile = 25;
  c.silence_blip_samples = 3 * kSegmentAnalysisSamples;
  c.hard_cut_search_samples = 2 * kRate;
  c.decision_pre_emphasis = 97;  // CONFIG_CAPTIONS_DECISION_PRE_EMPHASIS (default y)
  c.decision_gain_pct = 150;
  // CONFIG_CAPTIONS_DECISION_MODULATION_GATE (default n) would add decision_modulation_ratio_pct = 300
  // and decision_pre_hold_requires_raw = true; arms set them with --set.
  return c;
}

[[noreturn]] void usage(const std::string& why) {
  std::fprintf(stderr, "segmenter_replay: %s\n", why.c_str());
  std::fprintf(stderr,
               "usage: segmenter_replay raw.pcm [--sidecar raw.json] [--set FIELD=VALUE]... [--block N] [--inner N]\n"
               "       [--tail N] [--trace frames.csv] [--cut-dir DIR]\n"
               "       segmenter_replay --print-config [--set FIELD=VALUE]...\n"
               "       segmenter_replay --list-fields\n");
  std::exit(2);
}

// Unsigned integer with an optional "ms" or "s" suffix (converted to samples at 16 kHz).
unsigned long long parseNumber(const std::string& text, unsigned long long hi, const std::string& name) {
  char* end = nullptr;
  errno = 0;
  if (text.empty() || text[0] == '-' || text[0] == '+') usage("bad value for " + name + ": " + text);
  unsigned long long v = std::strtoull(text.c_str(), &end, 10);
  if (errno || end == text.c_str()) usage("bad value for " + name + ": " + text);
  const std::string suffix = end;
  if (suffix == "ms") {
    if (v > std::numeric_limits<unsigned long long>::max() / (kRate / 1000)) usage("value too large for " + name);
    v *= kRate / 1000;
  } else if (suffix == "s") {
    if (v > std::numeric_limits<unsigned long long>::max() / kRate) usage("value too large for " + name);
    v *= kRate;
  } else if (!suffix.empty()) {
    usage("bad value for " + name + ": " + text);
  }
  if (v > hi) usage("value out of range for " + name + ": " + text);
  return v;
}

void assign(bool& field, const std::string& text, const std::string& name) {
  if (text == "1" || text == "true") field = true;
  else if (text == "0" || text == "false") field = false;
  else usage("bad value for " + name + " (0/1/true/false): " + text);
}
template <class T>
void assign(T& field, const std::string& text, const std::string& name) {
  static_assert(std::is_unsigned<T>::value, "add an assign() overload for this field type");
  field = static_cast<T>(parseNumber(text, std::numeric_limits<T>::max(), name));
}

bool setField(SegmenterConfig& c, const std::string& name, const std::string& value) {
#define SET_FIELD(field)         \
  if (name == #field) {          \
    assign(c.field, value, name); \
    return true;                 \
  }
  SEGMENTER_CONFIG_FIELDS(SET_FIELD)
#undef SET_FIELD
  return false;
}

std::string configJson(const SegmenterConfig& c) {
  std::string out = "{";
  bool first = true;
#define PRINT_FIELD(field)                                                      \
  out += std::string(first ? "" : ",") + "\"" #field "\":" +                    \
         std::to_string(static_cast<unsigned long long>(c.field));              \
  first = false;
  SEGMENTER_CONFIG_FIELDS(PRINT_FIELD)
#undef PRINT_FIELD
  return out + "}";
}

// The sidecar is small, flat JSON; only samples_dropped and rate matter here.
long sidecarField(const std::string& text, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const size_t at = text.find(needle);
  if (at == std::string::npos) return -1;
  return std::strtol(text.c_str() + at + needle.size(), nullptr, 10);
}

const char* endName(SegmentEnd end) {
  switch (end) {
    case SegmentEnd::Pause: return "pause";
    case SegmentEnd::HardLimit: return "hard_limit";
    case SegmentEnd::SessionEnd: return "session_end";
  }
  return "?";
}

std::string jsonString(const char* text) {
  std::string out = "\"";
  for (const char* p = text; *p; ++p) {
    if (*p == '"' || *p == '\\') out += '\\';
    out += *p;
  }
  return out + "\"";
}

}  // namespace

int main(int argc, char** argv) {
  const char* pcmPath = nullptr;
  const char* sidecarPath = nullptr;
  const char* tracePath = nullptr;
  const char* cutDir = nullptr;
  bool printConfig = false;
  size_t block = kReadSamples, inner = kPipelinePiece, tail = kLatencyTail;
  SegmenterConfig config = exampleConfig();
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&]() -> const char* {
      if (i + 1 >= argc) usage("missing value after " + a);
      return argv[++i];
    };
    if (a == "--set") {
      const std::string kv = value();
      const size_t eq = kv.find('=');
      if (eq == std::string::npos) usage("--set needs FIELD=VALUE");
      if (!setField(config, kv.substr(0, eq), kv.substr(eq + 1)))
        usage("unknown SegmenterConfig field " + kv.substr(0, eq) + " (see --list-fields)");
    } else if (a == "--sidecar") sidecarPath = value();
    else if (a == "--block" || a == "--chunk") block = size_t(parseNumber(value(), 1u << 20, a));
    else if (a == "--inner") inner = size_t(parseNumber(value(), 1u << 20, a));
    else if (a == "--tail") tail = size_t(parseNumber(value(), kSegmentMaximumSamples, a));
    else if (a == "--trace") tracePath = value();
    else if (a == "--cut-dir") cutDir = value();
    else if (a == "--print-config") printConfig = true;
    else if (a == "--list-fields") {
#define LIST_FIELD(field) std::printf("%s\n", #field);
      SEGMENTER_CONFIG_FIELDS(LIST_FIELD)
#undef LIST_FIELD
      return 0;
    }
    // HardwareOne stt_raw_replay spellings.
    else if (a == "--speech-floor") setField(config, "adaptive_speech_floor", value());
    else if (a == "--silence-floor") setField(config, "adaptive_silence_floor", value());
    else if (a == "--noise-percentile") setField(config, "noise_percentile", value());
    else if (a == "--blip-ms") setField(config, "silence_blip_samples", std::string(value()) + "ms");
    else if (a == "--no-short-close") config.short_close_silence_samples = 0;
    else if (a.rfind("--", 0) == 0) usage("unknown option " + a);
    else if (!pcmPath) pcmPath = argv[i];
    else usage("more than one input file");
  }
  if (block == 0) usage("--block must be at least 1");

  std::vector<int16_t> work(config.max_segment_samples ? config.max_segment_samples : 1);
  Segmenter seg;
  if (!seg.reset(work.data(), work.size(), config))
    usage("the segmenter rejected these settings: " + configJson(config));
  // StreamPipeline::begin refuses a tail that is not shorter than the end silence (stt_stream.cpp).
  if (tail >= config.end_silence_samples) usage("--tail must be below end_silence_samples");
  if (printConfig) {
    std::printf("{\"segmenter_config\":%s,\"segmenter_defaults\":%s,\"block\":%zu,\"inner\":%zu,\"tail_samples\":%zu}\n",
                configJson(config).c_str(), configJson(SegmenterConfig{}).c_str(), block, inner, tail);
    return 0;
  }
  if (!pcmPath) usage("missing raw.pcm");

  long dropped = 0;
  if (sidecarPath) {
    std::ifstream in(sidecarPath);
    if (!in) usage("cannot read the sidecar");
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    dropped = sidecarField(text, "samples_dropped");
    const long rate = sidecarField(text, "rate");
    if (rate != long(kRate)) usage("sidecar rate is not 16000");
    if (dropped < 0) usage("sidecar has no samples_dropped");
  }

  std::ifstream in(pcmPath, std::ios::binary);
  if (!in) usage("cannot read raw.pcm");
  std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.size() % 2) usage("raw.pcm has an odd byte count");
  std::vector<int16_t> pcm(bytes.size() / 2);
  for (size_t i = 0; i < pcm.size(); ++i)  // explicit little-endian decode
    pcm[i] = int16_t(uint16_t(uint8_t(bytes[2 * i])) | uint16_t(uint8_t(bytes[2 * i + 1])) << 8);
  bytes.clear();
  bytes.shrink_to_fit();

  const bool noiseControls =
      config.decision_pre_emphasis && (config.decision_modulation_ratio_pct || config.decision_pre_hold_requires_raw);
  std::FILE* trace = nullptr;
  if (tracePath) {
    trace = std::fopen(tracePath, "w");
    if (!trace) usage("cannot write the trace");
    // The energy columns exist only with the pre-emphasis decision on, so an option-0 trace is
    // byte-identical to the one written before the option existed.
    // Likewise the noise-control columns exist only with a control on.
    std::fputs(config.decision_pre_emphasis
                   ? "sample,rms,noise,threshold,open,active,peak,ready,dec_e,dec_noise_e,dec_thr_e,raw_e,raw_thr_e,gain_q16"
                   : "sample,rms,noise,threshold,open,active,peak,ready",
               trace);
    std::fputs(noiseControls ? ",modulated,raw_heard\n" : "\n", trace);
    block = inner = kSegmentAnalysisSamples;  // one row per analysis frame
  }

  std::string out;
  char line[512];
  size_t count = 0, hardLimits = 0;
  uint64_t covered = 0, previousEnd = 0;
  auto emit = [&]() {
    const SpeechSegment* s = seg.segment();
    ++count;
    covered += s->samples;
    if (s->end == SegmentEnd::HardLimit) ++hardLimits;
    // What the recognizer must receive: the file's own samples, each fresh sample once.
    if (s->end_sample > pcm.size() || s->end_sample - s->start_sample != s->samples ||
        std::memcmp(s->pcm, pcm.data() + s->start_sample, s->samples * sizeof(int16_t)) != 0) {
      std::fprintf(stderr, "segmenter_replay: segment %zu does not equal the input at [%llu, %llu)\n", count,
                   (unsigned long long)s->start_sample, (unsigned long long)s->end_sample);
      std::exit(3);
    }
    if (s->fresh_start_sample < previousEnd) {
      std::fprintf(stderr, "segmenter_replay: segment %zu redelivers audio before %llu\n", count,
                   (unsigned long long)previousEnd);
      std::exit(3);
    }
    previousEnd = s->end_sample;
    // StreamPipeline::inputSamples (stt_stream.cpp): the recognizer's input length.
    size_t decode = s->samples;
    if (tail && s->end != SegmentEnd::HardLimit && s->voiced_end_samples)
      decode = std::min(decode, s->voiced_end_samples + tail);
    if (decode < 320) decode = 320;
    std::snprintf(line, sizeof(line),
                  "%s{\"n\":%zu,\"start\":%llu,\"end\":%llu,\"end_kind\":\"%s\",\"continuation\":%s,"
                  "\"voiced_end\":%llu,\"new_voiced\":%zu,\"samples\":%zu,\"fresh_start\":%llu,\"decode_end\":%llu}",
                  count > 1 ? ",\n  " : "\n  ", count, (unsigned long long)s->start_sample,
                  (unsigned long long)s->end_sample, endName(s->end), s->continuation ? "true" : "false",
                  (unsigned long long)(s->start_sample + s->voiced_end_samples), s->new_voiced_samples, s->samples,
                  (unsigned long long)s->fresh_start_sample, (unsigned long long)(s->start_sample + decode));
    out += line;
    if (cutDir) {
      const std::string path = std::string(cutDir) + "/seg_" + std::to_string(count) + ".pcm";
      std::FILE* f = std::fopen(path.c_str(), "wb");
      if (!f) usage("cannot write into --cut-dir");
      for (size_t i = 0; i < s->samples; ++i) {
        const uint16_t v = uint16_t(s->pcm[i]);
        std::fputc(v & 0xff, f);
        std::fputc(v >> 8, f);
      }
      std::fclose(f);
    }
    seg.release();
  };

  size_t at = 0;
  while (at < pcm.size()) {
    size_t blockLeft = std::min(block, pcm.size() - at);
    while (blockLeft) {
      size_t left = inner ? std::min(inner, blockLeft) : blockLeft;
      blockLeft -= left;
      while (left) {
        const SegmentPushResult r = seg.push(pcm.data() + at, left);
        at += r.consumed;
        left -= r.consumed;
        if (r.status != SegmentStatus::Ready && (r.status != SegmentStatus::Ok || !r.consumed))
          usage("the segmenter rejected the input");
        if (trace && at % kSegmentAnalysisSamples == 0) {
          const int16_t* openPcm = nullptr;
          size_t openSamples = 0;
          uint64_t liveStart = 0;
          const bool ready = r.status == SegmentStatus::Ready;
          std::fprintf(trace, "%zu,%u,%u,%u,%d,%d,%u,%d", at, seg.current_rms(), seg.noise_rms(), seg.threshold_rms(),
                       seg.in_progress(&openPcm, &openSamples) ? 1 : 0,
                       (ready || seg.live_view(&liveStart, &openSamples)) ? 1 : 0, seg.peak_rms(), ready ? 1 : 0);
          if (config.decision_pre_emphasis)
            std::fprintf(trace, ",%llu,%llu,%llu,%llu,%llu,%u", (unsigned long long)seg.decision_energy(),
                         (unsigned long long)seg.decision_noise_energy(), (unsigned long long)seg.decision_threshold_energy(),
                         (unsigned long long)seg.raw_energy(), (unsigned long long)seg.raw_threshold_energy(),
                         (unsigned)seg.decision_gain_q16());
          if (noiseControls)
            std::fprintf(trace, ",%d,%d", seg.decision_modulated() ? 1 : 0, seg.decision_raw_heard() ? 1 : 0);
          std::fputc('\n', trace);
        }
        if (r.status == SegmentStatus::Ready) emit();
      }
    }
  }
  if (seg.finish() == SegmentStatus::Ready) emit();
  if (trace) std::fclose(trace);

  std::printf("{\"file\":%s,\"samples\":%zu,\"seconds\":%.2f,\"sidecar_dropped\":%ld,"
              "\"config\":{\"speech_floor\":%u,\"silence_floor\":%u,\"noise_percentile\":%u,"
              "\"blip_samples\":%zu,\"min_segment\":%zu,\"short_close\":%zu,\"end_silence\":%zu,"
              "\"pre_roll\":%zu,\"min_voice\":%zu,\"max_segment\":%zu},"
              "\"segmenter_config\":%s,\"block\":%zu,\"inner\":%zu,\"tail_samples\":%zu,"
              "\"hard_limit_cuts\":%zu,\"segments_covered_samples\":%llu,\"segments\":[%s\n]}\n",
              jsonString(pcmPath).c_str(), pcm.size(), double(pcm.size()) / kRate, dropped,
              config.adaptive_speech_floor, config.adaptive_silence_floor, unsigned(config.noise_percentile),
              config.silence_blip_samples, config.min_segment_samples, config.short_close_silence_samples,
              config.end_silence_samples, config.pre_roll_samples, config.min_voice_samples,
              config.max_segment_samples, configJson(config).c_str(), block, inner, tail, hardLimits,
              (unsigned long long)covered, out.c_str());
  if (dropped > 0)
    std::fprintf(stderr, "warning: the device dropped %ld samples; offsets after the first gap do not match the "
                         "device's session clock\n", dropped);
  return 0;
}
