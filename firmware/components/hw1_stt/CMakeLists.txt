# HardwareOne's on-device speech-to-text engine as an ESP-IDF component (ESP32-P4 only).
#
# stt/ began as HardwareOne's components/hardwareone/stt (stt-speedups, 2e56063).
# Runtime/memory/LM/speaker and SDK improvements were reconciled on October 31;
# the pinned model and platform adapters remain standalone. See docs/STANDALONE_PORT.md.
# The three headers at this level stand in for the firmware's own:
#   System_STTLocal.h   the engine-facing control/stats types
#   Audio_VadPolicy.h   the adaptive VAD gate the segmenter uses (copied as is)
#   System_MemUtil.h    explicit PSRAM allocation (ps_alloc/ps_free), reimplemented
# It needs espressif/esp-dl 3.3.12 with ../../tools/esp-dl-3.3.12-hw1.patch applied
# (../../tools/prepare_esp_dl.py stages it; idf_component.yml points at the result).
if(NOT IDF_TARGET STREQUAL "esp32p4")
    idf_component_register()
    return()
endif()

# One acoustic model per build: Citrinet-384 (the default) or QuartzNet 15x5. The other's
# runtime, front end and ESP-DL kernels are not compiled. Set before project() or with -D.
set(HW1_STT_MODEL "citrinet" CACHE STRING "Acoustic model: citrinet or quartznet")
if(NOT HW1_STT_MODEL STREQUAL "citrinet" AND NOT HW1_STT_MODEL STREQUAL "quartznet")
    message(FATAL_ERROR "hw1_stt: HW1_STT_MODEL must be citrinet or quartznet, not '${HW1_STT_MODEL}'")
endif()
option(HW1_STT_SPEAKER_LABELS "Also compile the speaker-label model (stt/speaker_model.h)" OFF)
option(HW1_OVERLAP_GATE "Also compile the overlap-gate model (stt/overlap_gate.h)" OFF)

if(HW1_STT_MODEL STREQUAL "citrinet")
    # The word-piece decoder uses stt_lm.cpp's model view and custom-word list.
    set(hw1_stt_model_srcs stt/citrinet_runtime.cpp stt/citrinet_frontend.cpp stt/stt_lm_wordpiece.cpp)
else()
    set(hw1_stt_model_srcs stt/quartznet_runtime.cpp)
endif()
set(hw1_stt_srcs
    ${hw1_stt_model_srcs}
    stt/quartznet_frontend.cpp
    stt/stt_lm.cpp
    stt/stt_dl_metadata.cpp
    stt/stt_internal_memory.cpp
    stt/stt_stream.cpp
    System_MemUtil.cpp)
if(HW1_STT_SPEAKER_LABELS)
    list(APPEND hw1_stt_srcs stt/speaker_frontend.cpp stt/speaker_model.cpp)
endif()
if(HW1_OVERLAP_GATE)
    list(APPEND hw1_stt_srcs stt/overlap_gate.cpp)
endif()

idf_component_register(SRCS ${hw1_stt_srcs}
    INCLUDE_DIRS "." "stt"
    REQUIRES espressif__esp-dl esp_rom mbedtls esp_timer esp_hw_support esp_psram)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
# Same per-file options as HardwareOne: the frontend must not fuse multiply-adds
# (its features are hash-pinned against the host), inference code is built for speed.
set_source_files_properties(stt/quartznet_frontend.cpp stt/citrinet_frontend.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-O2")
set_source_files_properties(stt/quartznet_runtime.cpp stt/citrinet_runtime.cpp stt/stt_lm.cpp PROPERTIES COMPILE_OPTIONS "-O2")
# The word-piece decoder keeps the reference's arithmetic: no fused multiply-adds either.
set_source_files_properties(stt/stt_lm_wordpiece.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-O2")
if(HW1_STT_SPEAKER_LABELS)
    set_source_files_properties(stt/speaker_frontend.cpp stt/speaker_model.cpp PROPERTIES COMPILE_OPTIONS "-O2")
endif()
if(HW1_OVERLAP_GATE)
    set_source_files_properties(stt/overlap_gate.cpp PROPERTIES COMPILE_OPTIONS "-O2")
endif()
# One resident model per session (quartznet_runtime.h). Every translation unit
# that includes the runtime header must see the same value.
target_compile_definitions(${COMPONENT_LIB} PUBLIC HW1_STT_RESIDENT_MODEL=1)
# stt/stt_model_selection.h turns these into the model identity every file sees.
if(HW1_STT_MODEL STREQUAL "citrinet")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC ENABLE_STT_MODEL_CITRINET=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC ENABLE_STT_MODEL_QUARTZNET=1)
endif()

# Compile only the ESP-DL operators the model graph uses.
idf_build_get_property(_hw1_build_components BUILD_COMPONENTS)
if("espressif__esp-dl" IN_LIST _hw1_build_components)
    idf_component_get_property(_hw1_dl_dir espressif__esp-dl COMPONENT_DIR)
elseif("esp-dl" IN_LIST _hw1_build_components)
    idf_component_get_property(_hw1_dl_dir esp-dl COMPONENT_DIR)
else()
    message(FATAL_ERROR "hw1_stt: espressif/esp-dl is not in the build")
endif()
if(NOT EXISTS "${_hw1_dl_dir}/hw1-esp-dl-override.json")
    message(FATAL_ERROR "hw1_stt: ${_hw1_dl_dir} is not the patched ESP-DL. Run firmware/tools/prepare_esp_dl.py first.")
endif()
include("${_hw1_dl_dir}/cmake/compile_register.cmake")
esp_dl_compile_add_yml("${CMAKE_CURRENT_LIST_DIR}/stt/${HW1_STT_MODEL}.compile_req.yml")
if(HW1_STT_SPEAKER_LABELS)
    esp_dl_compile_add_yml("${CMAKE_CURRENT_LIST_DIR}/stt/speaker.compile_req.yml")
endif()
if(HW1_OVERLAP_GATE)
    esp_dl_compile_add_yml("${CMAKE_CURRENT_LIST_DIR}/stt/overlap_gate.compile_req.yml")
endif()

if(HW1_STT_SPEAKER_LABELS)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HW1_STT_SPEAKER_LABELS=1)
endif()
if(HW1_OVERLAP_GATE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HW1_OVERLAP_GATE=1)
endif()

if(CONFIG_CAPTIONS_STREAM_METRICS)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HW1_STT_STREAM_METRICS=1)
    set_property(SOURCE stt/stt_stream.cpp APPEND PROPERTY COMPILE_OPTIONS -fstack-usage)
endif()
