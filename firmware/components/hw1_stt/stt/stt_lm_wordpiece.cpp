#include "stt_lm_wordpiece.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
// Built with -ffp-contract=off (as stt_lm.cpp): the reference never fuses a multiply-add.
namespace hw1::stt::lm::wordpiece {
namespace {
constexpr double kNegInf=-std::numeric_limits<double>::infinity();
constexpr uint32_t kNone=0xffffffffu;
constexpr uint16_t kRootToken=0xffff;
constexpr uint8_t kOverflow=0xff;          // a word longer than kMaxWordBytes: never an LM or custom word
constexpr size_t kPollFrames=32;
static_assert(kMaxWordBytes<kOverflow, "word length marker");

void* allocate(size_t bytes) {
#if defined(ESP_PLATFORM)
    return heap_caps_aligned_alloc(16,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
    return ::operator new(bytes,std::nothrow);
#endif
}
void release(void* p) {
#if defined(ESP_PLATFORM)
    heap_caps_free(p);
#else
    ::operator delete(p);
#endif
}
// hw1lm._lse, operation for operation.
double lse(double a, double b) {
    if(a==kNegInf)return b;
    if(b==kNegInf)return a;
    if(a>b)return a+std::log1p(std::exp(b-a));
    return b+std::log1p(std::exp(a-b));
}
size_t pow2(size_t n) { size_t p=1; while(p<n)p<<=1; return p; }
uint32_t slotOf(uint32_t parent, uint16_t token, size_t mask) {
    return uint32_t(((parent*0x9e3779b1u)^(uint32_t(token)*0x85ebca77u)^(parent>>15))&mask);
}
// A kept prefix. Every token sequence has exactly one node for the whole decode (the trie index below),
// so a prefix reached again later, or through another beam, is the same key, as the reference's tuples are.
struct Node {
    double lm;                  // accumulated alpha*ln P + beta (+ bonus) of the completed words
    uint32_t parent;
    uint16_t token, depth, w2, w1;
    uint8_t length;             // bytes of the open word, kOverflow when too long
    bool glue;                  // the last token was an apostrophe: the next piece joins the word
    bool completed;             // completion below is filled (every word start after this prefix needs it)
    double completionDelta;     // alpha*ln10*log10P(word | w2, w1) + beta (+ bonus) of the open word
    uint16_t completionW2, completionW1;
    char word[kMaxWordBytes+1];
};
// One prefix of the frame being built (the reference's nxt entries).
struct Cand {
    double pb, pnb, lm, total;
    uint32_t parent, node;      // node: its existing Node, or kNone until it is kept
    uint16_t token, w2, w1;
    uint8_t length;
    bool glue;
};
struct BeamEntry { double pb, pnb; uint32_t node; };

class Search {
 public:
    Search(const Lm& lm, const Pieces& pieces, const Params& p, const CustomWords* words)
        : lm_(lm), pieces_(pieces), p_(p), words_(words), alphaLn_(p.alpha*std::log(10.0)) {}
    void layout(size_t frames, size_t classes, size_t maxActive, uint8_t* base, size_t* bytes);
    DecodeStatus run(const int8_t* logits, size_t frames, size_t stride, int exponent, const double* exps,
                     char* text, size_t capacity, const DecodeControl& control, DecodeStats* stats);
 private:
    struct Completion { double delta; uint16_t w2, w1; };
    Completion complete(const char* word, uint8_t length, uint16_t w2, uint16_t w1);
    Completion completeNode(uint32_t node);   // complete() of the node's open word, computed once per node
    // The open word after appending token c to prefix p (reference extend()): returns true when c first
    // completes p's word (a word start after an open, unglued word). word may be nullptr (length only).
    bool extendWord(const Node& p, uint16_t c, char* word, uint8_t& length, bool& glue) const;
    uint32_t own(uint32_t node);
    uint32_t child(uint32_t parent, uint16_t token);
    uint32_t findCand(uint32_t parent, uint16_t token, uint32_t& slot) const;
    uint32_t findNode(uint32_t parent, uint16_t token) const;
    size_t path(uint32_t parent, uint16_t token, uint16_t* out) const;
    bool before(uint32_t a, uint32_t b);        // reference order: higher total first, then the smaller tuple
    void logSoftmax(const int8_t* row);
    double lp(size_t c);                      // this frame's log-probability of class c

    const Lm& lm_;
    const Pieces& pieces_;
    const Params& p_;
    const CustomWords* words_;
    const double alphaLn_;
    size_t classes_=0, beam_=0, maxNodes_=0, maxCands_=0, frames_=0;
    Node* nodes_=nullptr; uint32_t nodeCount_=0;
    uint32_t* trie_=nullptr; size_t trieMask_=0;
    Cand* cands_=nullptr; uint32_t candCount_=0;
    uint32_t* candIndex_=nullptr; size_t candMask_=0;
    uint32_t* candSlot_=nullptr;              // each cand's slot in candIndex_, for clearing
    uint32_t* order_=nullptr;
    BeamEntry* beams_=nullptr; BeamEntry* next_=nullptr; size_t beamCount_=0;
    uint16_t* active_=nullptr; size_t activeCount_=0;
    const double* exps_=nullptr;               // exp(-d * 2^exponent), d = 0..255 (FrameTable)
    double scale_=0, logSum_=0;
    const int8_t* row_=nullptr; int qmax_=0;
    double* lpOf_=nullptr; uint32_t* lpStamp_=nullptr; uint32_t frameStamp_=0;   // lp by d, filled on demand
    uint16_t* pathA_=nullptr; uint16_t* pathB_=nullptr;
    DecodeStats local_{};
};

// Workspace layout, one allocation; base nullptr only measures.
size_t layoutBytes(size_t frames, size_t beam, size_t classes, size_t maxActive) {
    const size_t nodes=frames*beam+1, cands=beam*(maxActive+2);
    size_t at=0;
    auto take=[&](size_t n, size_t align) { at=(at+align-1)&~(align-1); at+=n; };
    take(sizeof(Node)*nodes,16); take(4*pow2(2*nodes),4); take(sizeof(Cand)*cands,16); take(4*pow2(2*cands),4);
    take(4*cands,4); take(4*cands,4); take(sizeof(BeamEntry)*beam,16); take(sizeof(BeamEntry)*beam,16);
    take(8*256,16); take(4*256,4); take(2*classes,2); take(2*(frames+2),2); take(2*(frames+2),2);
    return at;
}
void Search::layout(size_t frames, size_t classes, size_t maxActive, uint8_t* base, size_t* bytes) {
    classes_=classes; beam_=p_.beam; frames_=frames;
    maxNodes_=frames*beam_+1;
    maxCands_=beam_*(maxActive+2);
    const size_t trieSize=pow2(2*maxNodes_), candSize=pow2(2*maxCands_);
    size_t at=0;
    auto take=[&](size_t n, size_t align) -> uint8_t* {
        at=(at+align-1)&~(align-1);
        uint8_t* r=base?base+at:nullptr; at+=n; return r;
    };
    nodes_=reinterpret_cast<Node*>(take(sizeof(Node)*maxNodes_,16));
    trie_=reinterpret_cast<uint32_t*>(take(4*trieSize,4));
    cands_=reinterpret_cast<Cand*>(take(sizeof(Cand)*maxCands_,16));
    candIndex_=reinterpret_cast<uint32_t*>(take(4*candSize,4));
    candSlot_=reinterpret_cast<uint32_t*>(take(4*maxCands_,4));
    order_=reinterpret_cast<uint32_t*>(take(4*maxCands_,4));
    beams_=reinterpret_cast<BeamEntry*>(take(sizeof(BeamEntry)*beam_,16));
    next_=reinterpret_cast<BeamEntry*>(take(sizeof(BeamEntry)*beam_,16));
    lpOf_=reinterpret_cast<double*>(take(8*256,16));
    lpStamp_=reinterpret_cast<uint32_t*>(take(4*256,4));
    active_=reinterpret_cast<uint16_t*>(take(2*classes,2));
    pathA_=reinterpret_cast<uint16_t*>(take(2*(frames+2),2));
    pathB_=reinterpret_cast<uint16_t*>(take(2*(frames+2),2));
    trieMask_=trieSize-1; candMask_=candSize-1;
    *bytes=at;
}

Search::Completion Search::complete(const char* word, uint8_t length, uint16_t w2, uint16_t w1) {
    ++local_.completions;
    const bool overflow=length==kOverflow;
    const int id=overflow?-1:lm_.find(word,length);
    double l10; uint16_t wid;
    if(id<0||uint16_t(id)==lm_.bos()||uint16_t(id)==lm_.eos()||uint16_t(id)==lm_.unk()) { l10=p_.unkLog10; wid=lm_.unk(); }
    else { wid=uint16_t(id); l10=lm_.log10(w2,w1,wid); }
    const bool listed=words_&&!overflow&&words_->contains(word,length);
    const double delta=alphaLn_*l10+p_.beta+(listed?p_.hotwordBonus:0.0);
    return {delta,w1,wid};
}

Search::Completion Search::completeNode(uint32_t id) {
    Node& n=nodes_[id];
    if(!n.completed) {
        const Completion d=complete(n.word,n.length,n.w2,n.w1);
        n.completionDelta=d.delta; n.completionW2=d.w2; n.completionW1=d.w1; n.completed=true;
    }
    return {n.completionDelta,n.completionW2,n.completionW1};
}

bool Search::extendWord(const Node& p, uint16_t c, char* word, uint8_t& length, bool& glue) const {
    const Kind k=pieces_.kind(c);
    size_t n=0; const char* t=pieces_.text(c,n);
    if(k==Kind::Apostrophe) { t="'"; n=1; }
    const bool completes=k==Kind::Start&&!p.glue&&p.length!=0;
    glue=k==Kind::Apostrophe;
    const uint8_t from=completes?0:p.length;
    if(from==kOverflow||size_t(from)+n>kMaxWordBytes) { length=kOverflow; if(word)word[0]='\0'; return completes; }
    if(word) {
        if(from)std::memcpy(word,p.word,from);
        std::memcpy(word+from,t,n); word[from+n]='\0';
    }
    length=uint8_t(from+n);
    return completes;
}

uint32_t Search::findNode(uint32_t parent, uint16_t token) const {
    for(size_t s=slotOf(parent,token,trieMask_);;s=(s+1)&trieMask_) {
        const uint32_t n=trie_[s];
        if(n==kNone)return kNone;
        if(nodes_[n].parent==parent&&nodes_[n].token==token)return n;
    }
}
uint32_t Search::findCand(uint32_t parent, uint16_t token, uint32_t& slot) const {
    for(size_t s=slotOf(parent,token,candMask_);;s=(s+1)&candMask_) {
        const uint32_t i=candIndex_[s];
        if(i==kNone) { slot=uint32_t(s); return kNone; }
        if(cands_[i].parent==parent&&cands_[i].token==token) { slot=uint32_t(s); return i; }
    }
}
// The beam's own prefix in this frame (reference: nxt.get(prefix) or a new entry with its state).
uint32_t Search::own(uint32_t node) {
    const Node& b=nodes_[node];
    uint32_t slot;
    const uint32_t found=findCand(b.parent,b.token,slot);
    if(found!=kNone)return found;
    const uint32_t i=candCount_++;
    Cand& c=cands_[i];
    c.pb=kNegInf; c.pnb=kNegInf; c.lm=b.lm; c.parent=b.parent; c.node=node; c.token=b.token;
    c.w2=b.w2; c.w1=b.w1; c.length=b.length; c.glue=b.glue;
    candIndex_[slot]=i; candSlot_[i]=slot;
    return i;
}
// parent + token in this frame (reference: nxt.get(prefix + (c,)) or a new entry with extend()).
uint32_t Search::child(uint32_t parent, uint16_t token) {
    uint32_t slot;
    const uint32_t found=findCand(parent,token,slot);
    if(found!=kNone)return found;
    const uint32_t i=candCount_++;
    Cand& c=cands_[i];
    c.pb=kNegInf; c.pnb=kNegInf; c.parent=parent; c.token=token;
    const uint32_t existing=findNode(parent,token);
    if(existing!=kNone) {
        const Node& n=nodes_[existing];
        c.node=existing; c.lm=n.lm; c.w2=n.w2; c.w1=n.w1; c.length=n.length; c.glue=n.glue;
    } else {
        // reference extend(): a completed word adds its LM delta and moves the context
        const Node& p=nodes_[parent];
        c.node=kNone; c.lm=p.lm; c.w2=p.w2; c.w1=p.w1;
        if(extendWord(p,token,nullptr,c.length,c.glue)) {
            const Completion d=completeNode(parent);
            c.lm=p.lm+d.delta; c.w2=d.w2; c.w1=d.w1;
        }
    }
    candIndex_[slot]=i; candSlot_[i]=slot;
    ++local_.candidates;
    return i;
}

size_t Search::path(uint32_t parent, uint16_t token, uint16_t* out) const {
    // tokens of the sequence parent's tokens + token (token kRootToken: the parent itself)
    size_t depth=parent==kNone?0:nodes_[parent].depth;
    size_t n=depth+(token==kRootToken?0:1);
    if(token!=kRootToken)out[n-1]=token;
    for(uint32_t at=parent;at!=kNone&&nodes_[at].token!=kRootToken;at=nodes_[at].parent)
        out[--depth]=nodes_[at].token;
    return n;
}

bool Search::before(uint32_t a, uint32_t b) {
    const Cand& x=cands_[a]; const Cand& y=cands_[b];
    if(x.total!=y.total)return x.total>y.total;
    // equal totals: the smaller token tuple first (Python tuple order)
    const size_t na=path(x.parent,x.token,pathA_), nb=path(y.parent,y.token,pathB_);
    for(size_t i=0;i<na&&i<nb;++i) if(pathA_[i]!=pathB_[i])return pathA_[i]<pathB_[i];
    return na<nb;
}

// The reference's log_softmax (log_softmax_int8): lp(c) = double(q_c - qmax)*2^exponent - log(sum over classes,
// in class order, of exp(double(q - qmax)*2^exponent)). The exp values come from a table of the same
// expression (q - qmax takes 256 values); lp(c) depends only on d = qmax - q_c, so it is formed once per
// distinct d when first needed. It decreases strictly with d (the product is exact, subtraction of the same
// logSum rounds monotonically), so the prune set is every class whose d is at most the last d that passes.
void Search::logSoftmax(const int8_t* row) {
    row_=row;
    int qmax=-128;
    for(size_t i=0;i<classes_;++i) qmax=std::max(qmax,int(row[i]));
    qmax_=qmax;
    double sum=0;
    for(size_t i=0;i<classes_;++i) sum+=exps_[qmax-int(row[i])];
    logSum_=std::log(sum);
    ++frameStamp_;
    int cut=-1;
    for(int d=0;d<256;++d) {
        lpOf_[d]=double(-d)*scale_-logSum_; lpStamp_[d]=frameStamp_;
        if(!(lpOf_[d]>=p_.prune))break;
        cut=d;
    }
    activeCount_=0;
    const size_t blank=pieces_.blank();
    for(size_t c=0;c<blank;++c)
        if(qmax-int(row[c])<=cut&&pieces_.kind(c)!=Kind::Special)active_[activeCount_++]=uint16_t(c);
}
double Search::lp(size_t c) {
    const int d=qmax_-int(row_[c]);
    if(lpStamp_[d]!=frameStamp_) { lpOf_[d]=double(-d)*scale_-logSum_; lpStamp_[d]=frameStamp_; }
    return lpOf_[d];
}

DecodeStatus Search::run(const int8_t* logits, size_t frames, size_t stride, int exponent, const double* exps,
                         char* text, size_t capacity, const DecodeControl& control, DecodeStats* stats) {
    exps_=exps; scale_=std::ldexp(1.0,exponent);
    for(size_t d=0;d<256;++d)lpStamp_[d]=0;
    frameStamp_=0;
    for(size_t s=0;s<=trieMask_;++s)trie_[s]=kNone;
    for(size_t s=0;s<=candMask_;++s)candIndex_[s]=kNone;
    Node& root=nodes_[0];
    root.lm=0; root.parent=kNone; root.token=kRootToken; root.depth=0;
    root.w2=kNoWord; root.w1=lm_.bos(); root.length=0; root.glue=false; root.word[0]='\0'; root.completed=false;
    nodeCount_=1;
    beams_[0]={0.0,kNegInf,0}; beamCount_=1;
    local_.frames=uint32_t(frames); local_.beam=uint32_t(beam_);
    local_.startHistoryWords=0;
    const size_t blank=pieces_.blank();
    for(size_t t=0;t<frames;++t) {
        if(t%kPollFrames==0&&t) {
            if(control.cancelled&&control.cancelled(control.context))return DecodeStatus::Cancelled;
#if defined(ESP_PLATFORM)
            taskYIELD();
#endif
        }
        logSoftmax(logits+t*stride);
        for(uint32_t i=0;i<candCount_;++i)candIndex_[candSlot_[i]]=kNone;
        candCount_=0;
        const double lpBlank=lp(blank);
        for(size_t bi=0;bi<beamCount_;++bi) {
            const BeamEntry b=beams_[bi];
            const double tot=lse(b.pb,b.pnb);
            const uint32_t e=own(b.node);
            cands_[e].pb=lse(cands_[e].pb,tot+lpBlank);
            const int last=b.node==0?-1:int(nodes_[b.node].token);
            if(last>=0) {
                const double lc=lp(size_t(last));
                cands_[e].pnb=lse(cands_[e].pnb,b.pnb+lc);
                if(b.pb!=kNegInf) {
                    const uint32_t f=child(b.node,uint16_t(last));
                    cands_[f].pnb=lse(cands_[f].pnb,b.pb+lc);
                }
            }
            if(tot==kNegInf)continue;
            for(size_t a=0;a<activeCount_;++a) {
                const uint16_t c=active_[a];
                if(int(c)==last)continue;
                const uint32_t f=child(b.node,c);
                cands_[f].pnb=lse(cands_[f].pnb,tot+lp(c));
            }
        }
        size_t kept=0;
        for(uint32_t i=0;i<candCount_;++i) {
            const double s=lse(cands_[i].pb,cands_[i].pnb);
            if(s==kNegInf)continue;
            cands_[i].total=s+cands_[i].lm;
            order_[kept++]=i;
        }
        if(kept>beam_) {
            // (score, token sequence) is a strict total order: partial_sort keeps exactly sort's first beam_
            std::partial_sort(order_,order_+beam_,order_+kept,[this](uint32_t a, uint32_t b) { return before(a,b); });
            kept=beam_;
        }
        for(size_t k=0;k<kept;++k) {
            Cand& c=cands_[order_[k]];
            if(c.node==kNone) {   // a new prefix: its node, with the word bytes rebuilt from the parent
                if(nodeCount_>=maxNodes_)return DecodeStatus::OutOfMemory;
                const Node& p=nodes_[c.parent];
                Node& n=nodes_[nodeCount_];
                n.lm=c.lm; n.parent=c.parent; n.token=c.token; n.depth=uint16_t(p.depth+1);
                n.w2=c.w2; n.w1=c.w1; n.completed=false;
                extendWord(p,c.token,n.word,n.length,n.glue);
                const uint32_t id=nodeCount_++;
                for(size_t s=slotOf(c.parent,c.token,trieMask_);;s=(s+1)&trieMask_)
                    if(trie_[s]==kNone) { trie_[s]=id; break; }
                c.node=id;
            }
            next_[k]={c.pb,c.pnb,c.node};
        }
        std::swap(beams_,next_); beamCount_=kept;
    }
    // Final pass: complete the open word, add </s>, best (score, tuple).
    double best=kNegInf; uint32_t bestNode=0; bool any=false;
    for(size_t bi=0;bi<beamCount_;++bi) {
        const BeamEntry& b=beams_[bi];
        const Node& n=nodes_[b.node];
        const double tot=lse(b.pb,b.pnb);
        double lms=n.lm; uint16_t w2=n.w2, w1=n.w1;
        if(n.length!=0) {
            const Completion d=completeNode(b.node);
            lms+=d.delta; w2=d.w2; w1=d.w1;
        }
        lms+=alphaLn_*lm_.log10(w2,w1,lm_.eos());
        const double score=tot+lms;
        bool better=!any||score>best;
        if(any&&score==best) {
            const size_t na=path(b.node,kRootToken,pathA_), nb=path(bestNode,kRootToken,pathB_);
            size_t i=0; while(i<na&&i<nb&&pathA_[i]==pathB_[i])++i;
            better=i<na&&i<nb?pathA_[i]<pathB_[i]:na<nb;
        }
        if(better) { best=score; bestNode=b.node; any=true; }
    }
    local_.nodes=nodeCount_;
    local_.endHistoryWords=0;
    const size_t n=path(bestNode,kRootToken,pathA_);
    const size_t written=pieces_.join(pathA_,n,text,capacity);
    if(stats)*stats=local_;
    return written==SIZE_MAX?DecodeStatus::OutputTooSmall:DecodeStatus::Ok;
}
}

bool Pieces::parse(const char* data, size_t bytes) {
    count_=0; offset_[0]=0;
    size_t used=0, at=0;
    while(at<bytes) {
        size_t end=at; while(end<bytes&&data[end]!='\n')++end;
        size_t len=end-at;
        if(len&&data[at+len-1]=='\r')--len;
        const char* t=data+at;
        at=end<bytes?end+1:end;
        if(len==0||count_>=kMaxTokens) { count_=0; return false; }
        Kind k; size_t skip=0;
        bool special=len>=3&&t[0]=='['&&t[len-1]==']';
        for(size_t i=1;special&&i+1<len;++i) special=t[i]>='A'&&t[i]<='Z';
        if(special) { k=Kind::Special; skip=len; }
        else if(len==1&&t[0]=='\'') k=Kind::Apostrophe;
        else if(len>=2&&t[0]=='#'&&t[1]=='#') { k=Kind::Continuation; skip=2; }
        else k=Kind::Start;
        const size_t n=len-skip;
        if(used+n>kMaxPoolBytes) { count_=0; return false; }
        std::memcpy(pool_+used,t+skip,n); used+=n;
        kind_[count_]=k; offset_[count_+1]=uint16_t(used); ++count_;
    }
    return count_>0;
}

size_t Pieces::join(const uint16_t* ids, size_t count, char* out, size_t capacity) const {
    if(!out||!capacity)return SIZE_MAX;
    size_t n=0; bool glue=false, cut=false;
    auto put=[&](const char* s, size_t len) {
        for(size_t i=0;i<len;++i) { if(n+1<capacity)out[n++]=s[i]; else cut=true; }
    };
    for(size_t i=0;i<count;++i) {
        const uint16_t id=ids[i];
        if(id>=count_)continue;
        size_t len=0; const char* t=text(id,len);
        switch(kind_[id]) {
            case Kind::Special: break;
            // text after the apostrophe ends its glue (the reference's regex eats only adjacent spaces)
            case Kind::Continuation: put(t,len); if(len)glue=false; break;
            case Kind::Apostrophe: put("'",1); glue=true; break;
            case Kind::Start:
                if(n&&!glue)put(" ",1);
                put(t,len); glue=false; break;
        }
    }
    out[n]='\0';
    return cut?SIZE_MAX:n;
}

Params fileParams(const Lm& lm) {
    return {double(lm.alpha()),double(lm.beta()),double(lm.unkLog10()),double(lm.charPrune()),
            double(lm.hotwordBonus()),size_t(lm.beamWidth())};
}

size_t workspace_bytes(size_t frames, size_t beam, size_t classes) {
    if(!beam||beam>kMaxBeam||frames>kMaxFrames||classes<2||classes>kMaxTokens+1)return 0;
    return layoutBytes(frames,beam,classes,classes);   // worst case: every non-blank token a candidate
}

DecodeStatus decode(const int8_t* logits, size_t frames, size_t stride, int exponent, const Lm& lm,
                    const Pieces& pieces, const Params& params, const CustomWords* words, char* text,
                    size_t capacity, const DecodeControl& control, DecodeStats* stats, void* scratch,
                    size_t scratchBytes) {
    if(text&&capacity)text[0]='\0';
    const size_t classes=pieces.size()+1;
    if(!text||!capacity||!lm.valid()||!pieces.size()||(frames&&!logits)||stride<classes||frames>kMaxFrames||
       !params.beam||params.beam>kMaxBeam||!std::isfinite(params.alpha)||!std::isfinite(params.beta)||
       !std::isfinite(params.unkLog10)||std::isnan(params.prune)||!std::isfinite(params.hotwordBonus)||
       exponent<-30||exponent>30)
        return DecodeStatus::InvalidArgument;
    // The caller's scratch memory, from its first 16-byte boundary, in 16-byte steps.
    uint8_t* lent=static_cast<uint8_t*>(scratch);
    size_t lentBytes=lent ? scratchBytes : 0;
    if(lent) {
        const size_t skip=(16-(reinterpret_cast<uintptr_t>(lent)&15))&15;
        if(skip>=lentBytes)lentBytes=0; else { lent+=skip; lentBytes-=skip; }
    }
    auto borrow=[&](size_t bytes) -> uint8_t* {
        bytes=(bytes+15)&~size_t(15);
        if(bytes>lentBytes)return nullptr;
        uint8_t* p=lent; lent+=bytes; lentBytes-=bytes; return p;
    };
    // exp(-d * 2^exponent) for every d = qmax - q (0..255): the only values the per-frame log_softmax uses.
    // Lent memory or its own small PSRAM block (2 KiB), never the caller's stack.
    uint8_t* const lentExps=borrow(256*sizeof(double));
    double* exps=lentExps ? reinterpret_cast<double*>(lentExps) : static_cast<double*>(allocate(256*sizeof(double)));
    if(!exps)return DecodeStatus::OutOfMemory;
    const double scale=std::ldexp(1.0,exponent);
    for(int d=0;d<256;++d) exps[d]=std::exp(double(-d)*scale);
    // Pre-pass: the largest per-frame candidate count sizes the frame buffers (prune is per frame). Same
    // arithmetic as Search::logSoftmax: the class-order sum, then the monotone threshold on d.
    size_t maxActive=0;
    for(size_t t=0;t<frames;++t) {
        const int8_t* row=logits+t*stride;
        int qmax=-128;
        for(size_t i=0;i<classes;++i) qmax=std::max(qmax,int(row[i]));
        double sum=0;
        for(size_t i=0;i<classes;++i) sum+=exps[qmax-int(row[i])];
        const double logSum=std::log(sum);
        int cut=-1;
        for(int d=0;d<256&&double(-d)*scale-logSum>=params.prune;++d) cut=d;
        size_t active=0;
        for(size_t c=0;c+1<classes;++c)
            if(qmax-int(row[c])<=cut&&pieces.kind(c)!=Kind::Special)++active;
        maxActive=std::max(maxActive,active);
    }
    Search search(lm,pieces,params,words);
    size_t bytes=0;
    search.layout(frames,classes,maxActive,nullptr,&bytes);
    uint8_t* const lentBase=borrow(bytes);
    uint8_t* base=lentBase ? lentBase : static_cast<uint8_t*>(allocate(bytes));
    if(!base) { if(!lentExps)release(exps); return DecodeStatus::OutOfMemory; }
    search.layout(frames,classes,maxActive,base,&bytes);
    const DecodeStatus status=search.run(logits,frames,stride,exponent,exps,text,capacity,control,stats);
    if(stats) { stats->workspaceBytes=bytes+256*sizeof(double); stats->workspaceLent=lentBase!=nullptr; }
    if(!lentBase)release(base);
    if(!lentExps)release(exps);
    return status;
}
}
