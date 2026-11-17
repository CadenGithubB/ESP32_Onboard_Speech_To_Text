#pragma once
// Citrinet build only (ENABLE_STT_MODEL_CITRINET): the runtime API itself is quartznet_runtime.h, shared by
// both models' runtimes.
#include "stt_lm_wordpiece.h"
namespace hw1::stt {
// The WordPiece vocabulary compiled into the firmware (citrinet_vocab.h), parsed into PSRAM on the first
// transcribe() call and kept: the greedy fallback and the LM decoder (System_STTLocal.cpp) share it.
// nullptr until then; a decoder called from transcribe() always sees it set.
const lm::wordpiece::Pieces* citrinetPieces();
}
