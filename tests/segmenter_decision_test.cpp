// Level-robust onset (SegmenterConfig::decision_pre_emphasis): unit cases U1-U16.
// Run by tests/test_segmenter_decision.py, which also builds mutants of the header
// that each revert one defect fix and checks that the named case fails on them.
//
//   segmenter_decision_test            all cases
//   segmenter_decision_test U3 U6      only those cases (exit 1 on any failure)
//
// Built with -DSEGMENTER_DIAG_HASH_ONLY it prints only the option-0 diagnostics hash
// (U1), so it can be compiled against the header from before the option existed.
// Built with -DSEGMENTER_PROD_HASH_ONLY it prints only the production (option on) hash
// (U13), so it can be compiled against the header from before the modulation gate existed.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>
#define private public  // U7/U8 call frame_energy_d, energy_code and percentile_energy
#include "stt/stt_segmenter.h"
#undef private
using namespace hw1::stt;
#if !defined(SEGMENTER_DIAG_HASH_ONLY) && !defined(SEGMENTER_PROD_HASH_ONLY)
#define SEGMENTER_HAS_GATE 1  // the header has the modulation gate fields (U13-U16)
#endif

namespace {
constexpr size_t F=kSegmentAnalysisSamples;

int16_t clip(double v) {
    const long q=std::lround(v);
    return int16_t(q>32767?32767:(q<-32768?-32768:q));
}

// ---- U1: option 0 diagnostics, comparable with the header before the option ----
uint64_t fnv(uint64_t h,uint64_t v) {
    for(int i=0;i<8;++i) { h^=(v>>(8*i))&255u; h*=1099511628211ull; }
    return h;
}
std::vector<int16_t> diag_signal() {
    // A quiet room, phrases, a click, 14 s of unbroken speech and a quiet talker.
    std::mt19937 rng(20261112); std::normal_distribution<double> n01(0,1);
    const std::pair<double,int> plan[]={{2.0,0},{3.0,300},{2.0,0},{0.02,900},{2.0,0},{1.2,200},{3.0,0},
                                        {14.0,250},{2.5,0},{5.0,40},{3.0,0}};
    std::vector<int16_t> pcm;
    for(auto [seconds,level]:plan) {
        const size_t n=size_t(seconds*16000);
        for(size_t i=0;i<n;++i) pcm.push_back(clip(n01(rng)*(level?level:5)+40));
    }
    for(int i=0;i<77;++i)pcm.push_back(int16_t(i%5));  // a session end off the frame grid
    return pcm;
}
std::vector<SegmenterConfig> diag_configs(unsigned gain_pct_when_off,bool gate_when_off=false) {
    std::vector<SegmenterConfig> out(5);
    out[1].adaptive=true;
    SegmenterConfig& e=out[2];  // the example's settings with the option off
    e.adaptive=true;e.min_segment_samples=64000;e.max_segment_samples=160000;e.short_close_silence_samples=16000;
    e.noise_percentile=25;e.silence_blip_samples=480;e.hard_cut_search_samples=32000;
    out[3]=e;out[3].freeze_noise_during_speech=true;
    out[4].hard_cut_search_samples=3200;out[4].max_segment_samples=16000;out[4].min_segment_samples=3200;
#ifndef SEGMENTER_DIAG_HASH_ONLY
    for(auto& c:out)c.decision_gain_pct=uint16_t(gain_pct_when_off);  // ignored while off
#endif
#ifdef SEGMENTER_HAS_GATE
    for(auto& c:out)if(gate_when_off) { c.decision_modulation_ratio_pct=300; c.decision_pre_hold_requires_raw=true; }
#else
    (void)gate_when_off;
#endif
#ifdef SEGMENTER_DIAG_HASH_ONLY
    (void)gain_pct_when_off;
#endif
    return out;
}
uint64_t diag_hash(unsigned gain_pct_when_off,bool gate_when_off=false) {
    const auto pcm=diag_signal();
    uint64_t h=1469598103934665603ull;
    for(const auto& c:diag_configs(gain_pct_when_off,gate_when_off)) {
        std::vector<int16_t> buffer(c.max_segment_samples);
        Segmenter s; if(!s.reset(buffer.data(),buffer.size(),c))return 0;
        auto take=[&] {
            const auto* p=s.segment();
            h=fnv(h,p->start_sample);h=fnv(h,p->end_sample);h=fnv(h,p->new_voiced_samples);
            h=fnv(h,p->voiced_end_samples);h=fnv(h,uint64_t(p->end));s.release();
        };
        size_t at=0;
        while(at<pcm.size()) {
            const auto r=s.push(pcm.data()+at,std::min<size_t>(F,pcm.size()-at)); at+=r.consumed;
            h=fnv(h,s.current_rms());h=fnv(h,s.noise_rms());h=fnv(h,s.threshold_rms());h=fnv(h,s.peak_rms());
            if(r.status==SegmentStatus::Ready)take();
        }
        if(s.finish()==SegmentStatus::Ready)take();
        h=fnv(h,s.current_rms());h=fnv(h,s.noise_rms());h=fnv(h,s.threshold_rms());h=fnv(h,s.peak_rms());
    }
    return h;
}
// diag_hash(100) of the header at commit 1a51eac (before decision_pre_emphasis existed).
// tests/test_segmenter_decision.py recomputes it from that commit when git has it.
constexpr uint64_t kGoldenDiagHash=10559851243095114257ull;
}  // namespace

#ifdef SEGMENTER_DIAG_HASH_ONLY
int main() { (void)kGoldenDiagHash; std::printf("%llu\n",(unsigned long long)diag_hash(100)); return 0; }
#else

namespace {
int failures=0;
#define CHECK(c) do { if(!(c)) { std::printf("  FAILED %s (line %d)\n",#c,__LINE__); ++failures; } } while(0)

struct Seg { uint64_t start,end; size_t voice,voiced_end; SegmentEnd end_kind; bool cont; };
bool operator==(const Seg& a,const Seg& b) {
    return a.start==b.start && a.end==b.end && a.voice==b.voice && a.voiced_end==b.voiced_end &&
           a.end_kind==b.end_kind && a.cont==b.cont;
}
struct Frame { uint64_t dec,dec_thr,raw,raw_thr; uint32_t gain; bool open; };
using Ranges=std::vector<std::pair<size_t,size_t>>;

// Pushes `pcm` in `chunk`-sample calls (0: seeded random 1..4096). Every segment's PCM must be
// the source samples. With chunk == F, `frames` receives the diagnostics after every frame and
// `on_ready` sees each ready segment before release.
template <class OnReady>
std::vector<Seg> run(const std::vector<int16_t>& pcm,const SegmenterConfig& c,size_t chunk,
                     std::vector<Frame>* frames,OnReady on_ready,uint32_t seed=1) {
    std::vector<int16_t> buffer(c.max_segment_samples);
    Segmenter s; assert(s.reset(buffer.data(),buffer.size(),c));
    std::vector<Seg> out; std::mt19937 rng(seed);
    auto take=[&] {
        const auto* p=s.segment(); assert(p && p->end_sample<=pcm.size());
        for(size_t i=0;i<p->samples;++i)assert(p->pcm[i]==pcm[p->start_sample+i]);
        on_ready(s);
        out.push_back({p->start_sample,p->end_sample,p->new_voiced_samples,p->voiced_end_samples,p->end,p->continuation});
        assert(s.release());
    };
    size_t at=0;
    while(at<pcm.size()) {
        const size_t n=std::min(pcm.size()-at,chunk?chunk:size_t(1+rng()%4096)); size_t used=0;
        while(used<n) {
            const auto r=s.push(pcm.data()+at+used,n-used); assert(r.consumed); used+=r.consumed;
            assert(r.status==SegmentStatus::Ok || r.status==SegmentStatus::Ready);
            if(frames && (at+used)%F==0) {
                const int16_t* openPcm=nullptr; size_t openSamples=0; uint64_t liveStart=0;
                frames->push_back({s.decision_energy(),s.decision_threshold_energy(),s.raw_energy(),
                                   s.raw_threshold_energy(),s.decision_gain_q16(),
                                   r.status==SegmentStatus::Ready || s.live_view(&liveStart,&openSamples)});
                (void)openPcm;
            }
            if(r.status==SegmentStatus::Ready)take();
        }
        at+=n;
    }
    if(s.finish()==SegmentStatus::Ready)take();
    return out;
}
std::vector<Seg> run(const std::vector<int16_t>& pcm,const SegmenterConfig& c,size_t chunk=F,
                     std::vector<Frame>* frames=nullptr,uint32_t seed=1) {
    return run(pcm,c,chunk,frames,[](Segmenter&){},seed);
}
double covered(const std::vector<Seg>& segs,const Ranges& want) {
    uint64_t in=0,all=0;
    for(auto [a,b]:want) {
        all+=b-a;
        for(const auto& s:segs) { const uint64_t lo=std::max<uint64_t>(a,s.start),hi=std::min<uint64_t>(b,s.end); if(hi>lo)in+=hi-lo; }
    }
    return all?double(in)/double(all):0;
}
// p90 of the segmenter's own raw level (10 ms mean-removed RMS) over frames inside `where`.
double raw_p90(const std::vector<int16_t>& x,const Ranges& where) {
    std::vector<double> lv;
    for(auto [a,b]:where) for(size_t f=a;f+F<=b;f+=F) {
        double s=0,q=0; for(size_t i=f;i<f+F;++i) { s+=x[i]; q+=double(x[i])*x[i]; }
        lv.push_back(std::sqrt(q/F-(s/F)*(s/F)));
    }
    std::sort(lv.begin(),lv.end()); return lv.empty()?0:lv[lv.size()*9/10];
}
SegmenterConfig example(uint8_t pre,uint16_t gain_pct=150) {
    SegmenterConfig c; c.adaptive=true; c.min_segment_samples=64000; c.max_segment_samples=160000;
    c.short_close_silence_samples=16000; c.noise_percentile=25; c.silence_blip_samples=480;
    c.hard_cut_search_samples=32000; c.decision_pre_emphasis=pre; c.decision_gain_pct=gain_pct; return c;
}

// P4-like sources: a red room (lag-1 0.97 plus white) and HF-rich "speech" from a two-pole
// resonator near 2.5 kHz, like loudspeaker playback through this microphone.
struct Source {
    std::mt19937 rng; std::normal_distribution<double> n01{0,1}; double r=0,b1=0,b2=0;
    explicit Source(uint32_t seed):rng(seed) {}
    double room(double level,double white=0.9) { r=0.97*r+level*0.35*n01(rng); return r+white*n01(rng); }
    double talk(double amplitude) { const double y=n01(rng)+b1-0.81*b2; b2=b1; b1=y; return amplitude*0.33*y; }
};

// ---------------------------------------------------------------- U1
void u1() {
    std::vector<int16_t> buffer(kSegmentMaximumSamples); Segmenter s;
    SegmenterConfig c=example(97); CHECK(s.reset(buffer.data(),buffer.size(),c));
    c.decision_pre_emphasis=100; CHECK(s.reset(buffer.data(),buffer.size(),c));
    c.decision_pre_emphasis=1; CHECK(s.reset(buffer.data(),buffer.size(),c));
    c.decision_pre_emphasis=101; CHECK(!s.reset(buffer.data(),buffer.size(),c));
    c=example(97); c.adaptive=false; CHECK(!s.reset(buffer.data(),buffer.size(),c));
    c=example(97); c.decision_gain_pct=24; CHECK(!s.reset(buffer.data(),buffer.size(),c));
    c.decision_gain_pct=25; CHECK(s.reset(buffer.data(),buffer.size(),c));
    c.decision_gain_pct=400; CHECK(s.reset(buffer.data(),buffer.size(),c));
    c.decision_gain_pct=401; CHECK(!s.reset(buffer.data(),buffer.size(),c));
    c=example(0); c.decision_gain_pct=401; CHECK(!s.reset(buffer.data(),buffer.size(),c));
    // Off: the accessors stay at their neutral values through a whole session.
    const auto pcm=diag_signal();
    for(const auto& off:diag_configs(100)) {
        std::vector<int16_t> b(off.max_segment_samples); Segmenter t; assert(t.reset(b.data(),b.size(),off));
        bool neutral=t.decision_gain_q16()==kSegmentGainOne;
        size_t at=0;
        while(at<pcm.size()) {
            const auto r=t.push(pcm.data()+at,std::min<size_t>(F,pcm.size()-at)); at+=r.consumed;
            neutral=neutral && t.decision_gain_q16()==kSegmentGainOne && !t.decision_energy() && !t.decision_noise_energy()
                && !t.decision_threshold_energy() && !t.raw_energy() && !t.raw_threshold_energy();
            if(r.status==SegmentStatus::Ready)t.release();
        }
        CHECK(neutral);
    }
    // Off: current/noise/threshold/peak RMS and every segment equal the header before the option.
    const uint64_t h=diag_hash(100), h400=diag_hash(400);
    std::printf("  option-0 diagnostics hash %llu (golden %llu), with gain_pct 400 %llu\n",
                (unsigned long long)h,(unsigned long long)kGoldenDiagHash,(unsigned long long)h400);
    CHECK(h==kGoldenDiagHash); CHECK(h400==kGoldenDiagHash);
#ifdef SEGMENTER_HAS_GATE
    // The noise-control fields are validated whatever the option, and ignored while it is off.
    c=example(97); c.decision_modulation_ratio_pct=100; CHECK(!s.reset(buffer.data(),buffer.size(),c));
    c.decision_modulation_ratio_pct=101; CHECK(s.reset(buffer.data(),buffer.size(),c));
    c.decision_modulation_ratio_pct=1000; c.decision_pre_hold_requires_raw=true; CHECK(s.reset(buffer.data(),buffer.size(),c));
    c.decision_modulation_ratio_pct=1001; CHECK(!s.reset(buffer.data(),buffer.size(),c));
    c=example(0); c.decision_modulation_ratio_pct=1001; CHECK(!s.reset(buffer.data(),buffer.size(),c));
    c=example(0); c.decision_modulation_ratio_pct=300; c.decision_pre_hold_requires_raw=true; CHECK(s.reset(buffer.data(),buffer.size(),c));
    const uint64_t hg=diag_hash(100,true);
    std::printf("  option-0 diagnostics hash with modulation gate 300 and pre hold set (ignored): %llu\n",(unsigned long long)hg);
    CHECK(hg==kGoldenDiagHash);
    for(const auto& off:diag_configs(100,true)) {
        std::vector<int16_t> b(off.max_segment_samples); Segmenter t; assert(t.reset(b.data(),b.size(),off));
        bool neutral=true; size_t at=0;
        while(at<pcm.size()) {
            const auto r=t.push(pcm.data()+at,std::min<size_t>(F,pcm.size()-at)); at+=r.consumed;
            neutral=neutral && !t.decision_modulated() && !t.decision_raw_heard() && !t.mod_count_;
            if(r.status==SegmentStatus::Ready)t.release();
        }
        CHECK(neutral);
    }
#endif
}

// ---------------------------------------------------------------- U2
std::vector<int16_t> p4_stream(double room,double burst,bool bursts,uint32_t seed,Ranges* where) {
    // As design2-energy-domain/tests/p4_levels_test.cpp: 60 s, a 1.5 s burst every 5 s.
    Source g(seed); std::vector<int16_t> x(60*16000);
    for(size_t i=0;i<x.size();++i) {
        double v=g.room(room);
        const size_t t=i%(5*16000);
        if(bursts && i>=16000 && t>=16000 && t<40000) {
            v+=g.talk(burst);
            if(t==16000 && where)where->push_back({i,i+24000});
        }
        x[i]=clip(v);
    }
    return x;
}
void u2() {
    struct Case { double room,burst,want_off_max,want_on_min; } cases[]={{5,25,0.0,0.85},{15,30,0.2,1.0}};
    for(const auto& k:cases) {
        Ranges w; const auto x=p4_stream(k.room,k.burst,true,7,&w);
        const auto amb=p4_stream(k.room,0,false,8,nullptr);
        const double p90=raw_p90(x,w);
        const auto off=run(x,example(0)), on=run(x,example(97,100));
        const double c_off=covered(off,w), c_on=covered(on,w);
        const size_t a_off=run(amb,example(0)).size(), a_on=run(amb,example(97,100)).size();
        std::printf("  room %.0f: burst raw p90 %.1f; off %zu segments cover %.3f; C=97 gain 100: %zu segments cover %.3f;"
                    " ambient-only 60 s: off %zu, on %zu segments\n",k.room,p90,off.size(),c_off,on.size(),c_on,a_off,a_on);
        CHECK(p90<45); CHECK(c_off<=k.want_off_max); CHECK(c_on>=k.want_on_min); CHECK(a_off==0); CHECK(a_on==0);
        for(size_t chunk:{size_t(1),size_t(159),size_t(161),size_t(1024),size_t(4096)}) CHECK(run(x,example(97,100),chunk)==on);
        for(uint32_t seed:{1u,2u,3u}) CHECK(run(x,example(97,100),0,nullptr,seed)==on);
    }
}

// ---------------------------------------------------------------- U3 (defect a)
void u3() {
    // One fixed 160-sample ambient pattern (bass hum plus a fixed dither); each test frame adds a
    // 3 kHz tone 0.25 counts louder than the last. Only the tone amplitude changes.
    std::mt19937 rng(3); double ambient[F];
    for(size_t t=0;t<F;++t)ambient[t]=7*std::sin(2*M_PI*120.0*double(t)/16000)+double(int(rng()%3)-1)*0.4;
    const SegmenterConfig c=example(97);
    std::vector<int16_t> buffer(c.max_segment_samples); Segmenter s; assert(s.reset(buffer.data(),buffer.size(),c));
    int16_t frame[F];
    for(int k=0;k<60;++k) {
        for(size_t t=0;t<F;++t)frame[t]=clip(ambient[t]);
        if(s.push(frame,F).status==SegmentStatus::Ready)s.release();
    }
    CHECK(s.calibrated());
    std::vector<uint64_t> levels;
    for(int q=12;q<=36;++q) {
        for(size_t t=0;t<F;++t)frame[t]=clip(ambient[t]+q*0.25*std::sin(2*M_PI*3000.0*double(t)/16000+0.3));
        if(s.push(frame,F).status==SegmentStatus::Ready)s.release();
        levels.push_back(s.decision_energy());
    }
    size_t increasing=0; for(size_t i=1;i<levels.size();++i)increasing+=levels[i]>levels[i-1];
    std::printf("  25 tone frames 3.00..9.00 counts: decision energy %.2f..%.2f count^2, %zu of 24 steps increase\n",
                levels.front()/1e4,levels.back()/1e4,increasing);
    CHECK(levels.size()==25); CHECK(increasing==24);
}

// ---------------------------------------------------------------- U4 (defect b)
struct Boot { std::vector<int16_t> pcm; Ranges talk; size_t probe_frame; };
Boot boot_stream(size_t burst_samples) {
    Source g(9); Boot b;
    auto room=[&](size_t n) { for(size_t i=0;i<n;++i)b.pcm.push_back(clip(g.room(5))); };
    for(size_t i=0;i<burst_samples;++i)b.pcm.push_back(clip(g.room(5)+g.talk(1200)));  // loud HF bang
    room(8000-burst_samples+56000);                     // rest of calibration, then 3.5 s idle
    b.probe_frame=64000/F-1;
    for(int k=0;k<6;++k) {
        room(16000);
        b.talk.push_back({b.pcm.size(),b.pcm.size()+24000});
        for(size_t i=0;i<24000;++i)b.pcm.push_back(clip(g.room(5)+g.talk(40)));
    }
    room(32000);
    return b;
}
void u4() {
    // A clean boot and boots whose first 300 ms / 500 ms are a loud HF bang. With
    // kDecisionGainPercentile 25 a 300 ms bang (60% of the 50 calibration frames) does not reach
    // the quartile, so the 500 ms bang (all of calibration) is the case that freezes a bad gain.
    const Boot clean=boot_stream(0);
    std::vector<Frame> fc; const auto sc=run(clean.pcm,example(97),F,&fc);
    const double g_clean=fc[clean.probe_frame].gain/65536.0;
    CHECK(raw_p90(clean.pcm,clean.talk)<45); CHECK(covered(sc,clean.talk)==1.0);
    for(size_t bang:{size_t(4800),size_t(8000)}) {
        const Boot b=boot_stream(bang);
        std::vector<Frame> f; const auto segs=run(b.pcm,example(97),F,&f);
        const double g_cal=f[8000/F-1].gain/65536.0, g=f[b.probe_frame].gain/65536.0;
        std::vector<Seg> off=run(b.pcm,example(0));
        std::printf("  bang %zu ms: talk raw p90 %.1f; gain after calibration %.3f, after 3.5 s idle %.3f (clean boot %.3f);"
                    " %zu segments cover %.3f of the talk (off: %.3f)\n",bang/16,raw_p90(b.pcm,b.talk),g_cal,g,g_clean,
                    segs.size(),covered(segs,b.talk),covered(off,b.talk));
        CHECK(covered(segs,b.talk)==1.0);
        CHECK(std::fabs(g/g_clean-1)<=0.10);
        CHECK(covered(off,b.talk)<0.2);
    }
}

// ---------------------------------------------------------------- U5
void u5() {
    Source g(5); std::vector<int16_t> x; Ranges talk1,loud_room,talk2;
    auto add=[&](size_t n,double room,double talker,double white) {
        for(size_t i=0;i<n;++i)x.push_back(clip(g.room(room,white)+(talker?g.talk(talker):0)));
    };
    add(64000,5,0,0.9);
    talk1.push_back({x.size(),x.size()+24000}); add(24000,5,35,0.9);
    add(48000,5,0,0.9);
    const double up=std::pow(10.0,10.0/20);                  // the room steps up 10 dB
    loud_room.push_back({x.size(),x.size()+128000}); add(128000,5*up,0,0.9*up);
    talk2.push_back({x.size(),x.size()+24000}); add(24000,5*up,35,0.9*up);
    add(32000,5*up,0,0.9*up);
    const auto on=run(x,example(97)), off=run(x,example(0));
    Ranges both=talk1; both.push_back(talk2[0]);
    std::printf("  talk raw p90 before %.1f / after the rise %.1f; room raw p90 %.1f -> %.1f; on: talk1 %.3f, loud room %.3f,"
                " talk2 %.3f; off: talk %.3f\n",raw_p90(x,talk1),raw_p90(x,talk2),raw_p90(x,{{16000,64000}}),
                raw_p90(x,loud_room),covered(on,talk1),covered(on,loud_room),covered(on,talk2),covered(off,both));
    CHECK(raw_p90(x,talk1)<45); CHECK(raw_p90(x,talk2)<45);
    CHECK(covered(on,talk1)==1.0); CHECK(covered(on,loud_room)<=0.05); CHECK(covered(on,talk2)==1.0);
    CHECK(covered(off,both)<0.2);
}

// ---------------------------------------------------------------- U6/U7 (defect c)
// After a quiet cut, every buffered frame's frame_energy_d equals what the stream computed.
void check_recompute(Segmenter& s,const std::vector<Frame>& frames,size_t* checked,bool* equal) {
    if(s.segment()->end!=SegmentEnd::HardLimit || !s.cut_)return;
    for(size_t at=(F-s.start_%F)%F; at+F<=s.used_; at+=F) {
        if(!at)continue;
        const size_t frame=size_t((s.start_+at)/F);
        if(frame>=frames.size())continue;  // the frame that raised Ready is recorded after this call
        *equal=*equal && s.frame_energy_d(at)/(F*F)==frames[frame].dec;
        ++*checked;
    }
}
void u6_u7() {
    SegmenterConfig c; c.adaptive=true; c.max_segment_samples=16000; c.min_segment_samples=16000;
    c.hard_cut_search_samples=4800; c.end_silence_samples=1600; c.pre_roll_samples=0;
    c.noise_percentile=25; c.decision_pre_emphasis=97; c.decision_gain_pct=100;
    // 1.5 s room, 0.8 s of a 500 Hz phrase at amplitude 300, one 10 ms frame of digital zero, then
    // a 6 kHz tail that runs exactly to the 1 s limit (raw below both thresholds, pre-emphasised
    // above), then digital zero. The cut lands in the gap, so the tail is the continuation's only voice.
    Source g(11); std::vector<int16_t> x;
    for(size_t i=0;i<24000;++i)x.push_back(clip(g.room(3)));
    for(size_t t=0;t<12800;++t)x.push_back(clip(300*std::sin(2*M_PI*500.0*double(t)/16000)));
    for(size_t t=0;t<F;++t)x.push_back(0);
    const size_t tail0=x.size(), tail1=24000+16000;
    for(size_t t=tail0;t<tail1;++t)x.push_back(clip(8*std::sin(2*M_PI*6000.0*double(t)/16000)));
    for(size_t t=0;t<32000;++t)x.push_back(0);
    std::vector<Frame> frames; size_t checked=0; bool equal=true;
    const auto segs=run(x,c,F,&frames,[&](Segmenter& s) { check_recompute(s,frames,&checked,&equal); });
    // Preconditions: the tail is voice only in the pre-emphasised domain, and the raw energy is
    // also below the decision threshold (so comparing raw energy with it, the old unit mix, misses it).
    bool shaped=true;
    for(size_t f=tail0/F;f<tail1/F;++f) {
        const Frame& r=frames[f];
        shaped=shaped && r.open && r.dec>=r.dec_thr && r.raw<r.dec_thr && r.raw<r.raw_thr;
    }
    const size_t tail_frames=(tail1-tail0)/F*F;
    const Seg* cont=nullptr; for(const auto& s:segs) if(s.cont && s.start<=tail0 && s.end>=tail1)cont=&s;
    std::printf("  quiet cut: %zu segments (first ends %s at %llu), tail %zu samples, continuation %s with %zu voiced,"
                " tail covered %.3f; tail frames shaped as intended: %s\n",segs.size(),
                segs.empty()?"-":(segs[0].end_kind==SegmentEnd::HardLimit?"hard_limit":"other"),
                segs.empty()?0ull:(unsigned long long)segs[0].end,tail_frames,cont?"delivered":"MISSING",
                cont?cont->voice:size_t(0),covered(segs,{{tail0,tail1}}),shaped?"yes":"NO");
    CHECK(shaped);
    CHECK(!segs.empty() && segs[0].end_kind==SegmentEnd::HardLimit && segs[0].end==tail0-F);
    CHECK(cont!=nullptr); CHECK(cont && cont->voice>=tail_frames);
    CHECK(covered(segs,{{tail0,tail1}})==1.0);
    // U7 on this stream and on a long random one with many quiet cuts.
    size_t cuts=0;
    for(unsigned seed=1;seed<=6;++seed) {
        std::mt19937 rng(seed); std::vector<int16_t> y;
        for(unsigned block=0;block<30;++block) {
            const size_t n=block==0?8000:size_t(1+rng()%23000);
            const int amp=block%3==0?4:(block%3==1?800:55);
            for(size_t i=0;i<n;++i)y.push_back(clip(int(rng()%(2*amp+1))-amp));
        }
        SegmenterConfig d=c; d.min_segment_samples=3200; d.min_voice_samples=320; d.hard_cut_search_samples=3200;
        d.decision_gain_pct=uint16_t(100+50*(seed%3)); d.decision_pre_emphasis=uint8_t(seed%2?97:100);
        std::vector<Frame> fr;
        run(y,d,F,&fr,[&](Segmenter& s) { if(s.cut_)++cuts; check_recompute(s,fr,&checked,&equal); });
    }
    std::printf("  carry recomputation: %zu buffered frames over %zu extra quiet cuts equal the streamed decision energy: %s\n",
                checked,cuts,equal?"yes":"NO");
    CHECK(equal); CHECK(checked>1000); CHECK(cuts>=6);
}

// ---------------------------------------------------------------- U8
void u8() {
    uint16_t prev=0; bool mono=true,bounded=true;
    for(uint64_t v=0;v<70000;++v) {
        const uint16_t code=Segmenter::energy_code(v); mono=mono&&code>=prev; prev=code;
        const uint64_t e=Segmenter::energy_value(code);
        bounded=bounded && e<=v && (v<2048?e==v:(v-e)*1024<e);
    }
    std::mt19937_64 rng(1);
    for(int i=0;i<2000000;++i) {
        const uint64_t v=rng()>>(17+rng()%47), w=v+1+rng()%1000;
        if(w>=(uint64_t(1)<<47))continue;
        const uint16_t cv=Segmenter::energy_code(v), cw=Segmenter::energy_code(w);
        mono=mono&&cv<=cw;
        const uint64_t e=Segmenter::energy_value(cv);
        bounded=bounded && e<=v && (v<2048?e==v:(v-e)*1024<e);
    }
    const uint16_t top=Segmenter::energy_code((uint64_t(1)<<47)-1);
    CHECK(mono); CHECK(bounded); CHECK(top<65535 && Segmenter::energy_value(top)<=(uint64_t(1)<<47)-1);
    size_t windows=0; bool exact=true;
    for(unsigned pct:{0u,10u,25u,50u}) for(int t=0;t<3000;++t) {
        const size_t n=1+rng()%500; std::vector<uint16_t> w(n);
        for(auto& x:w)x=uint16_t(rng()%(t%3==0?40:38912));  // t%3==0: many duplicates
        std::vector<uint16_t> sorted=w; std::sort(sorted.begin(),sorted.end());
        exact=exact && Segmenter::percentile_energy(w.data(),n,pct)==Segmenter::energy_value(sorted[n*pct/100]);
        ++windows;
    }
    std::printf("  energy codes monotone and within 2^-10, exact below 2048; top code %u; percentile exact on %zu windows\n",top,windows);
    CHECK(exact); CHECK(windows==12000);
}

// ---------------------------------------------------------------- U9
void u9() {
    Source g(21); std::vector<int16_t> x;
    for(double room:{5.0,2.0,10.0,4.0,7.0}) for(size_t i=0;i<120000;++i)x.push_back(clip(g.room(room,0.3*room)));
    SegmenterConfig a=example(97),b=example(97); a.noise_percentile=0; b.noise_percentile=25;
    std::vector<Frame> fa,fb; const auto sa=run(x,a,F,&fa), sb=run(x,b,F,&fb);
    bool same=fa.size()==fb.size(), range=true; uint32_t lo=~0u,hi=0;
    for(size_t i=0;i<fa.size() && i<fb.size();++i) {
        same=same && fa[i].gain==fb[i].gain;
        range=range && fa[i].gain>=kSegmentGainMin && fa[i].gain<=kSegmentGainMax;
        lo=std::min(lo,fa[i].gain); hi=std::max(hi,fa[i].gain);
    }
    std::printf("  ambient-only 37.5 s: segments %zu / %zu; gain identical for noise_percentile 0 and 25: %s; range %.3f..%.3f\n",
                sa.size(),sb.size(),same?"yes":"NO",lo/65536.0,hi/65536.0);
    CHECK(sa.empty() && sb.empty()); CHECK(same); CHECK(range);
}

// ---------------------------------------------------------------- U10
void u10() {
    SegmenterConfig c=example(100,400); c.adaptive_speech_floor=32767; c.adaptive_silence_floor=32767;
    for(int floors=0;floors<2;++floors) {
        SegmenterConfig d=floors?c:example(100,400);
        std::vector<int16_t> x(20*16000);
        for(size_t i=0;i<x.size();++i)x[i]=int16_t(i&1?32767:-32768);
        std::vector<int16_t> buffer(d.max_segment_samples); Segmenter s; assert(s.reset(buffer.data(),buffer.size(),d));
        size_t at=0; bool sane=true;
        while(at<x.size()) {
            const auto r=s.push(x.data()+at,std::min<size_t>(1000,x.size()-at)); at+=r.consumed;
            // The ambient goes through a 16-bit energy code (2^-10 resolution): 32754, not 32767.
            if(s.samples_seen()>=9000) sane=sane && s.noise_rms()>=32700 && s.peak_rms()>=32700 && s.threshold_rms()>=32700;
            if(r.status==SegmentStatus::Ready)s.release();
        }
        std::printf("  full-scale square, floors %u: current %u noise %u threshold %u peak %u gain %.3f; no wrap: %s\n",
                    d.adaptive_speech_floor,s.current_rms(),s.noise_rms(),s.threshold_rms(),s.peak_rms(),
                    s.decision_gain_q16()/65536.0,sane?"yes":"NO");
        CHECK(sane);
    }
    // A quiet boot, then full scale: phrases open, hit the limit and quiet-cut in full-scale
    // audio, and finish() closes the last one on 1..159 leftover samples.
    bool all=true; size_t hard=0;
    for(size_t left=1;left<F;++left) {
        std::vector<int16_t> x(8000+(left==1?20*16000:16000+F*7)+left);
        for(size_t i=0;i<x.size();++i)x[i]=int16_t(i<8000?int(i%7)-3:(i&1?32767:-32768));
        for(const auto& d:{example(100,400),c}) {
            const auto segs=run(x,d,1000);
            for(const auto& g:segs)hard+=g.end_kind==SegmentEnd::HardLimit;
            all=all && !segs.empty() && segs.back().end==x.size() && segs.back().end_kind==SegmentEnd::SessionEnd;
        }
    }
    std::printf("  quiet boot then full scale, finish() on 1..159 leftover samples: last phrase delivered to the end: %s"
                " (%zu hard-limit cuts on the way)\n",all?"yes":"NO",hard);
    CHECK(all); CHECK(hard>=2);
}

// ---------------------------------------------------------------- U11 (review: zero ambient)
// Digital silence or constant DC in a quarter of the idle window makes the decision quartile 0.
// That must not become the maximum gain (room tone would then open and hold phrases).
void u11() {
    Source g(31); std::vector<int16_t> clean;
    for(size_t i=0;i<42*16000;++i)clean.push_back(clip(g.room(5)));
    std::vector<int16_t> muted=clean, dc=clean;
    std::fill(muted.begin(),muted.begin()+8000,int16_t(0));                 // 0.5 s of zero at boot
    std::fill(muted.begin()+20*16000,muted.begin()+22*16000,int16_t(0));    // 2 s mute mid-stream
    std::fill(dc.begin(),dc.begin()+16000,int16_t(-300));                   // 1 s of constant DC at boot
    std::vector<Frame> fc,fm,fd;
    const auto sc=run(clean,example(97),F,&fc), sm=run(muted,example(97),F,&fm), sd=run(dc,example(97),F,&fd);
    uint32_t top_c=0,top_m=0,top_d=0;
    for(const auto& f:fc)top_c=std::max(top_c,f.gain);
    for(const auto& f:fm)top_m=std::max(top_m,f.gain);
    for(const auto& f:fd)top_d=std::max(top_d,f.gain);
    // A sub-floor talker right after the mute is still found.
    Ranges talk={{22*16000+16000,22*16000+40000}}; std::vector<int16_t> spoken=muted; Source t(32);
    for(size_t i=talk[0].first;i<talk[0].second;++i)spoken[i]=clip(spoken[i]+t.talk(40));
    const auto ss=run(spoken,example(97));
    std::printf("  room only: clean %zu, zero boot + 2 s mute %zu, DC boot %zu segments; highest gain %.2f / %.2f / %.2f;"
                " final gain %.3f / %.3f / %.3f; talker after the mute (raw p90 %.1f) covered %.3f\n",sc.size(),sm.size(),
                sd.size(),top_c/65536.0,top_m/65536.0,top_d/65536.0,fc.back().gain/65536.0,fm.back().gain/65536.0,
                fd.back().gain/65536.0,raw_p90(spoken,talk),covered(ss,talk));
    CHECK(sc.empty()); CHECK(sm.empty()); CHECK(sd.empty());
    // While only a few room frames follow the silence the quartile is noisy; it must stay a
    // room-like gain (the bench recordings learn 1.1..39 at 100 percent), never kSegmentGainMax.
    CHECK(top_m<=2*top_c); CHECK(top_d<=2*top_c);
    CHECK(fm.back().gain==fc.back().gain); CHECK(fd.back().gain==fc.back().gain);  // last 5 s identical, all idle
    CHECK(raw_p90(spoken,talk)<45); CHECK(covered(ss,talk)==1.0);
}

// ---------------------------------------------------------------- U12 (review: persistent HF noise)
// A steady high-frequency source just above the room never reaches the raw onset rule but can
// open a phrase through the pre-emphasised level. Hard-limit continuations keep the phrase's
// levels, so without the re-seed it stays open for as long as the source runs.
void u12() {
    Source g(41); std::vector<int16_t> x; Ranges noise,talk;
    auto add=[&](size_t n,double hf,double talker,size_t on=0,size_t off=0) {
        for(size_t i=0;i<n;++i) {
            double v=g.room(5); if(hf)v+=g.talk(hf);
            if(talker && (!on || i%(on+off)<on))v+=g.talk(talker);
            x.push_back(clip(v));
        }
    };
    add(30*16000,0,0);
    noise.push_back({x.size(),x.size()+120*16000}); add(120*16000,25,0);
    add(30*16000,0,0);
    talk.push_back({x.size(),x.size()+24000}); add(24000,0,40);
    add(10*16000,0,0);
    // Continuous syllabic talk (250 ms on, 100 ms off) for 45 s: re-seeding must not cut it.
    talk.push_back({x.size(),x.size()+45*16000}); add(45*16000,0,40,4000,1600);
    add(10*16000,0,0);
    const auto on=run(x,example(97)), off=run(x,example(0));
    const double in_on=covered(on,noise)*120, in_off=covered(off,noise)*120;
    size_t hard=0; for(const auto& s:on)hard+=s.end_kind==SegmentEnd::HardLimit;
    std::printf("  steady HF noise (raw p90 %.1f, room %.1f) for 120 s: inside %.1f s on / %.1f s off; talker raw p90 %.1f"
                " covered %.3f / %.3f (off %.3f); %zu segments, %zu hard limits\n",raw_p90(x,noise),raw_p90(x,{{16000,30*16000}}),
                in_on,in_off,raw_p90(x,talk),covered(on,{talk[0]}),covered(on,{talk[1]}),covered(off,talk),on.size(),hard);
    CHECK(raw_p90(x,noise)<45); CHECK(raw_p90(x,talk)<45);
    CHECK(in_off==0); CHECK(in_on<=30.0);
    CHECK(covered(on,{talk[0]})==1.0); CHECK(covered(on,{talk[1]})>=0.99);
}

// ---------------------------------------------------------------- production hash (U13)
// Every per-frame diagnostic (RMS views, both energies and thresholds, the gain) and every
// segment, with the option ON, over the fixtures above. kGoldenProdHash is the value of the
// header before the modulation gate existed (13 Nov 2026, private/segmenter_replay/
// stt_segmenter.before-gate.h); the gate fields at 0 must reproduce it exactly.
std::vector<std::pair<SegmenterConfig,const std::vector<int16_t>*>> prod_fixtures(
        const std::vector<std::vector<int16_t>>& signals) {
    std::vector<SegmenterConfig> cs;
    cs.push_back(example(97));                          // production (97, 150)
    cs.push_back(example(97,100));
    cs.push_back(example(100,400));
    { SegmenterConfig c=example(97); c.freeze_noise_during_speech=true; cs.push_back(c); }
    { SegmenterConfig c=example(97); c.noise_percentile=0; c.short_close_silence_samples=0; cs.push_back(c); }
    { SegmenterConfig c; c.adaptive=true; c.decision_pre_emphasis=97; c.decision_gain_pct=150;  // many quiet cuts
      c.hard_cut_search_samples=3200; c.max_segment_samples=16000; c.min_segment_samples=3200; cs.push_back(c); }
    std::vector<std::pair<SegmenterConfig,const std::vector<int16_t>*>> out;
    for(const auto& c:cs) for(const auto& s:signals) out.push_back({c,&s});
    return out;
}
std::vector<std::vector<int16_t>> prod_signals() {
    std::vector<std::vector<int16_t>> v;
    v.push_back(diag_signal());
    v.push_back(p4_stream(5,25,true,7,nullptr));
    v.push_back(p4_stream(15,30,true,7,nullptr));
    v.push_back(boot_stream(8000).pcm);
    // U12's sources, shortened: room, 40 s of steady HF noise, room, syllabic talk.
    Source g(41); std::vector<int16_t> x;
    for(size_t i=0;i<10*16000;++i)x.push_back(clip(g.room(5)));
    for(size_t i=0;i<40*16000;++i)x.push_back(clip(g.room(5)+g.talk(25)));
    for(size_t i=0;i<10*16000;++i)x.push_back(clip(g.room(5)));
    for(size_t i=0;i<20*16000;++i)x.push_back(clip(g.room(5)+(i%5600<4000?g.talk(40):0)));
    v.push_back(std::move(x));
    return v;
}
template <class Tweak>
uint64_t prod_hash(Tweak tweak) {
    const auto signals=prod_signals();
    uint64_t h=1469598103934665603ull;
    for(auto [c,pcm]:prod_fixtures(signals)) {
        tweak(c);
        std::vector<int16_t> buffer(c.max_segment_samples);
        Segmenter s; if(!s.reset(buffer.data(),buffer.size(),c))return 0;
        auto take=[&] {
            const auto* p=s.segment();
            h=fnv(h,p->start_sample);h=fnv(h,p->end_sample);h=fnv(h,p->new_voiced_samples);
            h=fnv(h,p->voiced_end_samples);h=fnv(h,uint64_t(p->end));h=fnv(h,p->continuation);s.release();
        };
        auto diag=[&] {
            h=fnv(h,s.current_rms());h=fnv(h,s.noise_rms());h=fnv(h,s.threshold_rms());h=fnv(h,s.peak_rms());
            h=fnv(h,s.decision_energy());h=fnv(h,s.decision_noise_energy());h=fnv(h,s.decision_threshold_energy());
            h=fnv(h,s.raw_energy());h=fnv(h,s.raw_threshold_energy());h=fnv(h,s.decision_gain_q16());
        };
        size_t at=0;
        while(at<pcm->size()) {
            const auto r=s.push(pcm->data()+at,std::min<size_t>(F,pcm->size()-at)); at+=r.consumed;
            diag();
            if(r.status==SegmentStatus::Ready)take();
        }
        if(s.finish()==SegmentStatus::Ready)take();
        diag();
    }
    return h;
}
constexpr uint64_t kGoldenProdHash=3709073732580116122ull;

#ifdef SEGMENTER_HAS_GATE
SegmenterConfig gated(uint16_t ratio_pct=300,bool hold=true) {
    SegmenterConfig c=example(97); c.decision_modulation_ratio_pct=ratio_pct; c.decision_pre_hold_requires_raw=hold; return c;
}

// ---------------------------------------------------------------- U13
// Gate fields at 0: production (and every other option-on fixture) bit-for-bit as before the gate.
void u13() {
    const uint64_t h=prod_hash([](SegmenterConfig&){});
    std::printf("  option-on hash, noise controls off: %llu (golden %llu)\n",(unsigned long long)h,(unsigned long long)kGoldenProdHash);
    CHECK(h==kGoldenProdHash);
    // And the controls really are live when on (the hash moves).
    const uint64_t g=prod_hash([](SegmenterConfig& c) { c.decision_modulation_ratio_pct=300; c.decision_pre_hold_requires_raw=true; });
    CHECK(g!=kGoldenProdHash);
}

// ---------------------------------------------------------------- U14 (modulation gate)
// A steady high-frequency source (U12's resonator noise, unbroken) above a P4-like room: below
// the raw floor, above the pre-emphasised onset. Faded in over 3 s after 20 s of room (a fan
// spinning up), production opens on it and holds phrases open; with the gate its pre-emphasised
// level is unmodulated and never votes, so no phrase opens. Present from boot, nothing opens.
std::vector<int16_t> hf_noise(size_t lead,double fade_s,Ranges* noise) {
    Source g(51); std::vector<int16_t> x;
    for(size_t i=0;i<lead;++i)x.push_back(clip(g.room(5)));
    const size_t n=60*16000; noise->push_back({x.size(),x.size()+n});
    for(size_t i=0;i<n;++i) {
        const double a=fade_s?std::min(1.0,double(i)/(fade_s*16000)):1.0;
        x.push_back(clip(g.room(5)+a*g.talk(25)));
    }
    for(size_t i=0;i<10*16000;++i)x.push_back(clip(g.room(5)));
    return x;
}
void u14() {
    for(size_t lead:{size_t(0),size_t(20*16000)}) {
        Ranges noise; const auto x=hf_noise(lead,lead?3:0,&noise);
        const auto prod=run(x,example(97)), gate=run(x,gated(300,false)), both=run(x,gated());
        const double in_p=covered(prod,noise)*60;
        std::printf("  steady HF noise %s (raw p90 %.1f): inside %.1f s production; segments %zu production / %zu gate / %zu gate+hold\n",
                    lead?"faded in after 20 s":"from boot",raw_p90(x,noise),in_p,prod.size(),gate.size(),both.size());
        CHECK(raw_p90(x,noise)<45);
        if(lead)CHECK(in_p>=10.0);
        CHECK(gate.empty()); CHECK(both.empty());
    }
    // decision_modulated() reports whether the pre-emphasised level votes: on every frame while
    // the gate is off (the hold alone), on few frames of steady noise with the gate on.
    Ranges noise; const auto x=hf_noise(0,0,&noise);
    auto votes=[&](const SegmenterConfig& c) {
        std::vector<int16_t> b(c.max_segment_samples); Segmenter t; assert(t.reset(b.data(),b.size(),c));
        size_t on=0,frames=0;
        for(size_t at=0;at<x.size();) {
            const auto r=t.push(x.data()+at,std::min<size_t>(F,x.size()-at)); at+=r.consumed;
            ++frames; on+=t.decision_modulated()?1:0;
            if(r.status==SegmentStatus::Ready)t.release();
        }
        return std::pair<size_t,size_t>{on,frames};
    };
    const auto hold=votes(gated(0,true)), gate=votes(gated(300,false));
    std::printf("  pre path votes on %zu/%zu frames with the hold alone, %zu/%zu with gate 300\n",
                hold.first,hold.second,gate.first,gate.second);
    CHECK(hold.first==hold.second); CHECK(gate.first*20<gate.second);
}

// ---------------------------------------------------------------- U15 (pre hold)
// An abrupt steady source looks modulated for its first 50..450 ms, so the gated pre path can
// open a phrase; the source's raw level (above 2 x the raw ambient, below the 45 floor) then
// keeps it open through hard limits unless raw votes wait for a raw latch. Cases: U14's HF
// noise starting abruptly, and a quiet HF talker (pre only) running straight into steady
// white noise (raw ~22, pre-emphasised far above the onset).
void u15() {
    Ranges noise; const auto x=hf_noise(20*16000,0,&noise);
    {
        const auto prod=run(x,example(97)), gate=run(x,gated(300,false)), both=run(x,gated());
        const double in_p=covered(prod,noise)*60, in_g=covered(gate,noise)*60, in_b=covered(both,noise)*60;
        std::printf("  HF noise, abrupt start: inside %.1f s production / %.1f s gate / %.1f s gate+hold; segments %zu / %zu / %zu\n",
                    in_p,in_g,in_b,prod.size(),gate.size(),both.size());
        CHECK(in_g>=10.0); CHECK(in_b<=2.0); CHECK(both.size()<=1);
    }
    Source g(61),w(62); std::normal_distribution<double> n01(0,1); std::vector<int16_t> y; Ranges talk,white;
    for(size_t i=0;i<20*16000;++i)y.push_back(clip(g.room(5)));
    talk.push_back({y.size(),y.size()+2*16000});
    for(size_t i=0;i<2*16000;++i)y.push_back(clip(g.room(5)+(i%5600<4000?g.talk(40):0)));
    white.push_back({y.size(),y.size()+40*16000});
    for(size_t i=0;i<40*16000;++i)y.push_back(clip(g.room(5)+21*n01(w.rng)));
    for(size_t i=0;i<10*16000;++i)y.push_back(clip(g.room(5)));
    const auto prod=run(y,example(97)), gate=run(y,gated(300,false)), both=run(y,gated());
    const double in_p=covered(prod,white)*40, in_g=covered(gate,white)*40, in_b=covered(both,white)*40;
    double peak=0; for(size_t f=white[0].first;f+F<=white[0].second;f+=F)peak=std::max(peak,raw_p90(y,{{f,f+F}}));
    std::printf("  talker (raw p90 %.1f) into white noise (raw p90 %.1f, max frame %.1f): noise inside %.1f s production /"
                " %.1f s gate / %.1f s gate+hold; talker covered %.3f / %.3f / %.3f\n",raw_p90(y,talk),raw_p90(y,white),peak,
                in_p,in_g,in_b,covered(prod,talk),covered(gate,talk),covered(both,talk));
    CHECK(raw_p90(y,talk)<45); CHECK(peak<45);
    CHECK(covered(prod,talk)==1.0); CHECK(covered(gate,talk)==1.0); CHECK(covered(both,talk)==1.0);
    CHECK(in_g>=10.0); CHECK(in_b<=2.0);
    // The same white noise after a talker who DOES reach the raw floor: the raw latch carries
    // into at most one continuation after the talker's last raw-latched part, not through the
    // whole hard-cut chain (before 13 Nov it held the noise as long as production did).
    Source h(63),v(64); std::vector<int16_t> z; Ranges loud,white2;
    for(size_t i=0;i<20*16000;++i)z.push_back(clip(h.room(5)));
    loud.push_back({z.size(),z.size()+3*16000});
    for(size_t i=0;i<3*16000;++i)z.push_back(clip(h.room(5)+(i%5600<4000?h.talk(90):0)));
    white2.push_back({z.size(),z.size()+40*16000});
    for(size_t i=0;i<40*16000;++i)z.push_back(clip(h.room(5)+21*n01(v.rng)));
    for(size_t i=0;i<10*16000;++i)z.push_back(clip(h.room(5)));
    const auto prod2=run(z,example(97)), gate2=run(z,gated(300,false)), both2=run(z,gated());
    const double in_p2=covered(prod2,white2)*40, in_g2=covered(gate2,white2)*40, in_b2=covered(both2,white2)*40;
    const double phrase_s=double(example(97).max_segment_samples)/16000;
    std::printf("  loud talker (raw p90 %.1f) into the same noise: noise inside %.1f s production / %.1f s gate /"
                " %.1f s gate+hold; segments %zu / %zu / %zu; talker covered %.3f / %.3f / %.3f\n",raw_p90(z,loud),
                in_p2,in_g2,in_b2,prod2.size(),gate2.size(),both2.size(),covered(prod2,loud),covered(gate2,loud),covered(both2,loud));
    CHECK(raw_p90(z,loud)>=45);
    CHECK(covered(prod2,loud)==1.0); CHECK(covered(gate2,loud)==1.0); CHECK(covered(both2,loud)==1.0);
    CHECK(in_g2>=2.5*phrase_s); CHECK(in_b2<=2*phrase_s);
}

// ---------------------------------------------------------------- U16 (speech still opens)
// Quiet syllabic speech (250 ms on, 100 ms off; raw p90 below the 45 floor) over a day room and
// a night room, starting 2 s after boot and after a long idle: the gated detector covers at least
// what production covers, and the old raw detector finds none of it. Also chunk-size invariance with the gate.
void u16() {
    for(double room:{5.0,2.0}) {
        Source g(71); std::vector<int16_t> x; Ranges talk;
        auto idle=[&](double s) { for(size_t i=0;i<size_t(s*16000);++i)x.push_back(clip(g.room(room,0.3*room+0.6))); };
        auto speak=[&](double s,double amp) {
            talk.push_back({x.size(),x.size()+size_t(s*16000)});
            for(size_t i=0;i<size_t(s*16000);++i)x.push_back(clip(g.room(room,0.3*room+0.6)+(i%5600<4000?g.talk(amp):0)));
        };
        idle(2); speak(3,40); idle(30); speak(6,40); idle(5); speak(1.4,35); idle(8);
        const auto prod=run(x,example(97)), both=run(x,gated()), gate=run(x,gated(300,false)), old=run(x,example(0));
        std::printf("  room %.0f: talk raw p90 %.1f; covered %.3f production / %.3f gate / %.3f gate+hold / %.3f option 0;"
                    " segments %zu / %zu / %zu\n",room,raw_p90(x,talk),covered(prod,talk),covered(gate,talk),covered(both,talk),
                    covered(old,talk),prod.size(),gate.size(),both.size());
        CHECK(raw_p90(x,talk)<45);
        // No speech lost against production (onset is a few frames later at night in every arm).
        CHECK(covered(prod,talk)>=0.95); CHECK(covered(gate,talk)>=covered(prod,talk)); CHECK(covered(both,talk)>=covered(prod,talk));
        CHECK(covered(old,talk)<0.5);
        for(size_t chunk:{size_t(1),size_t(159),size_t(161),size_t(1024),size_t(4096)}) CHECK(run(x,gated(),chunk)==both);
        for(uint32_t seed:{1u,2u,3u}) CHECK(run(x,gated(),0,nullptr,seed)==both);
    }
}
#endif  // SEGMENTER_HAS_GATE
}  // namespace

#ifdef SEGMENTER_PROD_HASH_ONLY
int main() { std::printf("%llu\n",(unsigned long long)prod_hash([](SegmenterConfig&){})); return 0; }
#else
int main(int argc,char** argv) {
    struct Case { const char* name; void (*fn)(); } cases[]={
        {"U1",u1},{"U2",u2},{"U3",u3},{"U4",u4},{"U5",u5},{"U6",u6_u7},{"U8",u8},{"U9",u9},{"U10",u10},{"U11",u11},{"U12",u12},
        {"U13",u13},{"U14",u14},{"U15",u15},{"U16",u16}};
    int ran=0;
    for(const auto& k:cases) {
        bool want=argc<2;
        for(int i=1;i<argc;++i)want=want||std::string(argv[i])==k.name||(std::string(argv[i])=="U7"&&std::string(k.name)=="U6");
        if(!want)continue;
        const int before=failures;
        std::printf("%s%s\n",k.name,std::string(k.name)=="U6"?" + U7":"");
        k.fn(); ++ran;
        std::printf("  %s\n",failures==before?"ok":"FAILED");
    }
    std::printf("segmenter decision: %d case groups, %d failed checks\n",ran,failures);
    return failures||!ran?1:0;
}
#endif  // SEGMENTER_PROD_HASH_ONLY
#endif  // SEGMENTER_DIAG_HASH_ONLY
