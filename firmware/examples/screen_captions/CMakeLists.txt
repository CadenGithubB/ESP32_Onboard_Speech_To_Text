cmake_minimum_required(VERSION 3.16)
# Speech to text on the ESP32-P4-EYE with captions on the screen, settings on the
# wheel, and speaker labels. Build steps are in the README at the top of the repository.
set(HW1_STT_SPEAKER_LABELS ON CACHE BOOL "Compile the speaker-label model" FORCE)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/hw1_stt")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(screen_captions)
