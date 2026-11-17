#pragma once
#include <cstddef>
#include <cstdint>
#include "stt_lm.h"
// CTC prefix beam search over WordPiece tokens with the HW1LM1 word LM, for Citrinet. The reference is
// tools/stt_train/lm/hw1lm_wordpiece.py; the parity test (tools/stt_train/lm/parity/wordpiece_parity.py)
// requires identical texts on the same int8 outputs. Same search as stt_lm.h's decode with tokens for
// characters:
//  * a prefix is a token sequence; blank / repeat collapse is per token;
//  * a word-start piece (no "##") after a non-empty word completes that word: alpha*ln10*log10P(word |
//    context) + beta (+ hotword bonus), as a space does in the character decoder; "##" pieces extend it;
//  * the apostrophe token joins the word before it and the piece after it ("it ' s" is the word "it's");
//  * special tokens ([PAD] [UNK] [CLS] [SEP] [MASK]) are never emitted;
//  * prune applies per token (log-probability >= prune), the repeat of the last token is always kept;
//  * beams are cut to the width by (score, token sequence) as the reference sorts, kept in creation order
//    when they fit; the open word is completed and </s> scored at the end; each decode starts at <s>.
// The Lm view and CustomWords come from stt_lm.h and are never modified. No filesystem or logging; the
// workspace is one allocation (PSRAM on the P4).
namespace hw1::stt::lm::wordpiece {
// An open word longer than kMaxWordBytes is treated as out of vocabulary and never a hotword: exact for LMs
// from build_lm.py, which keeps words of at most 31 bytes (hw1lm.MAX_CUSTOM_BYTES), as custom words are.
inline constexpr size_t kMaxTokens=1024, kMaxPoolBytes=8192, kMaxWordBytes=31;
enum class Kind : uint8_t { Special, Apostrophe, Start, Continuation };

// The model's vocab.txt: one token per line ("\n" or "\r\n"); the CTC blank is index size().
class Pieces {
 public:
    bool parse(const char* data, size_t bytes);    // false (and empty) on a malformed or oversized list
    size_t size() const { return count_; }
    size_t blank() const { return count_; }
    Kind kind(size_t id) const { return kind_[id]; }
    // The piece's text without "##" (empty for specials).
    const char* text(size_t id, size_t& length) const {
        length=size_t(offset_[id+1]-offset_[id]); return pool_+offset_[id];
    }
    // The tokenizer's decode: a space before word starts, "##" joined, apostrophes glued on both sides,
    // specials dropped. Returns the text length; NUL-terminated, cut at capacity-1 (returns SIZE_MAX then).
    size_t join(const uint16_t* ids, size_t count, char* out, size_t capacity) const;
 private:
    Kind kind_[kMaxTokens]{};
    uint16_t offset_[kMaxTokens+1]{};
    char pool_[kMaxPoolBytes]{};
    size_t count_=0;
};

struct Params {
    double alpha, beta, unkLog10, prune, hotwordBonus;
    size_t beam;
};
// The LM file header's decoder parameters (QuartzNet's tuning); Citrinet's tuned values are passed by the caller.
Params fileParams(const Lm& lm);

// Bytes of the single workspace allocation decode() makes for this shape (0 if out of range).
size_t workspace_bytes(size_t frames, size_t beam, size_t classes);
// logits: int8 model output, `frames` rows of `stride` bytes, the first pieces.size()+1 used (blank last);
// value = q * 2^exponent (ESP-DL sign). Per-frame log_softmax is applied here. Text is the best prefix's
// join, NUL-terminated even on failure (OutputTooSmall keeps the leading capacity-1 bytes).
// scratch: memory the caller lends for this call (any alignment, any content; nullptr for none), e.g. an
// acoustic model's activation arena, idle while its output is decoded. The exp table and the workspace are
// taken from it when they fit (stats->workspaceLent), from the heap otherwise; the text is the same either way.
// logits must not lie inside it.
DecodeStatus decode(const int8_t* logits, size_t frames, size_t stride, int exponent, const Lm& lm,
                    const Pieces& pieces, const Params& params, const CustomWords* words, char* text,
                    size_t capacity, const DecodeControl& control={}, DecodeStats* stats=nullptr,
                    void* scratch=nullptr, size_t scratchBytes=0);
}
