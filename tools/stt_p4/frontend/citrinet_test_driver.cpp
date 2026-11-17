// Host driver for test_citrinet_frontend.py.
//   citrinet_test_driver                      argument and edge checks only
//   citrinet_test_driver IN.pcm16le OUT.f32le MODE
// MODE batch: compute_features. rows: log_mel_frame per row, then normalize_features. ring: the same rows
// through log_mel_frame_ring from a power-of-two ring the segment wraps. rows and ring must equal batch bit
// for bit (the Python side compares the files).
#include "citrinet_frontend.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
using namespace hw1::stt;
namespace c=hw1::stt::citrinet;

static void edge_tests() {
    c::FrontendWorkspace w;
    std::vector<int16_t> pcm(kMaxSamples+1,0);
    std::vector<float> out(c::kMelBins*3001);
    assert(c::feature_frames(kMinSamples-1)==0 && c::feature_frames(kMinSamples)==2);
    assert(c::feature_frames(kMaxSamples)==3000 && c::feature_frames(kMaxSamples+1)==0);
    assert(c::compute_features(pcm.data(),kMinSamples-1,out.data(),out.size(),&w)==Status::InvalidArgument);
    assert(c::compute_features(pcm.data(),kMaxSamples+1,out.data(),out.size(),&w)==Status::InvalidArgument);
    assert(c::compute_features(pcm.data(),1600,out.data(),10*c::kMelBins-1,&w)==Status::OutputTooSmall);
    assert(c::compute_features(nullptr,1600,out.data(),out.size(),&w)==Status::InvalidArgument);
    assert(c::compute_features(pcm.data(),1600,out.data(),out.size(),nullptr)==Status::InvalidArgument);
    assert(c::log_mel_frame(pcm.data(),1600,10,out.data(),&w)==Status::InvalidArgument);   // frames = 10
    assert(c::log_mel_frame_ring(pcm.data(),4094,0,1600,0,out.data(),&w)==Status::InvalidArgument);   // not 2^k-1
    assert(c::normalize_features(out.data(),1,&w)==Status::InvalidArgument);
    puts("Citrinet front end bounds, capacity and argument checks passed");
}

int main(int argc,char** argv) {
    edge_tests();
    if(argc==1) return 0;
    if(argc!=4) return 2;
    const std::string mode=argv[3];
    std::ifstream input(argv[1],std::ios::binary|std::ios::ate);
    if(!input) return 3;
    const auto bytes=input.tellg();
    if(bytes<0||static_cast<size_t>(bytes)%2||static_cast<size_t>(bytes)>kMaxSamples*2) return 4;
    std::vector<int16_t> pcm(static_cast<size_t>(bytes)/2); input.seekg(0);
    input.read(reinterpret_cast<char*>(pcm.data()),bytes); if(!input) return 5;
    const size_t frames=c::feature_frames(pcm.size());
    std::vector<float> result(frames*c::kMelBins+2,12345.0f);   // guard values on both sides
    float* rows=result.data()+1;
    c::FrontendWorkspace w;
    Status status=Status::Ok;
    if(mode=="batch") {
        status=c::compute_features(pcm.data(),pcm.size(),rows,frames*c::kMelBins,&w);
    } else if(mode=="rows"||mode=="ring") {
        size_t size=4096;
        while(size<pcm.size()) size*=2;
        std::vector<int16_t> ring(size,0);
        const uint64_t start=size-3;   // the segment wraps the ring
        for(size_t i=0;i<pcm.size()&&mode=="ring";++i) ring[(start+i)&(size-1)]=pcm[i];
        for(size_t f=0;f<frames&&status==Status::Ok;++f) {
            if(mode=="rows") status=c::log_mel_frame(pcm.data(),pcm.size(),f,rows+f*c::kMelBins,&w);
            else status=c::log_mel_frame_ring(ring.data(),size-1,start,pcm.size(),f,rows+f*c::kMelBins,&w);
        }
        if(status==Status::Ok) status=c::normalize_features(rows,frames,&w);
    } else return 2;
    if(status!=Status::Ok) return 6;
    if(result.front()!=12345.0f||result.back()!=12345.0f) return 8;
    std::ofstream output(argv[2],std::ios::binary);
    output.write(reinterpret_cast<const char*>(rows),static_cast<std::streamsize>(frames*c::kMelBins*sizeof(float)));
    return output?0:9;
}
