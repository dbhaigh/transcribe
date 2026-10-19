cmake_minimum_required(VERSION 3.20)
project(transcribe VERSION 0.0.1 LANGUAGES CXX)

option(TRANSCRIBE_ENABLE_ASIO "Build real-time capture using RtAudio's ASIO backend" OFF)
option(TRANSCRIBE_ENABLE_ONNX "Build ONNX Runtime model inference support" OFF)

add_library(transcribe_core STATIC
    src/audio_file.cpp
    src/asio_capture.cpp
    src/transcriber.cpp
    src/musicxml.cpp
    src/guitar_tab.cpp
)

target_include_directories(transcribe_core PUBLIC include)
target_compile_features(transcribe_core PUBLIC cxx_std_20)

add_executable(transcribe src/main.cpp)
target_link_libraries(transcribe PRIVATE transcribe_core)

if(WIN32)
    add_executable(transcribe-gui WIN32 src/gui.cpp)
    target_link_libraries(transcribe-gui PRIVATE transcribe_core comdlg32 user32 gdi32)
    target_compile_definitions(transcribe-gui PRIVATE UNICODE _UNICODE)
endif()

foreach(target transcribe_core transcribe)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
if(WIN32)
    if(MSVC)
        target_compile_options(transcribe-gui PRIVATE /W4 /permissive-)
    else()
        target_compile_options(transcribe-gui PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(TRANSCRIBE_ENABLE_ASIO)
    if(NOT WIN32)
        message(FATAL_ERROR "The ASIO capture option is supported only on Windows.")
    endif()
    find_package(RtAudio CONFIG REQUIRED)
    if(TARGET RtAudio::rtaudio)
        target_link_libraries(transcribe_core PUBLIC RtAudio::rtaudio)
    elseif(TARGET rtaudio)
        target_link_libraries(transcribe_core PUBLIC rtaudio)
    else()
        message(FATAL_ERROR "RtAudio was found, but its CMake package exports no supported target.")
    endif()
    target_compile_definitions(transcribe_core PUBLIC TRANSCRIBE_ENABLE_ASIO=1)
endif()

if(TRANSCRIBE_ENABLE_ONNX)
    find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
        HINTS ENV ONNXRUNTIME_ROOT
        PATH_SUFFIXES include include/onnxruntime)
    find_library(ONNXRUNTIME_LIBRARY onnxruntime
        HINTS ENV ONNXRUNTIME_ROOT
        PATH_SUFFIXES lib lib64)
    if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
        message(FATAL_ERROR "ONNX Runtime was not found. Set ONNXRUNTIME_ROOT to its installation directory.")
    endif()
    target_include_directories(transcribe_core PRIVATE "${ONNXRUNTIME_INCLUDE_DIR}")
    target_link_libraries(transcribe_core PUBLIC "${ONNXRUNTIME_LIBRARY}")
    target_compile_definitions(transcribe_core PUBLIC TRANSCRIBE_ENABLE_ONNX=1)
endif()
