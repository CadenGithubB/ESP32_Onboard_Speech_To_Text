#include "citrinet_frontend.h"
#include "citrinet_constants.h"
#include "quartznet_constants.h"   // the 512-point FFT tables (bit_reverse, twiddle_*), shared with QuartzNet
#include <cmath>
#include <cstring>
namespace hw1::stt::citrinet {
namespace {
constexpr float kPreemphasis=0.97f, kLogGuard=0x1p-24f;
constexpr size_t kWindowOffset=(kFft-kWindow)/2;   // 56: torch.stft centers a shorter window in n_fft
constexpr int kReach=static_cast<int>(kWindowOffset)-static_cast<int>(kFft/2);   // -200: first sample of a frame
namespace fft=hw1::stt::constants;
namespace mel=hw1::stt::citrinet::constants;
static_assert(sizeof(mel::window)/sizeof(mel::window[0])==kWindow, "window length");
static_assert(sizeof(mel::mel_count)/sizeof(mel::mel_count[0])==kMelBins, "mel bins");

// Power spectrum of the real 512-sample frame in w.real (as quartznet_frontend.cpp): one 256-point complex
// FFT plus the real-input split.
void real_power_spectrum(FrontendWorkspace& w) {
    for (size_t n=0;n<256;++n) w.imag[n]=w.real[2*n+1];
    for (size_t n=0;n<256;++n) w.real[n]=w.real[2*n];
    for (size_t i=0;i<256;++i) {
        const size_t j=fft::bit_reverse[i]>>1;
        if (j>i) {
            float t=w.real[i]; w.real[i]=w.real[j]; w.real[j]=t;
            t=w.imag[i]; w.imag[i]=w.imag[j]; w.imag[j]=t;
        }
    }
    for (size_t width=2;width<=256;width*=2) {
        const size_t half=width/2, step=512/width;
        for (size_t base=0;base<256;base+=width) for (size_t j=0;j<half;++j) {
            const size_t a=base+j,b=a+half,t=j*step;
            const float tr=fft::twiddle_real[t]*w.real[b]-fft::twiddle_imag[t]*w.imag[b];
            const float ti=fft::twiddle_real[t]*w.imag[b]+fft::twiddle_imag[t]*w.real[b];
            const float ar=w.real[a],ai=w.imag[a];
            w.real[a]=ar+tr; w.imag[a]=ai+ti;
            w.real[b]=ar-tr; w.imag[b]=ai-ti;
        }
    }
    for (size_t k=0;k<=256;++k) {
        const size_t a=k&255, b=(256-k)&255;
        const float zr=w.real[a], zi=w.imag[a], cr=w.real[b], ci=-w.imag[b];
        const float er=0.5f*(zr+cr), ei=0.5f*(zi+ci);
        const float or_=0.5f*(zi-ci), oi=-0.5f*(zr-cr);
        const float wr=k<256?fft::twiddle_real[k]:-1.0f, wi=k<256?fft::twiddle_imag[k]:0.0f;
        const float xr=er+wr*or_-wi*oi, xi=ei+wr*oi+wi*or_;
        w.power[k]=xr*xr+xi*xi;
    }
}
size_t reflect(int index, size_t samples) {
    if (index<0) return static_cast<size_t>(-index);
    if (static_cast<size_t>(index)>=samples) return 2*samples-2-static_cast<size_t>(index);
    return static_cast<size_t>(index);
}
void mel_bins(const FrontendWorkspace& w, float* out) {
    for (size_t m=0;m<kMelBins;++m) {
        float value=0;
        for (size_t j=0;j<mel::mel_count[m];++j)
            value+=mel::mel_weight[mel::mel_offset[m]+j]*w.power[mel::mel_start[m]+j];
        out[m]=std::log(value+kLogGuard);
    }
}
// One row from any sample source: exactly compute_features' loop body, s(i)=pcm[i]*(1/32768),
// conditioned(i)=i ? s(i)-0.97*s(i-1) : s(i), window, power, mel, log.
template<typename Source>
void mel_row(const Source& source,size_t samples,size_t frame,float* out,FrontendWorkspace& w) {
    const size_t center=frame*kHop;
    std::memset(w.real,0,sizeof(w.real)); std::memset(w.imag,0,sizeof(w.imag));
    for (size_t k=0;k<kWindow;++k) {
        const size_t input=reflect(static_cast<int>(center+k)+kReach,samples);
        const float sample=static_cast<float>(source(input))*(1.0f/32768.0f);
        float conditioned=sample;
        if (input) {
            const float previous=static_cast<float>(source(input-1))*(1.0f/32768.0f);
            conditioned=sample-kPreemphasis*previous;
        }
        w.real[kWindowOffset+k]=conditioned*mel::window[k];
    }
    real_power_spectrum(w);
    mel_bins(w,out);
}
}
size_t feature_frames(size_t samples) { return samples>=kMinSamples&&samples<=kMaxSamples?(samples+kHop-1)/kHop:0; }
Status compute_features(const int16_t* pcm,size_t samples,float* output,size_t capacity,
                        FrontendWorkspace* workspace,FrontendTiming* timing) {
    const auto now=[timing]() -> uint64_t { return timing&&timing->now_us?timing->now_us():0; };
    const size_t frames=feature_frames(samples);
    if (!pcm||!output||!workspace||!frames) return Status::InvalidArgument;
    if (capacity<frames*kMelBins) return Status::OutputTooSmall;
    auto& w=*workspace;
    size_t generated=0;
    float previous=0;
    for (size_t frame=0;frame<frames;++frame) {
        const uint64_t t0=now();
        const size_t center=frame*kHop;
        // Highest sample the frame reads: center+199, or 200 for the reflected start of frame 0.
        constexpr size_t back=static_cast<size_t>(-kReach);   // 200
        size_t required=center+(kWindow-1)-back;
        if (required<back) required=back;
        if (required>=samples) required=samples-1;
        while (generated<=required) {   // the ring holds 512 conditioned samples; a frame spans 400
            const float sample=static_cast<float>(pcm[generated])*(1.0f/32768.0f);
            w.conditioned[generated%512]=generated?sample-kPreemphasis*previous:sample;
            previous=sample; ++generated;
        }
        std::memset(w.real,0,sizeof(w.real)); std::memset(w.imag,0,sizeof(w.imag));
        for (size_t k=0;k<kWindow;++k) {
            const size_t input=reflect(static_cast<int>(center+k)+kReach,samples);
            w.real[kWindowOffset+k]=w.conditioned[input%512]*mel::window[k];
        }
        const uint64_t t1=now();
        real_power_spectrum(w);
        const uint64_t t2=now();
        mel_bins(w,output+frame*kMelBins);
        if (timing&&timing->now_us) {
            const uint64_t t3=now();
            timing->conditionUs+=uint32_t(t1-t0); timing->fftUs+=uint32_t(t2-t1); timing->melUs+=uint32_t(t3-t2);
        }
    }
    return normalize_features(output,frames,workspace,timing);
}
// As quartznet_frontend's: float, shifted by frame 0, Kahan-compensated sums, row-wise.
Status normalize_features(float* output,size_t frames,FrontendWorkspace* workspace,FrontendTiming* timing) {
    const auto now=[timing]() -> uint64_t { return timing&&timing->now_us?timing->now_us():0; };
    if (!output||!workspace||frames<2) return Status::InvalidArgument;
    auto& w=*workspace;
    const uint64_t normStart=now();
    float shift[kMelBins], sum[kMelBins]={}, carry[kMelBins]={};
    for (size_t m=0;m<kMelBins;++m) shift[m]=output[m];
    for (size_t t=0;t<frames;++t) {
        const float* row=output+t*kMelBins;
        for (size_t m=0;m<kMelBins;++m) {
            const float y=(row[m]-shift[m])-carry[m], s=sum[m]+y;
            carry[m]=(s-sum[m])-y; sum[m]=s;
        }
    }
    float mean[kMelBins], var[kMelBins]={};
    for (size_t m=0;m<kMelBins;++m) { mean[m]=sum[m]/static_cast<float>(frames); carry[m]=0; }
    for (size_t t=0;t<frames;++t) {
        const float* row=output+t*kMelBins;
        for (size_t m=0;m<kMelBins;++m) {
            const float d=(row[m]-shift[m])-mean[m], y=d*d-carry[m], s=var[m]+y;
            carry[m]=(s-var[m])-y; var[m]=s;
        }
    }
    float stddev[kMelBins];
    for (size_t m=0;m<kMelBins;++m) {
        stddev[m]=std::sqrt(var[m]/static_cast<float>(frames-1))+1e-5f;
        mean[m]+=shift[m];
        w.means[m]=mean[m]; w.variances[m]=stddev[m];
    }
    for (size_t t=0;t<frames;++t) {
        float* row=output+t*kMelBins;
        for (size_t m=0;m<kMelBins;++m) row[m]=(row[m]-mean[m])/stddev[m];
    }
    if (timing&&timing->now_us) timing->normUs+=uint32_t(now()-normStart);
    return Status::Ok;
}
Status log_mel_frame(const int16_t* pcm,size_t samples,size_t frame,float* out80,FrontendWorkspace* workspace) {
    if (!pcm||!out80||!workspace||frame>=feature_frames(samples)) return Status::InvalidArgument;
    mel_row([pcm](size_t i) { return pcm[i]; },samples,frame,out80,*workspace);
    return Status::Ok;
}
Status log_mel_frame_ring(const int16_t* ring,size_t ring_mask,uint64_t start,size_t samples,size_t frame,
                          float* out80,FrontendWorkspace* workspace) {
    if (!ring||!out80||!workspace||frame>=feature_frames(samples)||(ring_mask&(ring_mask+1))) return Status::InvalidArgument;
    mel_row([ring,ring_mask,start](size_t i) { return ring[static_cast<size_t>((start+i)&ring_mask)]; },
            samples,frame,out80,*workspace);
    return Status::Ok;
}
}
