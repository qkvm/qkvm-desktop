cmake_minimum_required(VERSION 3.24)
project(qkvm_webrtc_cpp_trial VERSION 0.1 LANGUAGES CXX)

# Standalone experiment. No FetchContent, downloads, production targets, or automatic installs.
set(QKVM_FFMPEG_DIR "" CACHE PATH "Existing desktop FFmpeg SDK root (include, lib, bin)")
set(QKVM_TRIAL_FIXTURE "" CACHE FILEPATH "Generated raw Annex B H.264 fixture for CTest")
find_package(LibDataChannel 0.24.6 EXACT CONFIG REQUIRED)
find_path(TRIAL_AVCODEC_INCLUDE libavcodec/avcodec.h HINTS "${QKVM_FFMPEG_DIR}/include" REQUIRED NO_DEFAULT_PATH)
find_library(TRIAL_AVCODEC_LIBRARY NAMES avcodec HINTS "${QKVM_FFMPEG_DIR}/lib" REQUIRED NO_DEFAULT_PATH)
find_library(TRIAL_AVUTIL_LIBRARY NAMES avutil HINTS "${QKVM_FFMPEG_DIR}/lib" REQUIRED NO_DEFAULT_PATH)

add_executable(qkvm_webrtc_cpp_trial main.cpp)
target_compile_features(qkvm_webrtc_cpp_trial PRIVATE cxx_std_20)
target_include_directories(qkvm_webrtc_cpp_trial SYSTEM PRIVATE "${TRIAL_AVCODEC_INCLUDE}")
target_link_libraries(qkvm_webrtc_cpp_trial PRIVATE LibDataChannel::LibDataChannel
    "${TRIAL_AVCODEC_LIBRARY}" "${TRIAL_AVUTIL_LIBRARY}")
if(MSVC)
    target_compile_options(qkvm_webrtc_cpp_trial PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    target_compile_definitions(qkvm_webrtc_cpp_trial PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(qkvm_webrtc_cpp_trial PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

include(CTest)
if(BUILD_TESTING AND QKVM_TRIAL_FIXTURE)
    add_test(NAME local_h264_roundtrip COMMAND qkvm_webrtc_cpp_trial "${QKVM_TRIAL_FIXTURE}" 5 30)
    set_tests_properties(local_h264_roundtrip PROPERTIES TIMEOUT 180)
endif()
