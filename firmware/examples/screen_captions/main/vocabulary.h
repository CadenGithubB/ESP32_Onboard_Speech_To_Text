#pragma once
#include <cstddef>

#include "stt/stt_lm.h"

// Your own words. Each line of "STT Models/user_vocab.tsv" on the card is a word the way it
// should be shown, or SHOWN<TAB>spoken when it is said differently from how it is spelled:
//   Kubernetes
//   ESP32<TAB>esp
// The recognizer leans towards these words, and captions show them with your spelling.
// It is a preference, not a guarantee. The file is optional.
namespace vocabulary {
size_t load();                                  // words read; 0 with no file or a bad one
const hw1::stt::lm::CustomWords* words();       // for the decoder; nullptr when there are none
// The caption as shown: your spellings, a capital at the start and on "I". False if it does not fit.
bool format(const char* raw, char* shown, size_t capacity);
}  // namespace vocabulary
