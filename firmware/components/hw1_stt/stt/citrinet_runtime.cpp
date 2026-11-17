#if defined(HW1_BOARD_CONFIG_HEADER) || defined(HW1_FEATURE_CONFIG_HEADER) || defined(HW1_DEPLOYMENT_CONFIG_HEADER)
#endif
// Citrinet-384's implementation of the local-STT runtime API (quartznet_runtime.h, shared by both models;
// a build compiles this file or quartznet_runtime.cpp, ENABLE_STT_MODEL_CITRINET / _QUARTZNET). The same
// loading, verification, admission, resident model, stage loop and decoder hook as QuartzNet's, with
// Citrinet's 80-band front end (citrinet_frontend.h), its four time rates (T, T/2, T/4, T/8) and fixed-size
// squeeze-and-excitation tensors in the resident model, its arena size, and a WordPiece greedy fallback
// (stt_lm_wordpiece.h) when no decoder (LM) decodes. The output is int8 [ceil(T/8) x 257], blank last.
#include "quartznet_runtime.h"
#include "citrinet_runtime.h"
#include "stt_internal_memory.h"
#include "citrinet_frontend.h"
#include "citrinet_vocab.h"
#include "stt_lm_wordpiece.h"
#include "stt_model_selection.h"
#include "stt_model_container.h"
#include "dl_model_base.hpp"
#if HW1_STT_RESIDENT_MODEL
// The tree-local ESP-DL (tools/p4/prepare_esp_dl.py) provides the metadata
// allocator hook; stt_dl_metadata.h installs a PSRAM one.
#include "dl_tool_metadata.hpp"
#include "stt_dl_metadata.h"
#endif
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"
#if defined(ESP_PLATFORM) && CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#include "esp_attr.h"
#define STT_PROFILE_BSS EXT_RAM_BSS_ATTR
#else
#define STT_PROFILE_BSS
#endif
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "miniz.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>
#if HW1_STT_RESIDENT_MODEL
// The tree-local ESP-DL's prepared-args pass (dl_module_conv.hpp).
extern "C" int hw1_dl_prepare_mode;
extern "C" uint32_t hw1_dl_prepare_gen;
#endif
#if !HW1_STT_CITRINET
#error "citrinet_runtime.cpp is the Citrinet build's runtime (ENABLE_STT_MODEL_CITRINET)"
#endif
namespace hw1::stt {
namespace fe = citrinet;
static_assert(identity::kFeatureBins==fe::kMelBins && identity::kTimeStride==8 &&
              identity::kOutputClasses==int(citrinet::kVocabularyTokens)+1, "Citrinet identity and front end disagree");
constexpr size_t kBins=fe::kMelBins, kClasses=identity::kOutputClasses;
constexpr size_t outputFrames(size_t frames) { return (frames+identity::kTimeStride-1)/identity::kTimeStride; }
// Activation arena per output frame of the pinned graph, measured on the P4 (greedy plan: 8.9 KB at 83 and
// at 126 output frames), with margin; QuartzNet's is 2048.
constexpr size_t kArenaPerOutput=9728;
// Greedy tokens of one call: at most one per output frame (30 s: 375).
constexpr size_t kMaxGreedyTokens=(identity::kMaxFeatureFrames+7)/8;
// Per-stage inference time accumulated across segments (sttperf stages).
static constexpr int kStageProfileMax = 512;
namespace { lm::wordpiece::Pieces* gPieces=nullptr; }
const lm::wordpiece::Pieces* citrinetPieces() { return gPieces; }
STT_PROFILE_BSS static uint32_t gStageTotalUs[kStageProfileMax];
static uint32_t gStageSegments = 0;
size_t stageProfile(uint32_t* totalUs, size_t cap, uint32_t* segments) {
    const size_t n = std::min<size_t>(cap, std::min<int>(identity::kNodeCount, kStageProfileMax));
    for (size_t i = 0; i < n; ++i) totalUs[i] = gStageTotalUs[i];
    if (segments) *segments = gStageSegments;
    return n;
}
void stageProfileReset() {
    for (auto& v : gStageTotalUs) v = 0;
    gStageSegments = 0;
}
namespace {
static_assert(kSampleRate==identity::kSampleRate && kMaxSamples==identity::kMaxSamples);
uint32_t millis() { return uint32_t(esp_timer_get_time()/1000); }
// Do not reconfigure the global watchdog around inference. Both cores retain
// their subscriptions; the bounded stage loop blocks periodically after the
// multicore kernel has joined, letting idle tasks run without unsubscribe races.
bool cancelled(const STTLocalControl& c) { return c.cancelled && c.cancelled(c.context); }
void progress(const STTLocalControl& c, STTLocalPhase phase) { if(c.progress)c.progress(c.context,phase); }
struct Buffer {
    void* p = nullptr;
    size_t bytes = 0;
    bool sensitive = false;
    explicit Buffer(size_t n, bool secret=false):bytes(n),sensitive(secret) {
        p=heap_caps_aligned_alloc(16,n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    }
    ~Buffer() { if(sensitive && p) { volatile uint8_t* d=static_cast<volatile uint8_t*>(p); for(size_t i=0;i<bytes;++i)d[i]=0; } heap_caps_free(p); }
    void* release() { void* value=p; p=nullptr; return value; }
    Buffer(const Buffer&)=delete;
    Buffer& operator=(const Buffer&)=delete;
};
bool readExact(ModelReader& r, void* dest, size_t size) {
    auto p=static_cast<uint8_t*>(dest);
    while(size) { size_t n=r.read(r.context,p,size); if(!n || n>size)return false; p+=n;size-=n; }
    return true;
}
bool digest(const void* data, size_t bytes, uint8_t* hash) {
    return mbedtls_sha256(static_cast<const unsigned char*>(data),bytes,hash,0)==0;
}
// ROM streaming inflate uses a non-wrapping output buffer of exactly the pinned
// size. No untrusted length reaches ESP-DL's unchecked flatbuffer parser.
// Time spent inside reader calls during the current load (card/file I/O only).
int64_t gLoadReadUs = 0;
bool timedRead(ModelReader& r, void* dest, size_t size) {
    const int64_t start=esp_timer_get_time();
    const bool ok=readExact(r,dest,size);
    gLoadReadUs+=esp_timer_get_time()-start;
    return ok;
}
// Stored (codec 0) graph: read straight into the final buffer in large blocks.
// A 20 MB graph in 4 KB reads spends most of its time in per-call file-system
// and SDMMC overhead; 256 KB transfers let the driver stream multi-block DMA.
bool readStoredModel(ModelReader& r, uint8_t* raw, const STTLocalControl& control) {
    constexpr size_t kBlock=256*1024;
    for(size_t done=0;done<identity::kRawBytes;) {
        if(cancelled(control))return false;
        const size_t n=std::min(kBlock,identity::kRawBytes-done);
        if(!timedRead(r,raw+done,n))return false;
        done+=n;
        vTaskDelay(1);
    }
    return true;
}
bool inflateModel(ModelReader& r, uint8_t* raw, const STTLocalControl& control) {
    Buffer workspace(sizeof(tinfl_decompressor)+4096);
    if(!workspace.p)return false;
    auto dec=static_cast<tinfl_decompressor*>(workspace.p);
    auto in=reinterpret_cast<uint8_t*>(dec+1);
    tinfl_init(dec);
    size_t left=identity::kCompressedBytes, available=0, offset=0, output=0, chunks=0;
    for(;;) {
        if(cancelled(control))return false;
        if(available==offset && left) {
            available=std::min(left,size_t(4096));offset=0;
            if(!timedRead(r,in,available))return false;
            left-=available;
        }
        size_t n=available-offset, out=identity::kRawBytes-output;
        uint32_t flags=TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
        if(left)flags|=TINFL_FLAG_HAS_MORE_INPUT;
        tinfl_status status=tinfl_decompress(dec,in+offset,&n,raw,raw+output,&out,flags);
        offset+=n;output+=out;
        if(status==TINFL_STATUS_DONE)return output==identity::kRawBytes && !left && offset==available;
        if(status<0 || (n==0 && out==0) || output>=identity::kRawBytes)return false;
        if(!left && offset==available && status==TINFL_STATUS_NEEDS_MORE_INPUT)return false;
        if(++chunks%32==0)vTaskDelay(1);
    }
}
bool tensor(dl::TensorBase* t, size_t frames, size_t channels, int exponent) {
    return t && t->data && t->dtype==dl::DATA_TYPE_INT8 && t->exponent.is_valid()
        && !t->exponent.is_per_channel() && int(t->exponent)==exponent
        && t->shape==std::vector<int>({1,int(frames),1,int(channels)})
        && t->get_bytes()==int(frames*channels);
}
}
// Bench builds only: prove on the device that the resident model on a trimmed image gives the bytes a
// fresh model on the whole image gives (TL selfcheck, in transcribe()).
#ifndef HW1_STT_SELFCHECK
#define HW1_STT_SELFCHECK 0
#endif
#if HW1_STT_RESIDENT_MODEL
// ---- HW1_STT_RESIDENT_MODEL: one resident vendor model per session ----
// Built once at the broker's 20 s cap (STT_MAX_CAPTURE_MS; the broker never
// sends more) and re-shaped for every later call. ESP-DL modules keep no
// shape state (Conv rebuilds its arguments from the live tensor shapes on each
// forward), TensorBase::set_shape never reallocates, and the greedy plan gave
// each tensor its max-length extent, so a call runs on a prefix of the same
// arena: same kernels, same arithmetic, no per-call load()/build().
constexpr size_t kResidentMaxSamples=10*kSampleRate;  // the broker's continuous phrases are capped at 10 s
constexpr size_t kResidentCallMargin=256*1024;    // per-call PSRAM beyond the frontend
// Reusing the pinned resident graph does not rebuild model/arena metadata.
// Keep a separate 128 KiB admission reserve for kernel scratch, small SDK
// allocations and concurrent system work. Cold construction still requires
// 150 KiB below. This policy needs hardware peak qualification; it is not a
// measured scratch bound or a credit from another task's heap delta.

constexpr size_t kResidentKeepFree=2u<<20;         // PSRAM left free after the build
// Everything the resident model keeps between phrases is requested
// explicitly from PSRAM where it is allocated. The vendor model's own objects
// and containers come from ESP-DL's metadata allocator, which transcribe()
// points at PSRAM before the first model is built (installDlMetadataAllocator,
// stt_dl_metadata.h: ps_alloc PreferPSRAM). The model object itself is one of
// those blocks (dl::Model's class operator new/delete, so unique_ptr's delete
// returns it), and this bookkeeping is ps_alloc'd too (class operator
// new/delete below, PsramStlAllocator for the tensor list). No malloc
// interception, no task-gated window, no heap threshold change.
struct ModelCache::Resident {
    struct Entry { dl::TensorBase* tensor; int channels; int divisor; };   // divisor 0: length 1 at any T
    std::unique_ptr<dl::Model> model;
    std::vector<Entry,PsramStlAllocator<Entry>> tensors;  // every activation tensor, once
    dl::TensorBase* input=nullptr;
    dl::TensorBase* output=nullptr;
    size_t frames=0;             // feature frames of the current shapes
    bool trimmed=false;          // the weights image was cut to identity::kResidentKeepBytes for this model
    // Only new(std::nothrow): a plain new Resident does not compile.
    static void* operator new(size_t n,const std::nothrow_t&) noexcept {
        static_assert(alignof(Resident)<=sizeof(void*),"the heap aligns blocks to the pointer size");
        return sttPsramAlloc(n,"stt.resident");
    }
    static void operator delete(void* p) noexcept { sttPsramFree(p); }
    static void operator delete(void* p,const std::nothrow_t&) noexcept { sttPsramFree(p); }
};
namespace {
// Lists every activation tensor with the rate it runs at: ceil(T/d) for d = 1 (feature frames), 2, 4 or 8
// (after each stride-2 stage), or length 1 (squeeze-and-excitation pooling and its two 1x1 convolutions).
// At the build length these five lengths are distinct; any other tensor refuses the resident model.
// Heap snapshots around construction are diagnostic only; other tasks may
// allocate or free memory during the build.
ModelCache::Resident* buildResident(const void* raw,const char*& why) {
    const size_t maxFrames=fe::feature_frames(kResidentMaxSamples), maxOutputs=outputFrames(maxFrames);
    static_assert((kResidentMaxSamples+159)/160>8, "rates must be distinguishable at the build length");
    std::unique_ptr<ModelCache::Resident> r(new(std::nothrow) ModelCache::Resident);
    if(!r) { why="alloc"; return nullptr; }
    std::map<std::string,std::vector<int>> shapes{{identity::kInputName,{1,int(maxFrames),1,int(kBins)}}};
    const DlCompactExponents compactExponents;
    r->model.reset(new(std::nothrow) dl::Model(static_cast<const char*>(raw),fbs::MODEL_LOCATION_IN_FLASH_RODATA,0,
        dl::MEMORY_MANAGER_GREEDY,nullptr,false,shapes));
    if(!r->model) { why="model"; return nullptr; }
    r->input=r->model->get_input(identity::kInputName);
    r->output=r->model->get_output(identity::kOutputName);
    if(!tensor(r->input,maxFrames,kBins,identity::kInputExponent) || !tensor(r->output,maxOutputs,kClasses,identity::kOutputExponent)) {
        why="io"; return nullptr;
    }
    fbs::FbsModel* graph=r->model->get_fbs_model();
    if(!graph) { why="graph"; return nullptr; }
    graph->load_map();
    std::vector<std::string> names=graph->get_graph_inputs();
    std::vector<std::string> inputs,outputs;
    bool listed=true;
    for(const std::string& node:graph->topological_sort()) {
        if(graph->get_operation_inputs_and_outputs(node,inputs,outputs)!=ESP_OK) { listed=false; break; }
        names.insert(names.end(),outputs.begin(),outputs.end());
    }
    graph->clear_map();
    if(!listed) { why="graph"; return nullptr; }
    r->tensors.reserve(names.size());  // one block, no growth copies
    for(const std::string& name:names) {
        dl::TensorBase* t=name.empty() ? nullptr : r->model->get_intermediate(name);
        if(!t) { why="tensor"; return nullptr; }
        bool seen=false;
        for(const auto& e:r->tensors) if(e.tensor==t) { seen=true; break; }
        if(seen) continue;
        const auto& s=t->shape;
        if(s.size()!=4 || s[0]!=1 || s[2]!=1 || s[3]<1 || t->dtype!=dl::DATA_TYPE_INT8) { why="shape"; return nullptr; }
        int divisor=-1;
        if(s[1]==1) divisor=0;
        for(int d=1;d<=8 && divisor<0;d*=2) if(s[1]==int((maxFrames+d-1)/d)) divisor=d;
        if(divisor<0) { why="shape"; return nullptr; }
        r->tensors.push_back({t,s[3],divisor});
    }
    r->frames=maxFrames;
    return r.release();
}
bool shapeResident(ModelCache::Resident& r,size_t frames,size_t outputs) {
    if(r.frames!=frames) {
        for(const auto& e:r.tensors)
            e.tensor->set_shape({1,e.divisor ? int((frames+e.divisor-1)/e.divisor) : 1,1,e.channels});
        r.frames=frames;
    }
    return tensor(r.input,frames,kBins,identity::kInputExponent) && tensor(r.output,outputs,kClasses,identity::kOutputExponent);
}
}
#endif  // HW1_STT_RESIDENT_MODEL
ModelCache::~ModelCache() { reset(); }
#if HW1_STT_RESIDENT_MODEL
// The resident model references the weights zero-copy: destroy it first.
void ModelCache::reset() { delete resident_; resident_=nullptr; residentTried_=false; heap_caps_free(raw_); raw_=nullptr; }
// Its arena still holds the last call's activations and quantized input:
// freed whenever the session changes hands (STTLocalSession::moveTo), and the
// next owner's first call may build a fresh one. A trimmed image cannot build
// another model (transcribe(), kResidentKeepBytes): its model stays for as long
// as the weights do, and changing hands zeroes that arena instead.
void ModelCache::dropResident() {
    if(resident_ && resident_->trimmed) { resident_->model->clear_arena(); return; }
    delete resident_; resident_=nullptr; residentTried_=false;
}
#else
void ModelCache::reset() { heap_caps_free(raw_); raw_=nullptr; }
#endif
#if HW1_STT_RUNTIME_DIAGNOSTICS
bool ModelCache::verify() const {
    uint8_t hash[32];
    return raw_ && digest(raw_,identity::kRawBytes,hash) && !memcmp(hash,identity::kRawSha,32);
}
#endif

// Mirrors transcribe()'s admission below (arenaBudget, frontendBytes,
// metadataMargin and the resident model's call budget); test_quartznet_cache
// checks that a call is admitted at exactly this much free PSRAM and refused
// one byte below.
size_t callBudget(size_t samples, const ModelCache* cache, size_t* contiguous) {
    const size_t frames=fe::feature_frames(samples), outputs=outputFrames(frames);
    const size_t arenaBudget=kArenaPerOutput*outputs+64*1024;
    const size_t frontendBytes=frames*kBins*sizeof(float)+sizeof(fe::FrontendWorkspace);
    constexpr size_t metadataMargin=1024*1024;
#if HW1_STT_RESIDENT_MODEL
    if(cache && cache->raw_ && cache->resident_ && samples<=kResidentMaxSamples) {
        if(contiguous)*contiguous=frames*kBins*sizeof(float)+kResidentCallMargin;
        return frontendBytes+kResidentCallMargin;
    }
#else
    (void)cache;
#endif
    if(contiguous)*contiguous=arenaBudget+metadataMargin;
    return arenaBudget+frontendBytes+metadataMargin;
}

bool transcribe(ModelReader reader, const int16_t* pcm, size_t samples,
                char* text, size_t capacity, const STTLocalControl& control,
                STTLocalStats& stats, char* error, size_t errorCapacity,
                Diagnostics* diagnostics, ModelCache* cache, const Decoder* decoder,
                const STTFeaturePrefix* prefix) {
    InternalMemoryScope internalMemory;
    stats={}; if(text && capacity)text[0]=0;if(error && errorCapacity)error[0]=0;
#if HW1_STT_RUNTIME_DIAGNOSTICS
    if(diagnostics)*diagnostics={};
#else
    (void)diagnostics;
#endif
    auto fail=[&](const char* why) { if(text && capacity)text[0]=0; if(error && errorCapacity)snprintf(error,errorCapacity,"%s",why);return false; };
#if HW1_STT_RESIDENT_MODEL
    // Before this process builds its first STT model, point ESP-DL's
    // metadata allocator at PSRAM (stt_dl_metadata.h). Idempotent; it stays
    // installed, so per-call models (the warm-up, calls beyond the resident
    // length) keep their metadata in PSRAM too.
    installDlMetadataAllocator();
#endif
    if(!pcm || !text || capacity<2 || !reader.read || samples<kMinSamples || samples>kMaxSamples)return fail("Invalid STT audio buffer");
    if(cancelled(control))return fail("Cancelled");
    const size_t frames=fe::feature_frames(samples), outputs=outputFrames(frames);
    stats.samples=samples;stats.featureFrames=frames;stats.outputFrames=outputs;
    if(!gPieces) {   // once per boot: the vocabulary the greedy fallback and the LM decoder share
        void* mem=heap_caps_malloc(sizeof(lm::wordpiece::Pieces),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(mem) {
            auto* pieces=new(mem) lm::wordpiece::Pieces();
            if(pieces->parse(citrinet::kVocabulary,sizeof(citrinet::kVocabulary)-1) &&
               pieces->size()==citrinet::kVocabularyTokens) gPieces=pieces;
            else { pieces->~Pieces(); heap_caps_free(mem); }
        }
        if(!gPieces)return fail("Cannot prepare the STT vocabulary");
    }
    if(!container::valid_size(reader.bytes))return fail("STT model size does not match this firmware");
    progress(control,STTLocalPhase::Loading);uint32_t start=millis();
    if(!container::read_header(reader.context,reader.bytes,reader.read))return fail("STT model identity mismatch");
    // This pinned graph's greedy arena is about 8.9 KB per output frame (P4 probe), kept with margin. Keep
    // extra alignment and metadata/kernel-scratch margins: vendor constructors do not report every OOM.
    const size_t arenaBudget=kArenaPerOutput*outputs+64*1024;
    const size_t frontendBytes=frames*kBins*sizeof(float)+sizeof(fe::FrontendWorkspace);
    constexpr size_t metadataMargin=1024*1024;
    const size_t remainingBudget=arenaBudget+frontendBytes+metadataMargin;
    // A null cache keeps one-shot calls self-contained. Continuous sessions
    // retain this allocation only until their worker has joined/closed.
    ModelCache temporary;
    ModelCache& weights=cache ? *cache : temporary;
#if HW1_STT_RESIDENT_MODEL
    // A trimmed image serves its resident model only: a call that model cannot take reloads the file.
    if(weights.raw_ && weights.resident_ && weights.resident_->trimmed && samples>kResidentMaxSamples)weights.reset();
#endif
    const bool reused=weights.raw_!=nullptr;
    const size_t newWeights=reused ? 0 : identity::kRawBytes;
#if HW1_STT_RESIDENT_MODEL
    // The resident path already owns its arena and metadata. Its remaining
    // allocation budget is independent of cold construction and of allocations
    // made by unrelated HTTP/Bluetooth tasks while that model was built.
    ModelCache::Resident* const resident=reused && weights.resident_ && samples<=kResidentMaxSamples
        ? weights.resident_ : nullptr;
    const size_t callBudget=resident ? frontendBytes+kResidentCallMargin : remainingBudget;
    const size_t callLargest=resident ? frames*kBins*sizeof(float)+kResidentCallMargin : arenaBudget+metadataMargin;
    const auto internalBudget=internalMemoryBudget(resident!=nullptr);
    const size_t callInternal=internalBudget.total;
    // Optional decoder memory (a cached LM) yields before any refusal.
    auto shed=[&] { return decoder && decoder->release && decoder->release(decoder->context); };
    auto psramAdmitted=[&] {
        return heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>=newWeights+callBudget
            && (reused || heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)>=identity::kRawBytes);
    };
#else
    // Optional decoder memory (a cached LM) yields before any refusal.
    auto shed=[&] { return decoder && decoder->release && decoder->release(decoder->context); };
    auto psramAdmitted=[&] {
        return heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>=newWeights+remainingBudget
            && (reused || heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)>=identity::kRawBytes);
    };
    const auto internalBudget=internalMemoryBudget(false);
    const size_t callInternal=internalBudget.total;
#endif
    // Releasing a PSRAM language model cannot satisfy an internal SRAM floor.
    // Do not churn that cache or degrade decoding for unrelated SRAM pressure.
    if(!psramAdmitted()) (void)shed();
    if(!psramAdmitted() || heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<callInternal ||
       heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<internalBudget.contiguous) {
#if HW1_STT_RESIDENT_MODEL
        const size_t requiredInternal=callInternal, requiredPsram=newWeights+callBudget;
#else
        const size_t requiredInternal=callInternal, requiredPsram=newWeights+remainingBudget;
#endif
        const size_t internalFree=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        const size_t internalLargest=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        const size_t psramFree=heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const size_t largest=heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        ESP_LOGW("STT","admission refused: internal=%u need=%u psram=%u need=%u largest=%u weights=%u internal_largest=%u block_need=%u",
                 unsigned(internalFree),unsigned(requiredInternal),unsigned(psramFree),
                 unsigned(requiredPsram),unsigned(largest),unsigned(newWeights),unsigned(internalLargest),unsigned(internalBudget.contiguous));
        return fail(internalFree<requiredInternal ? "Not enough internal SRAM for speech" :
                    internalLargest<internalBudget.contiguous ? "No contiguous internal SRAM block for speech" :
                    psramFree<requiredPsram ? "Not enough PSRAM for speech" :
                    "No contiguous PSRAM block for speech model");
    }
    if(!reused) {
        Buffer candidate(identity::kRawBytes);
        if(!candidate.p)return fail("Cannot allocate STT model");
        gLoadReadUs=0;
        const int64_t loadStart=esp_timer_get_time();
        const bool loaded=identity::kCodec==0
            ? readStoredModel(reader,static_cast<uint8_t*>(candidate.p),control)
            : inflateModel(reader,static_cast<uint8_t*>(candidate.p),control);
        if(!loaded)return fail(cancelled(control)?"Cancelled":"STT model read failed");
        const int64_t hashStart=esp_timer_get_time();
        uint8_t hash[32];
        if(!digest(candidate.p,identity::kRawBytes,hash) || memcmp(hash,identity::kRawSha,32))return fail("STT model checksum mismatch");
        const int64_t hashEnd=esp_timer_get_time();
        // Once per session (weights are cached after this), so always logged.
        ESP_LOGI("STT","model %u bytes %s: read %lu ms, inflate %lu ms, sha256 %lu ms, total %lu ms",
                 unsigned(identity::kRawBytes),identity::kCodec==0?"stored":"zlib",
                 (unsigned long)(gLoadReadUs/1000),
                 (unsigned long)((hashStart-loadStart-gLoadReadUs)/1000),
                 (unsigned long)((hashEnd-hashStart)/1000),
                 (unsigned long)((hashEnd-loadStart)/1000));
        if(cancelled(control))return fail("Cancelled");
        weights.raw_=candidate.release(); // publish only fully verified weights
    }
    // Cached weights were fully verified (SHA-256) when this session loaded
    // them. Re-hashing all 7.2 MB before every segment cost ~0.6 s per
    // segment on the P4 (sttperf), so production reuse trusts that check;
    // diagnostics probes keep the per-segment re-validation.
#if HW1_STT_RUNTIME_DIAGNOSTICS
    else {
        uint8_t hash[32];
        if(!digest(weights.raw_,identity::kRawBytes,hash) || memcmp(hash,identity::kRawSha,32)) {
            weights.reset();
            return fail("Cached STT model checksum mismatch");
        }
    }
#endif
    stats.modelBytes=identity::kRawBytes;
    stats.weightsReused=reused;
    if(cancelled(control))return fail("Cancelled");
    // Raw model allocation can split the largest block. Check again before
    // the vendor constructor requests a contiguous arena and creates metadata.
#if HW1_STT_RESIDENT_MODEL
    auto contiguous=[&] {
        return heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>=callBudget
            && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)>=callLargest;
    };
    if(!contiguous()) (void)shed();
    if(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<callInternal)
        return fail("Not enough internal SRAM for speech");
    if(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<internalBudget.contiguous)
        return fail("No contiguous internal SRAM block for speech");
    if(!contiguous())
        return fail("Not enough contiguous memory for STT activations");
    // A fresh model per utterance avoids Model::build's resize allocation leak.
    // Aligned EDL2+ in PSRAM allows zero-copy parameters; weights outlive model.
    // Later calls of a session use the resident model instead.
    std::unique_ptr<dl::Model> perCall;
    dl::Model* model=nullptr;
    dl::TensorBase* input=nullptr;
    dl::TensorBase* output=nullptr;
    if(resident) {
        if(!shapeResident(*resident,frames,outputs))return fail("STT runtime tensor mismatch");
        model=resident->model.get(); input=resident->input; output=resident->output;
    } else {
        std::map<std::string,std::vector<int>> shapes{{identity::kInputName,{1,int(frames),1,int(kBins)}}};
        const DlCompactExponents compactExponents;
        perCall.reset(new(std::nothrow) dl::Model(
            static_cast<const char*>(weights.raw_),fbs::MODEL_LOCATION_IN_FLASH_RODATA,0,
            dl::MEMORY_MANAGER_GREEDY,nullptr,false,shapes));
        if(!perCall)return fail("Cannot create STT model");
        model=perCall.get();
        input=model->get_input(identity::kInputName);output=model->get_output(identity::kOutputName);
        if(!tensor(input,frames,kBins,identity::kInputExponent) || !tensor(output,outputs,kClasses,identity::kOutputExponent))return fail("STT runtime tensor mismatch");
    }
#else
    auto contiguous=[&] {
        return heap_caps_get_free_size(MALLOC_CAP_SPIRAM)>=remainingBudget
            && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)>=arenaBudget+metadataMargin;
    };
    if(!contiguous()) (void)shed();
    if(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<callInternal)
        return fail("Not enough internal SRAM for speech");
    if(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<internalBudget.contiguous)
        return fail("No contiguous internal SRAM block for speech");
    if(!contiguous())
        return fail("Not enough contiguous memory for STT activations");
    // A fresh model per utterance avoids Model::build's resize allocation leak.
    // Aligned EDL2+ in PSRAM allows zero-copy parameters; weights outlive model.
    std::map<std::string,std::vector<int>> shapes{{identity::kInputName,{1,int(frames),1,int(kBins)}}};
    const DlCompactExponents compactExponents;
    std::unique_ptr<dl::Model> model(new(std::nothrow) dl::Model(
        static_cast<const char*>(weights.raw_),fbs::MODEL_LOCATION_IN_FLASH_RODATA,0,
        dl::MEMORY_MANAGER_GREEDY,nullptr,false,shapes));
    if(!model)return fail("Cannot create STT model");
    auto input=model->get_input(identity::kInputName);auto output=model->get_output(identity::kOutputName);
    if(!tensor(input,frames,kBins,identity::kInputExponent) || !tensor(output,outputs,kClasses,identity::kOutputExponent))return fail("STT runtime tensor mismatch");
#endif
#if HW1_STT_RUNTIME_DIAGNOSTICS
    if(diagnostics) {
        const auto memory=model->get_memory_info();
        const auto variable=memory.find("variable");
        if(variable!=memory.end()) {
            diagnostics->activationPsramBytes=variable->second.psram;
            diagnostics->activationInternalBytes=variable->second.internal;
        }
    }
#endif
    stats.loadMs=millis()-start;
    if(cancelled(control))return fail("Cancelled");
#if HW1_STT_RESIDENT_MODEL
    // On the resident model, first build every Conv layer's args in one pass,
    // so their setup code and tensor metadata come into the caches once
    // instead of after every layer's weights flushed them; each layer then
    // runs on its prepared args. Same args, bit-identical output. The
    // prepared blocks are PSRAM metadata the resident model keeps. Per-call
    // models (the warm-up) skip the pass, so the LM, which the warm-up loads
    // lazily at its decode, sees the same PSRAM as before.
    // The pass runs before the input is written: the planner gives a later
    // tensor the input's place in the arena, and any layer that still computed
    // in this pass would write its output over the features (until 16 Nov
    // 2026 the pass followed the front end and Citrinet's GlobalAveragePool,
    // Sigmoid and Mul computed in it: every phrase ran on a damaged input).
    // The real pass reads nothing but the input before writing it.
    uint32_t prepareMs=0;
    if(resident) {
        const uint32_t prepareStart=millis();
        if(++hw1_dl_prepare_gen==0)++hw1_dl_prepare_gen;
        hw1_dl_prepare_mode=1;
        for(int stage=0;stage<identity::kNodeCount;++stage)model->run(identity::kNodeCount,stage,dl::RUNTIME_MODE_MULTI_CORE);
        hw1_dl_prepare_mode=0;
        prepareMs=millis()-prepareStart;
    }
#endif
    progress(control,STTLocalPhase::Frontend);start=millis();
    {
        Buffer features(frames*kBins*sizeof(float),true), workspace(sizeof(fe::FrontendWorkspace),true);
        if(!features.p || !workspace.p)return fail("Cannot allocate STT frontend");
        auto f=static_cast<float*>(features.p);
        FrontendTiming timing;
        timing.now_us=[]() -> uint64_t { return static_cast<uint64_t>(esp_timer_get_time()); };
        // No dither (Citrinet's export and calibration use none). Rows computed during capture
        // (HW1_STT_CAPTURE_FEATURES) are copied, only the rest are computed here (citrinet::log_mel_frame:
        // the same per-row arithmetic), then the same CMVN: bit-identical to compute_features on this PCM.
        // A row computed alone re-conditions its whole 400-sample window, so a cache covering under a
        // third of the rows falls back to the batch front end.
        auto* ws=static_cast<fe::FrontendWorkspace*>(workspace.p);
        if(prefix && prefix->rows && prefix->count && prefix->count<=frames && prefix->count*3>=frames) {
            const int64_t copyStart=esp_timer_get_time();
            memcpy(f,prefix->rows,prefix->count*kBins*sizeof(float));
            const int64_t tailStart=esp_timer_get_time();
            for(size_t row=prefix->count;row<frames;++row)
                if(fe::log_mel_frame(pcm,samples,row,f+row*kBins,ws)!=Status::Ok)return fail("STT frontend failed");
            timing.conditionUs=static_cast<uint32_t>(tailStart-copyStart);   // copy of cached rows
            timing.fftUs=static_cast<uint32_t>(esp_timer_get_time()-tailStart);  // rows computed here
            if(fe::normalize_features(f,frames,ws,&timing)!=Status::Ok)return fail("STT frontend failed");
            stats.featureCachedFrames=static_cast<uint32_t>(prefix->count);
        } else if(fe::compute_features(pcm,samples,f,frames*kBins,ws,&timing)!=Status::Ok)return fail("STT frontend failed");
        stats.frontendConditionUs=timing.conditionUs; stats.frontendFftUs=timing.fftUs;
        stats.frontendMelUs=timing.melUs; stats.frontendNormUs=timing.normUs;
        const int64_t quantizeStart=esp_timer_get_time();
        auto q=static_cast<int8_t*>(input->data);
        const float scale=std::ldexp(1.0f,-identity::kInputExponent);
        for(size_t i=0;i<frames*kBins;++i) {
            // nearest-even, matching ESP-PPQ/P4 quantization, independent of
            // ambient FPU rounding mode (roundf would round ties away).
            float x=f[i]*scale, lo=std::floor(x), part=x-lo;
            int value=part>0.5f?int(lo)+1:part<0.5f?int(lo):(int(lo)&1)?int(lo)+1:int(lo);
            q[i]=static_cast<int8_t>(std::max(-128,std::min(127,value)));
        }
        stats.quantizeUs=static_cast<uint32_t>(esp_timer_get_time()-quantizeStart);
    }
    stats.frontendMs=millis()-start;
#if HW1_STT_RUNTIME_DIAGNOSTICS
    if(diagnostics) {
        digest(input->data,input->get_bytes(),diagnostics->inputSha);
        diagnostics->freePsramAtInference=heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        diagnostics->freeInternalAtInference=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    }
#endif
    if(cancelled(control))return fail("Cancelled");
    progress(control,STTLocalPhase::Inference);start=millis();
    // Blocking with vTaskDelay(1) every 5 stages cost 41 ticks per 15x5 run
    // (each up to 1 ms: 32-54 ms per phrase). The 5-stage points now only
    // taskYIELD() (round-robin with other priority-2 tasks; the capture task
    // and anything above priority 2 preempt at any time anyway), and one
    // blocking vTaskDelay(1) is kept on an esp_timer clock: at the first
    // 5-stage point at least kYieldBlockUs after the last block. That still
    // lets both idle tasks (watchdog 5 s) and the priority-1 loop run
    // several times a second after the multicore workers have joined. Drafts stay
    // preemptible: the cancel check still runs before every stage.
    constexpr int64_t kYieldBlockUs=250000;
    int64_t lastBlockUs=esp_timer_get_time();
    for(int stage=0;stage<identity::kNodeCount;++stage) {
        if(cancelled(control))return fail("Cancelled");
        const int64_t stageStart=esp_timer_get_time();
        model->run(identity::kNodeCount,stage,dl::RUNTIME_MODE_MULTI_CORE);
        const int64_t stageEnd=esp_timer_get_time();
        // Keep the three slowest stages (sorted, slowest first) for sttperf.
        uint32_t us=static_cast<uint32_t>(stageEnd-stageStart);
        uint16_t index=static_cast<uint16_t>(stage);
        for(int k=0;k<3;++k) if(stats.slowStage[k]==0xffff || us>stats.slowStageUs[k]) {
            std::swap(us,stats.slowStageUs[k]); std::swap(index,stats.slowStage[k]);
        }
        if(stage<kStageProfileMax){ gStageTotalUs[stage]+=static_cast<uint32_t>(stageEnd-stageStart); }
        // Yield every 5 stages (was every stage: 201 x ~1 ms on the 15x5);
        // block only on the timer above, otherwise a non-blocking yield.
        if((stage+1)%5==0 || stage+1==identity::kNodeCount) {
            if(stageEnd-lastBlockUs>=kYieldBlockUs) {
                vTaskDelay(1);
                lastBlockUs=esp_timer_get_time();
                ++stats.inferenceBlocks;
            } else {
                taskYIELD();
            }
            stats.inferenceYieldUs+=static_cast<uint32_t>(esp_timer_get_time()-stageEnd);
        }
    }
    ++gStageSegments;
    stats.inferenceMs=millis()-start;
#if HW1_STT_RESIDENT_MODEL
    stats.inferenceMs+=prepareMs;   // the prepare pass belongs to inference, wherever it runs
#endif
    if(cancelled(control))return fail("Cancelled");
    progress(control,STTLocalPhase::Decoding);start=millis();
#if HW1_STT_RUNTIME_DIAGNOSTICS
    if(diagnostics)digest(output->data,output->get_bytes(),diagnostics->outputSha);
#endif
    // Any decoder failure other than cancellation decodes greedily instead.
    auto logits=static_cast<const int8_t*>(output->data);
    DecodeScratch scratch;
#if HW1_STT_RESIDENT_MODEL
    // The resident model's arena is idle until the next run, which writes all of it again before reading
    // it: the logits move to its start and the rest is lent to the decoder (the word-piece search otherwise
    // allocates up to 0.7 MB per phrase). A per-call model's arena is small and freed with it: nothing lent.
    if(resident) {
        void* root=nullptr; size_t arena=0;
        model->get_psram_arena(&root,&arena);
        const size_t logitBytes=outputs*kClasses, kept=(logitBytes+15)&~size_t(15);
        if(root && arena>kept) {
            memmove(root,logits,logitBytes);
            logits=static_cast<const int8_t*>(root);
            scratch={static_cast<uint8_t*>(root)+kept,arena-kept};
        }
    }
#endif
    DecodeResult decoded=decoder && decoder->decode
        ? decoder->decode(decoder->context,logits,outputs,identity::kOutputExponent,text,capacity,control,scratch)
        : DecodeResult::Fallback;
    if(decoded==DecodeResult::Cancelled)return fail("Cancelled");
    if(decoded==DecodeResult::Decoded && !memchr(text,0,capacity))decoded=DecodeResult::Fallback;
    if(decoded!=DecodeResult::Decoded) {
        // WordPiece greedy CTC: first-index argmax per frame, repeats and blanks collapsed, the tokenizer's
        // join (as hw1lm_wordpiece's reference and the training code's greedy decode).
        Buffer tokens(kMaxGreedyTokens*sizeof(uint16_t));   // PSRAM: the STT task's stack has ~4 KB spare
        if(!tokens.p)return fail("Cannot allocate STT decode");
        auto* ids=static_cast<uint16_t*>(tokens.p);
        size_t count=0; int previous=-1;
        for(size_t t=0;t<outputs;++t) {
            const int8_t* row=logits+t*kClasses;
            size_t best=0;
            for(size_t c=1;c<kClasses;++c) if(row[c]>row[best]) best=c;
            if(best!=kClasses-1 && int(best)!=previous) {
                if(count==kMaxGreedyTokens)return fail("STT transcript exceeds result capacity");
                ids[count++]=uint16_t(best);
            }
            previous=int(best);
        }
        if(gPieces->join(ids,count,text,capacity)==SIZE_MAX)return fail("STT transcript exceeds result capacity");
    }
    stats.lmUsed=decoded==DecodeResult::Decoded;
    stats.decodeMs=millis()-start;
    if(cancelled(control))return fail("Cancelled");
#if HW1_STT_RESIDENT_MODEL
    // Build the session's resident model once, after the session's first
    // call (the warm-up) has decoded, so a lazily loaded LM already took its
    // PSRAM, and after this call's own model and arena are gone. It needs the
    // budget of a 20 s per-call build and must leave kResidentKeepFree.
    if(cache && !cache->resident_ && !cache->residentTried_) {
        cache->residentTried_=true;
        perCall.reset();
        const size_t maxOutputs=outputFrames(fe::feature_frames(kResidentMaxSamples));
        const size_t need=kArenaPerOutput*maxOutputs+64*1024+metadataMargin;
        const size_t internal0=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        const size_t psram0=heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const char* why="memory";
        const uint32_t buildStart=millis();
        DlMetadataStats kind0[kDlMetadataKinds];
        for(size_t k=0;k<kDlMetadataKinds;++k)kind0[k]=dlMetadataStats(k);
        const DlMetadataStats meta0=dlMetadataStats();
        const dl::tool::MetadataForeignStats foreign0=dl::tool::metadata_foreign_stats();
        if(psram0>=need+kResidentKeepFree && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)>=need
           && internal0>=internalMemoryBudget(false).total
           && heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)>=internalMemoryBudget(false).contiguous)
            cache->resident_=buildResident(cache->raw_,why);
        // Global heap delta is diagnostic, not model-owned memory. Network
        // tasks can change it in either direction; it never changes admission.
        const size_t internal1=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        const DlMetadataStats meta=dlMetadataDelta(meta0,dlMetadataStats());
        const dl::tool::MetadataForeignStats foreign1=dl::tool::metadata_foreign_stats();
        // int_used/psram_used are process-wide positive heap deltas, not
        // ownership attribution. Kept as diagnostic fields for existing tools;
        // meta_n / meta_bytes are the blocks (and bytes) ESP-DL requested
        // through the metadata allocator during the build (growth copies
        // included; meta_frees of them were returned), fallback /
        // fallback_bytes those that landed in internal RAM (PSRAM refused),
        // meta_failed those neither heap could serve. param_objs are the
        // parameter tensor objects prebuilt libfbs_model allocated itself
        // (heap order puts them in PSRAM), param_int how many of them are in
        // internal RAM. TL e3m: bytes requested per metadata kind. All
        // numeric, so tl_capture.py records both lines.
        if(cache->resident_) {
            size_t arenaPsram=0,arenaInternal=0;
            cache->resident_->model->get_arena_size(&arenaPsram,&arenaInternal);
            ESP_LOGI("STT","TL e3 resident=1 t=%lu build_ms=%lu tensors=%u int_used=%u psram_used=%u int_free=%u psram_free=%u"
                     " arena=%u arena_int=%u meta_n=%u meta_bytes=%u meta_frees=%u fallback=%u fallback_bytes=%u"
                     " meta_failed=%u param_objs=%u param_int=%u",
                     (unsigned long)millis(),(unsigned long)(millis()-buildStart),unsigned(cache->resident_->tensors.size()),
                     unsigned(internal0>internal1 ? internal0-internal1 : 0),
                     unsigned(psram0-std::min(psram0,heap_caps_get_free_size(MALLOC_CAP_SPIRAM))),
                     unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),
                     unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                     unsigned(arenaPsram),unsigned(arenaInternal),unsigned(meta.allocs),unsigned(meta.bytes),
                     unsigned(meta.frees),unsigned(meta.internal),unsigned(meta.internalBytes),unsigned(meta.failed),
                     unsigned(foreign1.objects-foreign0.objects),unsigned(foreign1.internal-foreign0.internal));
            uint32_t kindBytes[kDlMetadataKinds];
            for(size_t k=0;k<kDlMetadataKinds;++k)kindBytes[k]=dlMetadataDelta(kind0[k],dlMetadataStats(k)).bytes;
            static_assert(kDlMetadataKinds==13,"TL e3m lists every metadata kind");
            ESP_LOGI("STT","TL e3m model=%u ctx=%u loader=%u fbs=%u module=%u tensor=%u exp=%u shape=%u index=%u list=%u names=%u "
                           "plan=%u fbsmap=%u",
                     unsigned(kindBytes[0]),unsigned(kindBytes[1]),unsigned(kindBytes[2]),unsigned(kindBytes[3]),
                     unsigned(kindBytes[4]),unsigned(kindBytes[5]),unsigned(kindBytes[6]),unsigned(kindBytes[7]),
                     unsigned(kindBytes[8]),unsigned(kindBytes[9]),unsigned(kindBytes[10]),unsigned(kindBytes[11]),
                     unsigned(kindBytes[12]));
#if HW1_STT_SELFCHECK
            // Bench check, once per session (never a release build): one fixed input through fresh per-call
            // models on the whole image (int and byte exponent arrays) and through the resident model before
            // and after its image is trimmed, with its arena as left, zeroed and filled with other data.
            // Same graph, same kernels: every output must be the same bytes.
            constexpr size_t kCheckFrames=500;
            const size_t checkOutputs=outputFrames(kCheckFrames);
            auto checkInput=[&](dl::TensorBase* in) {
                uint32_t x=12345;
                auto* q=static_cast<int8_t*>(in->data);
                for(size_t i=0;i<kCheckFrames*kBins;++i) { x=x*1664525u+1013904223u; q[i]=int8_t(int((x>>24)%96)-48); }
            };
            uint32_t checks[8]={};   // first four bytes of each output's SHA-256; 0: did not run
            auto word=[](const uint8_t* sha) { return (uint32_t(sha[0])<<24)|(uint32_t(sha[1])<<16)|(uint32_t(sha[2])<<8)|sha[3]; };
            auto perCallCheck=[&](bool compact,bool prepare) -> uint32_t {
                std::unique_ptr<DlCompactExponents> bytes(compact ? new DlCompactExponents : nullptr);
                std::map<std::string,std::vector<int>> shapes{{identity::kInputName,{1,int(kCheckFrames),1,int(kBins)}}};
                std::unique_ptr<dl::Model> reference(new(std::nothrow) dl::Model(
                    static_cast<const char*>(cache->raw_),fbs::MODEL_LOCATION_IN_FLASH_RODATA,0,
                    dl::MEMORY_MANAGER_GREEDY,nullptr,false,shapes));
                dl::TensorBase* in=reference ? reference->get_input(identity::kInputName) : nullptr;
                dl::TensorBase* out=reference ? reference->get_output(identity::kOutputName) : nullptr;
                if(!tensor(in,kCheckFrames,kBins,identity::kInputExponent) || !tensor(out,checkOutputs,kClasses,identity::kOutputExponent))return 0;
                if(prepare) {
                    if(++hw1_dl_prepare_gen==0)++hw1_dl_prepare_gen;
                    hw1_dl_prepare_mode=1;
                    for(int stage=0;stage<identity::kNodeCount;++stage)reference->run(identity::kNodeCount,stage,dl::RUNTIME_MODE_MULTI_CORE);
                    hw1_dl_prepare_mode=0;
                }
                checkInput(in);
                for(int stage=0;stage<identity::kNodeCount;++stage) {
                    reference->run(identity::kNodeCount,stage,dl::RUNTIME_MODE_MULTI_CORE);
                    if(stage%5==4)taskYIELD();
                }
                uint8_t sha[32]={};
                return digest(out->data,out->get_bytes(),sha) ? word(sha) : 0;
            };
            auto residentCheck=[&](int fill) -> uint32_t {   // fill: -1 the arena as it is, else that byte
                auto& r=*cache->resident_;
                void* root=nullptr; size_t arena=0;
                r.model->get_psram_arena(&root,&arena);
                if(root && fill>=0)memset(root,fill,arena);
                if(!shapeResident(r,kCheckFrames,checkOutputs))return 0;
                if(++hw1_dl_prepare_gen==0)++hw1_dl_prepare_gen;
                hw1_dl_prepare_mode=1;
                for(int stage=0;stage<identity::kNodeCount;++stage)r.model->run(identity::kNodeCount,stage,dl::RUNTIME_MODE_MULTI_CORE);
                hw1_dl_prepare_mode=0;
                checkInput(r.input);
                for(int stage=0;stage<identity::kNodeCount;++stage) {
                    r.model->run(identity::kNodeCount,stage,dl::RUNTIME_MODE_MULTI_CORE);
                    if(stage%5==4)taskYIELD();
                }
                uint8_t sha[32]={};
                return digest(r.output->data,r.output->get_bytes(),sha) ? word(sha) : 0;
            };
            checks[0]=perCallCheck(false,false);   // per-call, int exponents: upstream's path
            checks[1]=perCallCheck(true,false);    // per-call, byte exponents
            checks[2]=perCallCheck(true,true);     // per-call, byte exponents, prepared conv arguments
            checks[3]=residentCheck(-1);           // resident, whole image
            checks[4]=residentCheck(-1);           // again: the arena now holds the previous run
#endif
#if !HW1_STT_RUNTIME_DIAGNOSTICS   // diagnostics builds re-hash the whole image before every call
            // The resident model reads the image's tables and weights only. What the packager put after them
            // (tools/stt_p4/repack_espdl.py: per-channel exponents, parameter shapes and names, the embedded
            // test tensors) was copied out while models were built, and no other model will be built from this
            // image: return it. A shrinking realloc stays in place; were the block ever moved, every parameter
            // tensor would point at freed memory, so the cache is emptied and the next call loads the file again.
            if(identity::kResidentKeepBytes<identity::kRawBytes) {
                void* const kept=heap_caps_realloc(cache->raw_,identity::kResidentKeepBytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
                const bool inPlace=kept==cache->raw_;
                if(inPlace)cache->resident_->trimmed=true;
                else if(kept) { cache->raw_=kept; cache->reset(); }
                ESP_LOGI("STT","TL e3t keep=%u freed=%u ok=%d psram_free=%u psram_largest=%u",
                         unsigned(identity::kResidentKeepBytes),unsigned(identity::kRawBytes-identity::kResidentKeepBytes),
                         kept ? (inPlace ? 1 : -1) : 0,unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                         unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
            }
#endif
#if HW1_STT_SELFCHECK
            if(cache->resident_) {
                checks[5]=residentCheck(-1);       // resident, image trimmed
                checks[6]=residentCheck(0);        // arena zeroed first
                checks[7]=residentCheck(0xa5);     // arena filled with other data first
                ESP_LOGW("STT","TL selfcheck trimmed=%d frames=%u percall_int=%08x percall_bytes=%08x percall_prepared=%08x resident=%08x "
                               "resident_again=%08x trimmed_image=%08x arena_zero=%08x arena_a5=%08x",
                         cache->resident_->trimmed ? 1 : 0,unsigned(kCheckFrames),unsigned(checks[0]),unsigned(checks[1]),
                         unsigned(checks[2]),unsigned(checks[3]),unsigned(checks[4]),unsigned(checks[5]),unsigned(checks[6]),
                         unsigned(checks[7]));
            }
#endif
        } else {
            ESP_LOGW("STT","TL e3 resident=0 reason=%s int_free=%u psram_free=%u psram_largest=%u meta_n=%u fallback=%u; per-call models",
                     why,unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)),
                     unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                     unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
                     unsigned(meta.allocs),unsigned(meta.internal));
        }
    }
#endif
#if HW1_STT_RESIDENT_MODEL
    internalMemory.success(resident!=nullptr);
#else
    internalMemory.success(false);
#endif
    // Model destructor releases its arena before temporary weights are freed.
    // Session weights remain immutable and are rechecked on the next call.
    return true;
}
}
