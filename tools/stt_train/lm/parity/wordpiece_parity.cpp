// Host harness for wordpiece_parity.py: decode int8 Citrinet outputs with stt_lm_wordpiece.
//   wordpiece_parity LM VOCAB CASES [CUSTOM_WORDS]
// CASES: one case per line, tab-separated: path frames stride exponent alpha beta beam prune
// (path: frames*stride int8 bytes; exponent in the ESP-DL sign). Prints one line per case:
// status \t text \t nodes \t candidates \t completions \t workspace bytes.
#include <cstring>
#include "stt_lm.h"
#include "stt_lm_wordpiece.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
using namespace hw1::stt::lm;

static std::vector<char> slurp(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    if(argc!=4&&argc!=5) { std::fprintf(stderr,"usage: wordpiece_parity LM VOCAB CASES [CUSTOM]\n"); return 2; }
    const std::vector<char> blob=slurp(argv[1]);
    Lm lm;
    const LoadStatus ls=lm.open(reinterpret_cast<const uint8_t*>(blob.data()),blob.size());
    if(ls!=LoadStatus::Ok) { std::fprintf(stderr,"LM: %s\n",describe(ls)); return 3; }
    const std::vector<char> vocab=slurp(argv[2]);
    auto pieces=std::make_unique<wordpiece::Pieces>();
    if(!pieces->parse(vocab.data(),vocab.size())) { std::fprintf(stderr,"vocab rejected\n"); return 4; }
    auto words=std::make_unique<CustomWords>();
    const CustomWords* custom=nullptr;
    if(argc==5) {
        const std::vector<char> list=slurp(argv[4]);
        words->parse(list.data(),list.size());
        custom=words.get();
    }
    std::ifstream cases(argv[3]);
    std::string line;
    std::vector<char> text(4096);
    while(std::getline(cases,line)) {
        if(line.rfind("JOIN\t",0)==0) {   // JOIN \t id id ... : the tokenizer join alone
            std::istringstream f(line.substr(5));
            std::vector<uint16_t> ids; unsigned id;
            while(f>>id)ids.push_back(uint16_t(id));
            const size_t n=pieces->join(ids.data(),ids.size(),text.data(),text.size());
            std::printf("%d\t%s\t0\t0\t0\t0\n",n==SIZE_MAX?4:0,text.data());
            continue;
        }
        std::istringstream f(line);
        std::string path; size_t frames, stride; int exponent; double alpha, beta, prune; size_t beam;
        std::getline(f,path,'\t');
        f>>frames>>stride>>exponent>>alpha>>beta>>beam>>prune;
        const std::vector<char> data=slurp(path.c_str());
        if(data.size()!=frames*stride) { std::printf("BADFILE\t\t0\t0\t0\t0\n"); continue; }
        wordpiece::Params p=wordpiece::fileParams(lm);
        p.alpha=alpha; p.beta=beta; p.beam=beam; p.prune=prune;
        DecodeStats st;
        const DecodeStatus s=wordpiece::decode(reinterpret_cast<const int8_t*>(data.data()),frames,stride,exponent,
                                              lm,*pieces,p,custom,text.data(),text.size(),{},&st);
        // The same decode in memory the caller lends (an odd address, old content): the same result, no heap
        // workspace; and with too little lent, the heap again.
        if(s!=DecodeStatus::InvalidArgument) {
            std::vector<char> again(text.size());
            for(const size_t lend : {st.workspaceBytes+64, size_t(1000)}) {
                std::vector<uint8_t> scratch(lend+1,0xa5);
                DecodeStats st2;
                const DecodeStatus s2=wordpiece::decode(reinterpret_cast<const int8_t*>(data.data()),frames,stride,
                                                       exponent,lm,*pieces,p,custom,again.data(),again.size(),{},&st2,
                                                       scratch.data()+1,lend);
                if(s2!=s || std::strcmp(again.data(),text.data()) || st2.nodes!=st.nodes || st2.candidates!=st.candidates
                   || st2.completions!=st.completions || st2.workspaceBytes!=st.workspaceBytes
                   || st2.workspaceLent!=(lend>1000) || st.workspaceLent) {
                    std::fprintf(stderr,"lent workspace (%zu bytes) changed the decode of %s\n",lend,path.c_str());
                    return 5;
                }
            }
        }
        std::printf("%d\t%s\t%u\t%u\t%u\t%zu\n",int(s),text.data(),st.nodes,st.candidates,st.completions,st.workspaceBytes);
    }
    return 0;
}
