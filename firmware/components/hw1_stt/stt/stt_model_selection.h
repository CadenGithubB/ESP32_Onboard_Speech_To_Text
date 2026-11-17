#pragma once

// Firmware callers resolve System_BuildConfig.h first. Standalone model tools
// keep the original 5x5 identity when no profile selection is supplied.
// HW1_STT_CITRINET is 1 when this build's acoustic model is Citrinet-384
// (ENABLE_STT_MODEL_CITRINET, the firmware default), 0 for QuartzNet.
#if defined(ENABLE_STT_MODEL_CITRINET) && ENABLE_STT_MODEL_CITRINET && \
    !(defined(ENABLE_STT_MODEL_QUARTZNET) && ENABLE_STT_MODEL_QUARTZNET)
#define HW1_STT_CITRINET 1
#include "stt_model_identity_citrinet384.h"
#ifndef HW1_STT_MODEL_PATH
#define HW1_STT_MODEL_PATH "/sd/STT Models/citrinet384-2000h.p4.stt"
#endif
#elif defined(ENABLE_STT_MODEL_15X5_GEN2) && ENABLE_STT_MODEL_15X5_GEN2
#define HW1_STT_CITRINET 0
#include "stt_model_identity_15x5_gen2.h"
#ifndef HW1_STT_MODEL_PATH
#define HW1_STT_MODEL_PATH "/sd/STT Models/quartznet15x5-gen2-25397.p4.stt"
#endif
#else
#define HW1_STT_CITRINET 0
#include "stt_model_identity.h"
#ifndef HW1_STT_MODEL_PATH
#define HW1_STT_MODEL_PATH "/STT Models/quartznet5x5.p4.stt"
#endif
#endif
