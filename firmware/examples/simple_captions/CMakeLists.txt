cmake_minimum_required(VERSION 3.16)
# Speech to text on the ESP32-P4-EYE: microphone -> captions on the serial console.
# Build steps are in the README at the top of the repository.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/hw1_stt")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(simple_captions)
