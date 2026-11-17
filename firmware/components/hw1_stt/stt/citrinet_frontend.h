#pragma once
// Citrinet-384 front end: int16 PCM -> per-utterance normalized log-mel features [T, 80].
// The same pipeline as quartznet_frontend (pre-emphasis 0.97, centered reflect-padded 512-point FFT,
// hop 160, squared magnitude, sparse checkpoint mel bank, log(mel + 2^-24), per-bin CMVN with the
// unbiased variance and sqrt(var) + 1e-5), with Citrinet's 400-sample window (centered at offset 56 of
// the FFT frame) and 80-band mel bank from citrinet_constants.h. No dither: the device never adds it.
// Reference: tools/stt_train_15x5/common.py Frontend(state)(samples, dither=False).
// quartznet_frontend is unchanged; its Status, FrontendTiming and sample bounds are shared.
#include <cstddef>
#include <cstdint>
#include "quartznet_frontend.h"
namespace hw1::stt::citrinet {
inline constexpr size_t kMelBins=80, kWindow=400, kHop=160, kFft=512;
// All workspace is caller-owned (no heap, microphone or filesystem calls); one per thread at a time.
struct FrontendWorkspace {
    float conditioned[512], real[512], imag[512], power[257];
    double means[kMelBins], variances[kMelBins];
};
// ceil(samples / 160) for kMinSamples..kMaxSamples samples, else 0 (as QuartzNet's).
size_t feature_frames(size_t samples);
// Row-major float32 [frames, 80] into output (capacity in floats). Input, output and workspace must not overlap.
Status compute_features(const int16_t* pcm, size_t samples, float* output, size_t output_floats,
                        FrontendWorkspace* workspace, FrontendTiming* timing=nullptr);
// Row `frame` of compute_features before CMVN, alone, bit-identical (same float operations in the same
// order; this file keeps -ffp-contract=off). It reads samples [frame*160-201, frame*160+200), reflecting
// at the start and at `samples`; a row with frame*160+200 <= samples never reflects at the end.
Status log_mel_frame(const int16_t* pcm, size_t samples, size_t frame, float* out80, FrontendWorkspace* workspace);
// The same row reading segment sample i from ring[(start + i) & ring_mask] (ring_mask + 1 a power of two).
Status log_mel_frame_ring(const int16_t* ring, size_t ring_mask, uint64_t start, size_t samples, size_t frame,
                          float* out80, FrontendWorkspace* workspace);
// compute_features' per-utterance CMVN over finished rows, in place.
Status normalize_features(float* output, size_t frames, FrontendWorkspace* workspace, FrontendTiming* timing=nullptr);
}
