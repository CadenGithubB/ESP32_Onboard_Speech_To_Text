#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include "../Audio_VadPolicy.h"

namespace hw1::stt {
inline constexpr size_t kSegmentSampleRate=16000;
inline constexpr size_t kSegmentAnalysisSamples=160; // 10 ms, independent of push chunk size.
inline constexpr size_t kSegmentMaximumSamples=20*kSegmentSampleRate;
inline constexpr size_t kSegmentNoiseWindowFrames=500; // At most five seconds of 10 ms levels.
// Pre-emphasis decision (decision_pre_emphasis != 0): frame energies are 10 ms
// variances in (count/100)^2, i.e. 10^4 x count^2, for the raw signal and for
// d[n] = 100 x[n] - C x[n-1]. Exact integers; no square root takes part in a decision.
inline constexpr uint64_t kSegmentEnergyUnit=10000;
inline constexpr uint32_t kSegmentGainOne=65536;               // Q16 energy gain 1.0
inline constexpr uint32_t kSegmentGainMin=kSegmentGainOne/4;   // gain^2 >= 1/4
inline constexpr uint32_t kSegmentGainMax=kSegmentGainOne*4096u; // gain^2 <= 4096
// The ambient-matching gain compares the lower quartiles of the idle raw and
// pre-emphasised energies, independent of noise_percentile (whose default 0,
// the window minimum, would match two single quietest frames).
inline constexpr uint8_t kDecisionGainPercentile=25;
// A phrase held open for this many hard limits in a row with no raw frame at the
// raw onset rule (>= the speech floor and the raw stop level) is treated as a
// steady high-frequency source: both ambients are re-seeded from the window, as a
// pause would, so the phrase can end. None of the bench recordings reaches it.
inline constexpr uint8_t kDecisionReseedCuts=2;
// Modulation gate (decision_modulation_ratio_pct): 500 ms of pre-emphasised frame
// energies; the 10th and 90th percentiles are ranks 5 and 45 of the 50.
inline constexpr uint8_t kDecisionModulationFrames=50;

// Simple fixed/adaptive energy endpointing, not a neural voice-activity detector. All
// durations are samples and must be multiples of the 10 ms analysis frame.
struct SegmenterConfig {
    size_t max_segment_samples=kSegmentMaximumSamples;
    size_t min_segment_samples=8*kSegmentSampleRate;
    size_t pre_roll_samples=4800;
    size_t end_silence_samples=9600;
    size_t min_voice_samples=3200;
    size_t forced_overlap_samples=0;
    uint16_t rms_threshold=400; // Fixed mode, DC-removed RMS in native int16 PCM units.
    bool freeze_noise_during_speech=false; // opt-in: freeze floor and history while active
    bool adaptive=false; // Opt in per consumer; existing fixed-mode behavior is unchanged.
    size_t calibration_samples=8000; // Initial 500 ms is ambient only, never admitted as speech.
    size_t noise_window_frames=kSegmentNoiseWindowFrames;
    // Raw-RMS defaults separate measured P4 room noise (3..31) from speech.
    // Callers may tune these for other microphones or acoustic conditions.
    uint16_t adaptive_speech_floor=45;
    uint16_t adaptive_silence_floor=16;
    // Opt-in for microphones with a jittery noise floor (e.g. the G2 glasses
    // mic, whose quiet frames swing ~21..60+ RMS). 0 keeps the minimum of the
    // window; 25 uses its lower quartile, which tracks the real background
    // instead of its quietest 10 ms frame.
    uint8_t noise_percentile=0;
    // Opt-in: louder runs up to this long do not restart the end-silence
    // count (a click or codec blip is not speech). 0 keeps the strict rule.
    size_t silence_blip_samples=0;
    // Opt-in: when a segment reaches max_segment_samples without a pause, cut
    // in the middle of the quietest 10 ms frame within its last N samples
    // (usually a gap between words) instead of at the exact limit. Samples
    // after the cut start the continuation as fresh audio (not overlap), so
    // every sample is still delivered exactly once. 0 cuts at the limit.
    // Requires forced_overlap_samples == 0.
    size_t hard_cut_search_samples=0;
    // Opt-in: a phrase still shorter than min_segment_samples also closes
    // once its pause reaches this many samples (>= end_silence_samples), so a
    // short reply is not held back by the minimum length. 0 keeps the strict
    // minimum length.
    size_t short_close_silence_samples=0;
    // Opt-in, adaptive mode only: level-robust onset. 0 (default) keeps the raw
    // RMS decision exactly. C in 1..100 adds a second level, the energy of the
    // pre-emphasised signal d[n] = x[n] - (C/100) x[n-1], and a frame counts as
    // onset/voiced when EITHER the raw energy or the pre-emphasised energy passes
    // its rule (silent only when both are below their stop level). The fixed
    // floors keep their raw-count meaning in the pre-emphasised domain through
    // an ambient-matching gain, the ratio of the lower quartiles
    // (kDecisionGainPercentile) of the raw and pre-emphasised energies over the
    // last idle frames, re-learned on every calibration and idle frame, never
    // while a phrase is open. Every comparison is exact 64-bit integer energy.
    // Raw PCM delivered to consumers is never changed.
    uint8_t decision_pre_emphasis=0;
    // Percent applied to the learned ambient energy gain (25..400). 100 maps the
    // floors at the ambient percentile itself; 150 reproduces the sensitivity of
    // the HardwareOne 8 Nov judge on the bench recordings. Ignored when off.
    uint16_t decision_gain_pct=100;
    // Opt-in noise controls for the level-robust onset (ignored while
    // decision_pre_emphasis is 0; with both at 0 the option behaves exactly as
    // without them).
    // Modulation gate: 0 = off; R in 101..1000 lets the pre-emphasised level
    // vote (onset, voiced, not silent, quiet-cut carry) only while its last
    // 500 ms is modulated, i.e. p90 > (R/100) x p10 of the frame RMS levels,
    // compared exactly as energies: p90 x 10^4 > R^2 x max(p10, 1). Steady
    // fans and hiss then leave the raw rule in charge (day-room babble only at
    // ratio 300; night babble not at all). 300 (3.0, 9.5 dB) is the reference
    // design's value. The window fills during
    // calibration; until 50 frames are seen the pre path does not vote.
    uint16_t decision_modulation_ratio_pct=0;
    // Pre hold: a phrase only the pre-emphasised level opened is kept open by
    // that level alone until the raw level reaches the raw onset rule by itself
    // (>= the speech floor and the raw stop level); until then the raw level's
    // voiced/not-silent votes and its quiet-cut carry are ignored. Without it,
    // steady noise the raw floor never admits can be held open by raw energy
    // merely above 4 x the raw ambient. The latch carries into the continuation
    // after a hard limit only when the released part itself reached the raw
    // rule (raw evidence in the current or the previous continuation), so a
    // talker who reached the raw floor and then gives way to steady noise keeps
    // raw votes for at most one more continuation. Reset when a phrase ends.
    bool decision_pre_hold_requires_raw=false;
};
enum class SegmentEnd : uint8_t { Pause, HardLimit, SessionEnd };
enum class SegmentStatus : uint8_t { Ok, Ready, Finished, InvalidArgument, NotInitialized, CounterOverflow };
struct SegmentPushResult { size_t consumed; SegmentStatus status; };
struct SpeechSegment {
    const int16_t* pcm=nullptr;
    size_t samples=0;
    uint64_t start_sample=0, end_sample=0; // Absolute session offsets, end exclusive.
    uint64_t fresh_start_sample=0; // Samples before this offset are explicit overlap.
    size_t overlap_before_samples=0;
    size_t next_overlap_samples=0; // Planned context if a hard-cut continuation is emitted.
    size_t new_voiced_samples=0; // Classified voiced samples, excluding retained overlap.
    SegmentEnd end=SegmentEnd::Pause;
    bool continuation=false;
    // Offset from start_sample of the end of the last ambient-voiced run
    // (level >= trimCut = max(silence floor, 2 x noise floor), the gate that
    // keeps quiet word tails) longer than silence_blip_samples, so 30 ms
    // clicks do not count. 0: no such run in this segment. Reported only; the
    // segment's samples are unchanged.
    size_t voiced_end_samples=0;
};

// Single-owner, no heap or platform dependencies. Buffer must remain valid and
// have at least config.max_segment_samples elements. Input must not alias it.
// push stops at one Ready result; only `consumed` samples belong to this call.
// Copy/queue segment()->pcm before release(), then push the unconsumed suffix.
// A ready segment is immutable until release/reset. Idle/noise gaps are skipped
// deliberately; absolute offsets identify them. Raw PCM is never filtered.
class Segmenter {
public:
    bool reset(int16_t* buffer,size_t capacity,const SegmenterConfig& config=SegmenterConfig{}) {
        if(!buffer || !valid(config) || capacity<config.max_segment_samples)return false;
        buffer_=buffer;config_=config;initialized_=true;reset_session();return true;
    }
    void reset_session() {
        seen_=start_=fresh_start_=0;used_=head_=history_=voice_=silence_=0;loud_run_=0;
        cut_=carry_voice_=0;
        voiced_run_=0;voiced_end_abs_=0;
        frame_count_=0;frame_sum_=0;frame_square_=0;
        active_=continuation_=ready_=finishing_=finished_=false;segment_={};
        calibrated_=!config_.adaptive;heard_speech_=false;calibration_seen_=0;
        noise_head_=noise_count_=0;current_rms_=noise_rms_=peak_rms_=0;
        threshold_rms_=config_.adaptive?config_.adaptive_speech_floor:config_.rms_threshold;
        std::memset(noise_window_,0,sizeof(noise_window_));
        pre_prev_=pre_sum_=0;pre_square_=0;
        dec_e_=raw_e_=amb_d_=amb_r_=peak_d_=peak_r_=0;
        std::memset(raw_window_,0,sizeof(raw_window_));
        std::memset(idle_d_,0,sizeof(idle_d_));std::memset(idle_r_,0,sizeof(idle_r_));idle_head_=idle_count_=0;
        set_gain(kSegmentGainOne);
        raw_onset_=false;dec_only_cuts_=0;
        std::memset(mod_ring_,0,sizeof(mod_ring_));mod_head_=mod_count_=0;modulated_=raw_heard_=false;
        thr_d_=thr_r_=0;
        if(config_.decision_pre_emphasis) { thr_d_=onset_d_;thr_r_=raw_floor(config_.adaptive_speech_floor); }
    }
    uint64_t samples_seen() const { return seen_; }
    bool calibrated() const { return initialized_ && calibrated_; }
    uint16_t current_rms() const { return current_rms_; }
    uint16_t noise_rms() const { return noise_rms_; }
    uint16_t threshold_rms() const { return threshold_rms_; }
    uint16_t peak_rms() const { return peak_rms_; }
    // Pre-emphasis mode only (zero otherwise; gain 1.0 = 65536): the last frame's
    // energies and the current ambient, threshold and gain, in kSegmentEnergyUnit
    // units / Q16. Diagnostics: no decision reads them.
    uint64_t decision_energy() const { return dec_e_; }
    uint64_t decision_noise_energy() const { return amb_d_; }
    uint64_t decision_threshold_energy() const { return thr_d_; }
    uint64_t raw_energy() const { return raw_e_; }
    uint64_t raw_threshold_energy() const { return thr_r_; }
    uint32_t decision_gain_q16() const { return gain_q16_; }
    // Noise controls (false while decision_pre_emphasis is 0): whether the
    // pre-emphasised level may vote in the current frame (the modulation gate's
    // state; true on every frame while the gate is off), and whether the raw
    // level's votes count (its latch under the pre hold; see
    // decision_pre_hold_requires_raw; false while the hold is off).
    bool decision_modulated() const { return config_.decision_pre_emphasis && pre_votes(); }
    bool decision_raw_heard() const { return raw_heard_; }
    const SpeechSegment* segment() const { return ready_?&segment_:nullptr; }
    // Live view of the utterance still being captured (pre-roll included),
    // for draft transcription. Valid only on the pushing thread, until the
    // next push/release/reset. False while idle or before enough voice.
    bool in_progress(const int16_t** pcm,size_t* samples) const {
        if(!initialized_||!active_||ready_||!eligible()||!used_)return false;
        *pcm=buffer_;*samples=used_;return true;
    }
    // The open phrase's absolute start and length, from its first sample
    // (before it qualifies as eligible), so features can be computed while it
    // is captured. Pushing thread only.
    bool live_view(uint64_t* start,size_t* samples) const {
        if(!initialized_||!active_||ready_)return false;
        *start=start_;*samples=used_;return true;
    }
    // The in-progress phrase's final input if the current pause goes on to
    // close it. True once the phrase's voiced end (voiced_end_samples) plus
    // `tail` samples has been captured and that pause would close it as an
    // ordinary Pause below the hard limit: eligible, and at
    // end_silence_samples the minimum length reached by then or, with
    // short_close_silence_samples, at the short close or on reaching the
    // minimum, whichever comes first.
    // start/voiced_end/samples describe exactly the prefix the broker's
    // trailing-silence trim would infer. Same thread and lifetime rules as
    // in_progress().
    bool speculation(size_t tail,uint64_t* start,size_t* voiced_end_out,size_t* samples) const {
        if(!initialized_||!active_||ready_||finishing_||finished_||!eligible()||!silence_)return false;
        const size_t ve=voiced_end(used_);
        if(!ve || ve+tail>used_)return false;
        const size_t remaining=silence_<config_.end_silence_samples?config_.end_silence_samples-silence_:0;
        // Below the minimum length a continuing pause still closes the phrase
        // as a Pause, at short_close_silence_samples or on reaching the
        // minimum, whichever comes first (the evaluate_frame rule).
        size_t need=remaining;
        if(used_+remaining<config_.min_segment_samples) {
            if(!config_.short_close_silence_samples)return false;
            const size_t to_min=config_.min_segment_samples-used_;
            const size_t to_short=silence_<config_.short_close_silence_samples?config_.short_close_silence_samples-silence_:0;
            need=std::max(remaining,std::min(to_min,to_short));
        }
        if(used_+need>config_.max_segment_samples)return false;
        *start=start_;*voiced_end_out=ve;*samples=ve+tail;return true;
    }
    SegmentPushResult push(const int16_t* pcm,size_t count) {
        if(!initialized_)return {0,SegmentStatus::NotInitialized};
        if(ready_)return {0,SegmentStatus::Ready};
        if(finished_ || finishing_)return {0,SegmentStatus::Finished};
        if(!pcm && count)return {0,SegmentStatus::InvalidArgument};
        if(count>std::numeric_limits<uint64_t>::max()-seen_)return {0,SegmentStatus::CounterOverflow};
        size_t consumed=0;
        while(consumed<count) {
            const int16_t sample=pcm[consumed++];
            // Calibration advances absolute positions but cannot become pre-roll
            // or model input, even if it contains loud audio or ends mid-push.
            if(calibrated_) {
                if(active_)buffer_[used_++]=sample;
                else append_history(sample);
            }
            if(config_.decision_pre_emphasis) {
                // |d| <= 200 * 32768 < 2^23; a frame's sum < 2^31, its square sum < 2^53.
                const int32_t d=100*int32_t(sample)-int32_t(config_.decision_pre_emphasis)*pre_prev_;
                pre_prev_=sample;pre_sum_+=d;pre_square_+=static_cast<uint64_t>(static_cast<int64_t>(d)*d);
            }
            ++seen_;++frame_count_;frame_sum_+=sample;
            frame_square_+=static_cast<uint64_t>(static_cast<int64_t>(sample)*sample);
            if(frame_count_==kSegmentAnalysisSamples) {
                evaluate_frame(false);clear_frame();
                if(ready_)return {consumed,SegmentStatus::Ready};
            }
        }
        return {consumed,SegmentStatus::Ok};
    }
    // End the session. Flush qualified voiced tails even below the minimum
    // wall span; no empty/silence-only continuation is emitted. Calling finish
    // while a segment is ready keeps that view and seals the session on release.
    SegmentStatus finish() {
        if(!initialized_)return SegmentStatus::NotInitialized;
        finishing_=true;
        if(ready_) {
            // A pending quiet-point cut would leave its carry behind when the
            // session seals on release: deliver the whole buffer instead.
            if(cut_) { segment_.samples=used_;segment_.end_sample=seen_;cut_=carry_voice_=0;segment_.voiced_end_samples=voiced_end(used_); }
            return SegmentStatus::Ready;
        }
        if(finished_)return SegmentStatus::Finished;
        if(frame_count_) { evaluate_frame(true);clear_frame(); }
        if(active_ && eligible()) { publish(SegmentEnd::SessionEnd);return SegmentStatus::Ready; }
        finished_=true;active_=false;return SegmentStatus::Finished;
    }
    bool release() {
        if(!ready_)return false;
        ready_=false;
        if(finishing_) { active_=false;finished_=true;return true; }
        if(segment_.end==SegmentEnd::HardLimit && cut_) {
            // Quiet-point cut: the samples after the cut were never delivered,
            // so they open the continuation as fresh audio.
            const size_t keep=used_-cut_;
            if(keep)std::memmove(buffer_,buffer_+cut_,keep*sizeof(int16_t));
            used_=keep;start_=seen_-keep;fresh_start_=start_;
            voice_=carry_voice_;silence_=0;active_=true;continuation_=true;
            cut_=carry_voice_=0;
        } else if(segment_.end==SegmentEnd::HardLimit) {
            const size_t keep=config_.forced_overlap_samples;
            if(keep)std::memmove(buffer_,buffer_+used_-keep,keep*sizeof(int16_t));
            used_=keep;start_=seen_-keep;fresh_start_=seen_;
            voice_=0;active_=true;continuation_=true;
        } else {
            // Already-delivered trailing silence must not become a new segment's
            // pre-roll: the default zero-overlap mode is exactly nonoverlapping.
            used_=head_=history_=voice_=silence_=0;loud_run_=0;active_=continuation_=false;
            voiced_run_=0;voiced_end_abs_=0;
            reset_utterance_levels();
        }
        if(config_.decision_pre_emphasis && segment_.end==SegmentEnd::HardLimit)after_hard_cut();
        segment_={};return true;
    }
private:
    static bool aligned(size_t n) { return n%kSegmentAnalysisSamples==0; }
    static bool valid(const SegmenterConfig& c) {
        return c.max_segment_samples>=2*kSegmentAnalysisSamples && c.max_segment_samples<=kSegmentMaximumSamples
            && c.min_segment_samples>0 && c.min_segment_samples<=c.max_segment_samples
            && c.pre_roll_samples<=c.max_segment_samples-kSegmentAnalysisSamples
            && c.end_silence_samples>0 && c.end_silence_samples<=c.max_segment_samples
            && c.min_voice_samples>0 && c.min_voice_samples<=c.max_segment_samples-c.pre_roll_samples
            && c.forced_overlap_samples<c.max_segment_samples && c.rms_threshold>0 && c.rms_threshold<=32767
            && aligned(c.max_segment_samples) && aligned(c.min_segment_samples) && aligned(c.pre_roll_samples)
            && aligned(c.end_silence_samples) && aligned(c.min_voice_samples) && aligned(c.forced_overlap_samples)
            && (!c.adaptive || (c.noise_window_frames>0 && c.noise_window_frames<=kSegmentNoiseWindowFrames
                && c.calibration_samples>=kSegmentAnalysisSamples
                && c.calibration_samples<=kSegmentNoiseWindowFrames*kSegmentAnalysisSamples && aligned(c.calibration_samples)
                && c.adaptive_speech_floor>0 && c.adaptive_speech_floor<=32767
                && c.adaptive_silence_floor>0 && c.adaptive_silence_floor<=c.adaptive_speech_floor))
            && c.noise_percentile<=50 && aligned(c.silence_blip_samples)
            && c.silence_blip_samples<c.end_silence_samples
            && aligned(c.hard_cut_search_samples) && c.hard_cut_search_samples<=c.max_segment_samples/2
            && (!c.hard_cut_search_samples || !c.forced_overlap_samples)
            && aligned(c.short_close_silence_samples)
            && (!c.short_close_silence_samples || (c.short_close_silence_samples>=c.end_silence_samples
                && c.short_close_silence_samples<=c.max_segment_samples))
            && c.decision_pre_emphasis<=100 && (!c.decision_pre_emphasis || c.adaptive)
            && c.decision_gain_pct>=25 && c.decision_gain_pct<=400
            && (!c.decision_modulation_ratio_pct || (c.decision_modulation_ratio_pct>100 && c.decision_modulation_ratio_pct<=1000));
    }
    size_t history_capacity() const { return config_.pre_roll_samples+kSegmentAnalysisSamples; }
    void append_history(int16_t sample) {
        const size_t capacity=history_capacity();
        if(history_==capacity) { buffer_[head_]=sample;head_=(head_+1)%capacity; }
        else { buffer_[(head_+history_)%capacity]=sample;++history_; }
    }
    void reverse(size_t begin,size_t end) {
        while(begin<end) { --end;if(begin>=end)break;int16_t v=buffer_[begin];buffer_[begin++]=buffer_[end];buffer_[end]=v; }
    }
    void begin_segment() {
        // A full ring contains pre-roll plus the triggering analysis frame.
        if(head_) { reverse(0,head_);reverse(head_,history_);reverse(0,history_); }
        used_=history_;start_=seen_-used_;fresh_start_=start_;
        head_=history_=voice_=silence_=0;loud_run_=0;active_=true;continuation_=false;
    }
    bool eligible() const { return voice_>0 && (continuation_ || voice_>=config_.min_voice_samples); }
    void clear_frame() { frame_count_=0;frame_sum_=0;frame_square_=0;pre_sum_=0;pre_square_=0; }
    void evaluate_frame(bool finalizing) {
        const uint64_t n=frame_count_;
        const uint64_t centered=frame_square_*n-static_cast<uint64_t>(frame_sum_*frame_sum_);
        // Integer floor(sqrt(variance)) is reproducible across hosts and chips;
        // the fixed-mode comparison below retains its original exact arithmetic.
        current_rms_=integer_sqrt(static_cast<uint32_t>(centered/(n*n)));
        bool voiced=false, silent=false;
        if(config_.decision_pre_emphasis) {
            if(!decide_energy(centered,n,&voiced,&silent))return;
        } else if(config_.adaptive) {
            const int32_t floor_before=noise_count_?noise_rms_:-1;
            const uint16_t window_min=(config_.freeze_noise_during_speech && active_) ?
                noise_rms_ : track_noise(current_rms_);
            if(!calibrated_) {
                noise_rms_=window_min;calibration_seen_+=frame_count_;
                if(calibration_seen_>=config_.calibration_samples)calibrated_=true;
                threshold_rms_=onset_threshold();
                return;
            }
            // A sustained voice can fill the entire window. Do not learn it as
            // its own ambient floor after five seconds; retain the utterance's
            // seed while permitting quieter evidence to lower it. Natural
            // endpoints resume upward adaptation. A noise jump that resembles
            // speech is inherently ambiguous to this energy-only detector.
            if(!active_ || window_min<noise_rms_)noise_rms_=window_min;
            // No utterance exists while idle: an old ambient peak must not
            // prevent a quiet voice after the room/source becomes quieter.
            if(!active_ || current_rms_>peak_rms_)peak_rms_=current_rms_;
            const auto decision=hw1::audio::adaptiveVadDecision(current_rms_,floor_before,noise_rms_,peak_rms_,
                heard_speech_,{config_.adaptive_speech_floor,config_.adaptive_silence_floor});
            threshold_rms_=static_cast<uint16_t>(heard_speech_?decision.stopCut:
                (decision.stopCut>config_.adaptive_speech_floor?decision.stopCut:config_.adaptive_speech_floor));
            heard_speech_=decision.heardSpeech;
            // Peak-relative stop timing must never remove quiet word tails.
            // All admitted raw samples stay in the segment; floor-relative
            // evidence counts toward minimum voice duration independently.
            voiced=heard_speech_ && !decision.trimSilent;
            silent=decision.stopSilent;
        } else {
            if(current_rms_>peak_rms_)peak_rms_=current_rms_;
            const uint64_t threshold=uint64_t(config_.rms_threshold)*config_.rms_threshold*n*n;
            voiced=centered>=threshold;silent=!voiced;
        }
        if(!active_ && voiced)begin_segment();
        if(!active_)return;
        if(voiced)voice_+=frame_count_;
        // End of the last ambient-voiced run longer than the blip limit
        // (absolute samples, so it survives a quiet-cut continuation).
        if(voiced) { voiced_run_+=frame_count_; if(voiced_run_>config_.silence_blip_samples)voiced_end_abs_=seen_; }
        else voiced_run_=0;
        if(silent) { silence_+=frame_count_; loud_run_=0; }
        else {
            loud_run_+=frame_count_;
            if(loud_run_>config_.silence_blip_samples)silence_=0;
        }
        if(finalizing)return;
        // Below the minimum length a pause closes the phrase only once it
        // reaches short_close_silence_samples; at or above it, end_silence.
        const bool pause=silence_>=config_.end_silence_samples &&
            (used_>=config_.min_segment_samples ||
             (config_.short_close_silence_samples && silence_>=config_.short_close_silence_samples));
        if(pause || used_>=config_.max_segment_samples) {
            if(!eligible())discard_candidate();
            else if(pause || !config_.hard_cut_search_samples)publish(pause?SegmentEnd::Pause:SegmentEnd::HardLimit);
            else publish_quiet_cut();
        }
    }
    static uint16_t integer_sqrt(uint32_t value) {
        uint32_t result=0,bit=uint32_t(1)<<30;
        while(bit>value)bit>>=2;
        while(bit) {
            if(value>=result+bit) { value-=result+bit;result=(result>>1)+bit; }
            else result>>=1;
            bit>>=2;
        }
        return static_cast<uint16_t>(result);
    }
    uint16_t track_noise(uint16_t level) {
        noise_window_[noise_head_]=level;
        noise_head_=(noise_head_+1)%config_.noise_window_frames;
        if(noise_count_<config_.noise_window_frames)++noise_count_;
        return window_floor();
    }
    // Minimum (default) or a low percentile of the ambient window. Percentiles
    // use a 2-RMS-wide histogram (0..190, larger levels in the last bin): no
    // copy of the window and 192 bytes of stack on the real-time capture task.
    uint16_t window_floor() const {
        if(!noise_count_)return 0;
        if(!config_.noise_percentile) {
            uint16_t minimum=noise_window_[0];
            for(size_t i=1;i<noise_count_;++i)if(noise_window_[i]<minimum)minimum=noise_window_[i];
            return minimum;
        }
        constexpr size_t kBins=96, kWidth=2;
        uint16_t bins[kBins]{};
        for(size_t i=0;i<noise_count_;++i) {
            const size_t bin=noise_window_[i]/kWidth;
            ++bins[bin<kBins?bin:kBins-1];
        }
        const size_t rank=noise_count_*config_.noise_percentile/100;
        size_t seen=0;
        for(size_t b=0;b<kBins;++b) {
            seen+=bins[b];
            if(seen>rank)return static_cast<uint16_t>(b*kWidth);
        }
        return static_cast<uint16_t>((kBins-1)*kWidth);
    }
    uint16_t onset_threshold() const {
        uint16_t threshold=static_cast<uint16_t>(2*noise_rms_);
        if(threshold<config_.adaptive_speech_floor)threshold=config_.adaptive_speech_floor;
        return threshold;
    }
    void reset_utterance_levels() {
        heard_speech_=false;peak_rms_=0;
        if(config_.decision_pre_emphasis) {
            peak_d_=peak_r_=0;raw_onset_=false;dec_only_cuts_=0;raw_heard_=false;
            if(noise_count_) {
                amb_d_=percentile_energy(noise_window_,noise_count_,config_.noise_percentile);
                amb_r_=percentile_energy(raw_window_,noise_count_,config_.noise_percentile);
            }
            thr_d_=std::max(onset_d_,4*amb_d_);thr_r_=std::max(raw_floor(config_.adaptive_speech_floor),4*amb_r_);
            publish_levels();
            return;
        }
        if(config_.adaptive) {
            if(noise_count_) {
                noise_rms_=window_floor();
            }
            threshold_rms_=onset_threshold();
        }
    }
    void publish(SegmentEnd end) {
        segment_={buffer_,used_,start_,seen_,fresh_start_,static_cast<size_t>(fresh_start_-start_),
                  end==SegmentEnd::HardLimit?config_.forced_overlap_samples:0,voice_,end,continuation_};
        segment_.voiced_end_samples=voiced_end(used_);
        ready_=true;
    }
    // Voiced end as an offset into the current segment, clamped to its first
    // `samples` samples; 0 when the last qualifying run ended before it.
    size_t voiced_end(size_t samples) const {
        if(voiced_end_abs_<=start_)return 0;
        const uint64_t offset=voiced_end_abs_-start_;
        return offset<samples?static_cast<size_t>(offset):samples;
    }
    // Hard limit with hard_cut_search_samples: find the analysis frame with the
    // least DC-removed energy in the last N samples (latest wins ties: the
    // shorter carry) and cut at its START, so the phrase ends in the quiet and
    // the carry stays a whole number of frames. Cuts must stay on the stream's
    // frame grid: limits are only checked at frame ends, and a carry of a
    // fractional frame would let the next phrase overrun the buffer (found by
    // ASan, 2026-10-02). Frames after the cut above the current threshold count
    // as the carry's voice, so a word right after the cut is not dropped as
    // silence if speech stops. Once per hard limit: N/160 frames of integer math.
    void publish_quiet_cut() {
        constexpr size_t F=kSegmentAnalysisSamples;
        // Buffer index i is a frame start when (start_+i) is on the grid.
        const size_t phase=static_cast<size_t>((F-start_%F)%F);
        const size_t search=config_.hard_cut_search_samples;
        size_t first=used_>search ? used_-search : 0;
        if(first<phase)first=phase;
        first+=(F-(first-phase)%F)%F;
        size_t best=0;   // 0: no candidate, cut at the limit
        uint64_t best_energy=std::numeric_limits<uint64_t>::max();
        for(size_t at=first; at+F<=used_; at+=F) {
            if(!at)continue;  // never an empty phrase
            const uint64_t e=config_.decision_pre_emphasis?frame_energy_d(at):frame_energy(at);
            if(e<=best_energy) { best_energy=e; best=at; }
        }
        if(!best) { publish(SegmentEnd::HardLimit); return; }
        cut_=best;
        carry_voice_=0;
        if(config_.decision_pre_emphasis) {
            // Same units and thresholds as the streaming decision (defect (c)),
            // and the same noise-control votes (both true while those are off).
            const bool dec_votes=pre_votes(), raw_votes=raw_votes_count();
            for(size_t at=best; at+F<=used_; at+=F)
                if((dec_votes && frame_energy_d(at)/(F*F)>=thr_d_) ||
                   (raw_votes && frame_energy(at)*kSegmentEnergyUnit/(F*F)>=thr_r_))carry_voice_+=F;
        } else {
            const uint64_t limit=uint64_t(threshold_rms_)*threshold_rms_*F*F;
            for(size_t at=best; at+F<=used_; at+=F)
                if(frame_energy(at)>=limit)carry_voice_+=F;
        }
        segment_={buffer_,cut_,start_,start_+cut_,fresh_start_,static_cast<size_t>(fresh_start_-start_),
                  0,voice_,SegmentEnd::HardLimit,continuation_};
        segment_.voiced_end_samples=voiced_end(cut_);
        ready_=true;
    }
    // ---------------- pre-emphasis energy decision ----------------
    // Monotone 16-bit code of an energy < 2^47: exact below 2048, else 6-bit
    // exponent + 10 explicit mantissa bits (value() <= v < value()*(1+2^-10)).
    static uint16_t energy_code(uint64_t v) {
        if(v<1024)return static_cast<uint16_t>(v);
        unsigned p;   // msb index (10..46)
#if defined(__GNUC__) || defined(__clang__)
        p=63u-static_cast<unsigned>(__builtin_clzll(v));
#else
        p=0; for(uint64_t t=v>>1;t;t>>=1)++p;
#endif
        return static_cast<uint16_t>(((p-9)<<10)|((v>>(p-10))&1023));
    }
    static uint64_t energy_value(uint16_t c) {
        const unsigned e=c>>10, m=c&1023;
        return e?(uint64_t(1024|m)<<(e-1)):m;
    }
    // Exact order statistic (rank count*pct/100, as window_floor) of a code
    // window: two radix passes over the high and low byte, 512 bytes of stack.
    static uint64_t percentile_energy(const uint16_t* w,size_t count,unsigned pct) {
        const size_t rank=count*pct/100;
        uint16_t bins[256];
        std::memset(bins,0,sizeof(bins));
        for(size_t i=0;i<count;++i)++bins[w[i]>>8];
        size_t seen=0;unsigned hi=0;
        for(;hi<255;++hi) { if(seen+bins[hi]>rank)break; seen+=bins[hi]; }
        std::memset(bins,0,sizeof(bins));
        for(size_t i=0;i<count;++i)if((w[i]>>8)==hi)++bins[w[i]&255];
        unsigned lo=0;
        for(;lo<255;++lo) { if(seen+bins[lo]>rank)break; seen+=bins[lo]; }
        return energy_value(static_cast<uint16_t>((hi<<8)|lo));
    }
    static uint64_t raw_floor(uint64_t f) { return f*f*kSegmentEnergyUnit; }
    // A raw floor F counts in decision energy: F^2 * 10^4 / gain (< 2^60 before the divide).
    void set_gain(uint32_t g) {
        gain_q16_=g;
        onset_d_=(raw_floor(config_.adaptive_speech_floor)<<16)/g;
        voiced_d_=(raw_floor(config_.adaptive_silence_floor)<<16)/g;
    }
    // gain = raw ambient / decision ambient over the idle frames (defect (b): re-learned on every idle frame).
    void learn_gain() {
        if(!idle_count_)return;
        const uint64_t d=percentile_energy(idle_d_,idle_count_,kDecisionGainPercentile);
        // A zero decision quartile carries no evidence about the room: keep the previous
        // gain. (Mapping it to kSegmentGainMax let room tone open and hold phrases after
        // digital silence; constant frames are now kept out of the window, so this is a guard.)
        if(!d)return;
        const uint64_t r=percentile_energy(idle_r_,idle_count_,kDecisionGainPercentile);
        uint64_t g=(r<<16)/d;   // r < 2^44, so r<<16 < 2^60
        if(g>kSegmentGainMax)g=kSegmentGainMax;
        g=g*config_.decision_gain_pct/100;
        const uint32_t next=static_cast<uint32_t>(std::min<uint64_t>(std::max<uint64_t>(g,kSegmentGainMin),kSegmentGainMax));
        if(next!=gain_q16_)set_gain(next);
    }
    // After a hard-limit release (option on): see kDecisionReseedCuts. Keeps
    // heard_speech_ and the gain; the phrase continues and may now end on a pause.
    // A released part with no raw evidence also drops the pre hold's raw latch
    // (no-op while the hold is off: raw_heard_ is then always false).
    void after_hard_cut() {
        if(!raw_onset_)raw_heard_=false;
        if(raw_onset_)dec_only_cuts_=0;
        else if(++dec_only_cuts_>=kDecisionReseedCuts) {
            dec_only_cuts_=0;peak_d_=peak_r_=0;
            if(noise_count_) {
                amb_d_=percentile_energy(noise_window_,noise_count_,config_.noise_percentile);
                amb_r_=percentile_energy(raw_window_,noise_count_,config_.noise_percentile);
            }
            thr_d_=std::max(voiced_d_,4*amb_d_);thr_r_=std::max(raw_floor(config_.adaptive_silence_floor),4*amb_r_);
            publish_levels();
        }
        raw_onset_=false;
    }
    // Whether each level may vote in this frame (the noise controls; true while off).
    bool pre_votes() const { return !config_.decision_modulation_ratio_pct || modulated_; }
    bool raw_votes_count() const { return !config_.decision_pre_hold_requires_raw || raw_heard_; }
    // Modulation gate, every frame (calibration, idle and active alike): ring of
    // the last kDecisionModulationFrames pre-emphasised energy codes; modulated
    // when p90 x 10^4 > R^2 x max(p10, 1), R = decision_modulation_ratio_pct.
    // Order statistics by nth_element on a 100-byte stack copy; exact integer
    // compare (p90 < 2^47, so p90 x 10^4 < 2^61; R^2 <= 10^6).
    void track_modulation(uint16_t code) {
        mod_ring_[mod_head_]=code;
        mod_head_=static_cast<uint8_t>((mod_head_+1)%kDecisionModulationFrames);
        if(mod_count_<kDecisionModulationFrames)++mod_count_;
        if(mod_count_<kDecisionModulationFrames) { modulated_=false;return; }
        constexpr size_t kLo=kDecisionModulationFrames/10, kHi=kDecisionModulationFrames*9/10;
        uint16_t w[kDecisionModulationFrames];
        std::memcpy(w,mod_ring_,sizeof(w));
        std::nth_element(w,w+kHi,w+kDecisionModulationFrames);
        std::nth_element(w,w+kLo,w+kHi);  // every element before kHi is <= w[kHi]
        const uint64_t lo=std::max<uint64_t>(energy_value(w[kLo]),1), hi=energy_value(w[kHi]);
        const uint64_t r2=uint64_t(config_.decision_modulation_ratio_pct)*config_.decision_modulation_ratio_pct;
        // lo > hi x 10^4 / r2 (no overflow): then r2 x lo > hi x 10^4.
        modulated_=lo<=hi*10000/r2 && hi*10000>r2*lo;
    }
    // Diagnostics only (UI and trace): raw-count views of the energy state.
    static uint16_t energy_rms(uint64_t e) {
        const uint64_t v=e/kSegmentEnergyUnit;
        return integer_sqrt(static_cast<uint32_t>(std::min<uint64_t>(v,0xffffffffu)));
    }
    void publish_levels() {
        noise_rms_=energy_rms(amb_r_);peak_rms_=energy_rms(peak_r_);
        // Saturate before the multiply: min(.,2^35) x gain (< 2^29) stays below 2^64.
        const uint64_t d_scaled=std::min<uint64_t>(thr_d_/kSegmentEnergyUnit,uint64_t(1)<<35);
        const uint64_t d_as_raw=std::min<uint64_t>((d_scaled*gain_q16_)>>16,0xffffffffu);
        threshold_rms_=std::min(energy_rms(thr_r_),integer_sqrt(static_cast<uint32_t>(d_as_raw)));
    }
    bool decide_energy(uint64_t centered,uint64_t n,bool* voiced,bool* silent) {
        const uint64_t nn=n*n;
        dec_e_=(pre_square_*n-static_cast<uint64_t>(static_cast<int64_t>(pre_sum_)*pre_sum_))/nn; // < 2^46
        raw_e_=centered*kSegmentEnergyUnit/nn;                                                  // < 2^44
        const bool seeded=noise_count_>0;
        uint64_t wd=amb_d_, wr=amb_r_;
        const uint16_t code_d=energy_code(dec_e_), code_r=energy_code(raw_e_);
        if(config_.decision_modulation_ratio_pct)track_modulation(code_d);
        if(!(config_.freeze_noise_during_speech && active_)) {
            noise_window_[noise_head_]=code_d;raw_window_[noise_head_]=code_r;
            noise_head_=(noise_head_+1)%config_.noise_window_frames;
            if(noise_count_<config_.noise_window_frames)++noise_count_;
            wd=percentile_energy(noise_window_,noise_count_,config_.noise_percentile);
            wr=percentile_energy(raw_window_,noise_count_,config_.noise_percentile);
        }
        // Constant frames (digital silence, a muted input, DC) say nothing about the
        // room's spectrum and stay out of the gain windows.
        if((!calibrated_ || !active_) && raw_e_) {
            idle_d_[idle_head_]=code_d;idle_r_[idle_head_]=code_r;
            idle_head_=(idle_head_+1)%config_.noise_window_frames;
            if(idle_count_<config_.noise_window_frames)++idle_count_;
        }
        const uint64_t sp_r=raw_floor(config_.adaptive_speech_floor), si_r=raw_floor(config_.adaptive_silence_floor);
        if(!calibrated_) {
            amb_d_=wd;amb_r_=wr;learn_gain();
            calibration_seen_+=frame_count_;
            if(calibration_seen_>=config_.calibration_samples)calibrated_=true;
            thr_d_=std::max(onset_d_,4*amb_d_);thr_r_=std::max(sp_r,4*amb_r_);
            publish_levels();
            return false;
        }
        if(!active_ || wd<amb_d_)amb_d_=wd;
        if(!active_ || wr<amb_r_)amb_r_=wr;
        if(!active_)learn_gain();
        if(!active_ || dec_e_>peak_d_)peak_d_=dec_e_;
        if(!active_ || raw_e_>peak_r_)peak_r_=raw_e_;
        // adaptiveVadDecision in energy units: 2 x floor -> 4 x, peak/8 -> peak/64.
        const uint64_t trim_d=std::max(voiced_d_,4*amb_d_), stop_d=std::max(trim_d,peak_d_>>6);
        const uint64_t trim_r=std::max(si_r,4*amb_r_), stop_r=std::max(trim_r,peak_r_>>6);
        if(active_ && raw_e_>=std::max(stop_r,sp_r))raw_onset_=true;   // raw evidence in this phrase (kDecisionReseedCuts)
        // Noise controls; both votes are true while their options are off.
        if(config_.decision_pre_hold_requires_raw && seeded && raw_e_>=stop_r && raw_e_>=sp_r)raw_heard_=true;
        const bool dv=pre_votes(), rv=raw_votes_count();
        const bool latch=!heard_speech_ && seeded &&
            ((dv && dec_e_>=stop_d && dec_e_>=onset_d_) || (raw_e_>=stop_r && raw_e_>=sp_r));
        thr_d_=heard_speech_?stop_d:std::max(stop_d,onset_d_);
        thr_r_=heard_speech_?stop_r:std::max(stop_r,sp_r);
        heard_speech_=heard_speech_||latch;
        *voiced=heard_speech_ && ((dv && dec_e_>=trim_d) || (rv && raw_e_>=trim_r));
        *silent=heard_speech_ && (!dv || dec_e_<stop_d) && (!rv || raw_e_<stop_r);
        publish_levels();
        return true;
    }
    // n^2 * variance of d for one buffered frame (buffer_[at-1] precedes it, at >= 1):
    // exactly the value the stream computed for that frame.
    uint64_t frame_energy_d(size_t at) const {
        int64_t sum=0; uint64_t square=0;
        int32_t prev=buffer_[at-1];
        for(size_t i=0;i<kSegmentAnalysisSamples;++i) {
            const int32_t v=buffer_[at+i];
            const int32_t d=100*v-int32_t(config_.decision_pre_emphasis)*prev; prev=v;
            sum+=d; square+=static_cast<uint64_t>(static_cast<int64_t>(d)*d);
        }
        return square*kSegmentAnalysisSamples-static_cast<uint64_t>(sum*sum);
    }
    // n^2 * variance of one analysis frame (same scale as evaluate_frame).
    uint64_t frame_energy(size_t at) const {
        int64_t sum=0; uint64_t square=0;
        for(size_t i=0;i<kSegmentAnalysisSamples;++i) {
            const int32_t v=buffer_[at+i];
            sum+=v; square+=static_cast<uint64_t>(static_cast<int64_t>(v)*v);
        }
        return square*kSegmentAnalysisSamples-static_cast<uint64_t>(sum*sum);
    }
    void discard_candidate() {
        const size_t fresh=static_cast<size_t>(seen_-fresh_start_);
        size_t keep=used_<history_capacity()?used_:history_capacity();
        // A rejected forced continuation may contain retained, already-delivered
        // context. Only fresh samples may seed a subsequent ordinary pre-roll.
        if(keep>fresh)keep=fresh;
        if(keep)std::memmove(buffer_,buffer_+used_-keep,keep*sizeof(int16_t));
        history_=keep;head_=used_=voice_=silence_=0;loud_run_=0;active_=continuation_=false;
        voiced_run_=0;voiced_end_abs_=0;
        reset_utterance_levels();
    }
    int16_t* buffer_=nullptr;
    SegmenterConfig config_{};
    SpeechSegment segment_{};
    uint64_t seen_=0,start_=0,fresh_start_=0;
    size_t used_=0,head_=0,history_=0,voice_=0,silence_=0,loud_run_=0;
    size_t cut_=0,carry_voice_=0;  // pending quiet-point cut (publish_quiet_cut -> release)
    size_t voiced_run_=0;          // current ambient-voiced run (samples)
    uint64_t voiced_end_abs_=0;    // end of the last run longer than the blip limit
    int64_t frame_sum_=0;
    uint64_t frame_square_=0;
    uint16_t frame_count_=0;
    uint16_t noise_window_[kSegmentNoiseWindowFrames]{};
    size_t noise_head_=0,noise_count_=0,calibration_seen_=0;
    uint16_t current_rms_=0,noise_rms_=0,threshold_rms_=0,peak_rms_=0;
    bool calibrated_=false,heard_speech_=false;
    // Pre-emphasis mode (decision_pre_emphasis != 0); untouched otherwise.
    int32_t pre_prev_=0,pre_sum_=0;      // previous input sample (stream state), frame sum of d
    uint64_t pre_square_=0;              // frame sum of d^2
    uint64_t dec_e_=0,raw_e_=0;          // last frame's energies
    uint64_t amb_d_=0,amb_r_=0;          // ambient (noise_percentile of the window), may only fall while active
    uint64_t peak_d_=0,peak_r_=0;        // utterance peaks
    uint64_t onset_d_=0,voiced_d_=0;     // speech/silence floors in decision energy
    uint64_t thr_d_=0,thr_r_=0;          // current stop/onset threshold per domain (quiet-cut carry)
    uint32_t gain_q16_=kSegmentGainOne;  // raw/decision ambient energy ratio, Q16
    uint16_t raw_window_[kSegmentNoiseWindowFrames]{};  // noise_window_ holds decision codes
    uint16_t idle_d_[kSegmentNoiseWindowFrames]{},idle_r_[kSegmentNoiseWindowFrames]{};
    size_t idle_head_=0,idle_count_=0;
    bool raw_onset_=false;              // a raw frame reached the raw onset rule since the last release
    uint8_t dec_only_cuts_=0;           // consecutive hard limits without raw_onset_
    // Noise controls (decision_modulation_ratio_pct, decision_pre_hold_requires_raw); untouched while off.
    uint16_t mod_ring_[kDecisionModulationFrames]{};  // pre-emphasised energy codes, last 500 ms
    uint8_t mod_head_=0,mod_count_=0;
    bool modulated_=false;              // gate state for the current frame
    bool raw_heard_=false;              // raw level met the raw onset rule in this or the previous continuation
    bool initialized_=false,active_=false,continuation_=false,ready_=false,finishing_=false,finished_=false;
};
} // namespace hw1::stt
