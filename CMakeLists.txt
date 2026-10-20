cmake_minimum_required(VERSION 3.20)
project(transcribe VERSION 0.1.3 LANGUAGES CXX)

option(TRANSCRIBE_ENABLE_ASIO "Build real-time capture using RtAudio's ASIO backend" OFF)
option(TRANSCRIBE_ENABLE_ONNX "Build ONNX Runtime model inference support" OFF)
option(TRANSCRIBE_ENABLE_FFMPEG "Enable FFmpeg decoding for compressed audio formats" OFF)
option(TRANSCRIBE_ENABLE_WEBVIEW2 "Use the WebView2 GUI instead of the native Win32 form" OFF)
option(TRANSCRIBE_BUILD_TESTS "Build Catch2-based unit tests" OFF)

if(TRANSCRIBE_ENABLE_WEBVIEW2 AND NOT WIN32)
    message(FATAL_ERROR "The WebView2 GUI is supported only on Windows.")
endif()

add_library(transcribe_core STATIC
    src/audio_file.cpp
    src/audio_ring_buffer.cpp
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
    add_library(transcribe_gui_backend STATIC src/gui_transcription.cpp)
    target_link_libraries(transcribe_gui_backend PUBLIC transcribe_core)
    target_compile_features(transcribe_gui_backend PUBLIC cxx_std_20)

    if(TRANSCRIBE_ENABLE_WEBVIEW2)
        set(TRANSCRIBE_WEBVIEW2_SDK_ROOT "" CACHE PATH
            "Root of an extracted Microsoft.Web.WebView2 NuGet package")
        set(TRANSCRIBE_WEBVIEW2_BOOTSTRAPPER_PATH "" CACHE FILEPATH
            "Path to a pre-downloaded Evergreen WebView2 bootstrapper")
        if(TRANSCRIBE_WEBVIEW2_BOOTSTRAPPER_PATH)
            set(_webview2_bootstrapper "${TRANSCRIBE_WEBVIEW2_BOOTSTRAPPER_PATH}")
        else()
            set(_webview2_bootstrapper
                "${CMAKE_CURRENT_BINARY_DIR}/webview2-runtime/MicrosoftEdgeWebView2Setup.exe")
            if(NOT EXISTS "${_webview2_bootstrapper}")
                get_filename_component(_webview2_runtime_dir
                    "${_webview2_bootstrapper}" DIRECTORY)
                file(MAKE_DIRECTORY "${_webview2_runtime_dir}")
                file(DOWNLOAD
                    "https://go.microsoft.com/fwlink/p/?LinkId=2124703"
                    "${_webview2_bootstrapper}"
                    STATUS _webview2_download_status
                    TLS_VERIFY ON
                    TIMEOUT 180)
                list(GET _webview2_download_status 0 _webview2_download_code)
                list(GET _webview2_download_status 1 _webview2_download_message)
                if(NOT _webview2_download_code EQUAL 0)
                    file(REMOVE "${_webview2_bootstrapper}")
                    message(FATAL_ERROR
                        "Could not download the Evergreen WebView2 bootstrapper: "
                        "${_webview2_download_message}. Set "
                        "TRANSCRIBE_WEBVIEW2_BOOTSTRAPPER_PATH to a local copy to build offline.")
                endif()
            endif()
        endif()
        if(NOT EXISTS "${_webview2_bootstrapper}")
            message(FATAL_ERROR
                "WebView2 bootstrapper not found: ${_webview2_bootstrapper}")
        endif()
        file(SIZE "${_webview2_bootstrapper}" _webview2_bootstrapper_size)
        if(_webview2_bootstrapper_size LESS 100000)
            message(FATAL_ERROR
                "WebView2 bootstrapper is unexpectedly small: ${_webview2_bootstrapper}")
        endif()
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_webview2_arch x64)
        else()
            set(_webview2_arch x86)
        endif()
        find_path(WEBVIEW2_INCLUDE_DIR WebView2.h
            HINTS "${TRANSCRIBE_WEBVIEW2_SDK_ROOT}" ENV WEBVIEW2_SDK_ROOT
            PATH_SUFFIXES build/native/include include)
        find_file(WEBVIEW2_LOADER_DLL WebView2Loader.dll
            HINTS "${TRANSCRIBE_WEBVIEW2_SDK_ROOT}" ENV WEBVIEW2_SDK_ROOT
            PATH_SUFFIXES "runtimes/win-${_webview2_arch}/native"
                          "build/native/${_webview2_arch}")
        find_file(WEBVIEW2_LICENSE_FILE LICENSE.txt
            HINTS "${TRANSCRIBE_WEBVIEW2_SDK_ROOT}" ENV WEBVIEW2_SDK_ROOT)
        find_file(WEBVIEW2_NOTICE_FILE NOTICE.txt
            HINTS "${TRANSCRIBE_WEBVIEW2_SDK_ROOT}" ENV WEBVIEW2_SDK_ROOT)
        if(NOT WEBVIEW2_INCLUDE_DIR OR NOT WEBVIEW2_LOADER_DLL OR
           NOT WEBVIEW2_LICENSE_FILE OR NOT WEBVIEW2_NOTICE_FILE)
            message(FATAL_ERROR
                "WebView2 SDK not found. Set TRANSCRIBE_WEBVIEW2_SDK_ROOT to an extracted "
                "Microsoft.Web.WebView2 NuGet package.")
        endif()
        add_executable(transcribe-gui WIN32 src/gui_webview.cpp)
        target_include_directories(transcribe-gui SYSTEM PRIVATE "${WEBVIEW2_INCLUDE_DIR}")
        target_compile_definitions(transcribe-gui PRIVATE TRANSCRIBE_ENABLE_WEBVIEW2=1)
        set_property(SOURCE src/gui_webview.cpp APPEND PROPERTY OBJECT_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/styles.css"
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/app.js")
        target_link_libraries(transcribe-gui PRIVATE transcribe_gui_backend
            comdlg32 user32 gdi32 shell32 ole32)
        add_custom_command(TARGET transcribe-gui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${WEBVIEW2_LOADER_DLL}" "$<TARGET_FILE_DIR:transcribe-gui>/WebView2Loader.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${WEBVIEW2_LICENSE_FILE}" "$<TARGET_FILE_DIR:transcribe-gui>/WebView2-LICENSE.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${WEBVIEW2_NOTICE_FILE}" "$<TARGET_FILE_DIR:transcribe-gui>/WebView2-NOTICE.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_webview2_bootstrapper}" "$<TARGET_FILE_DIR:transcribe-gui>/MicrosoftEdgeWebView2Setup.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/ui" "$<TARGET_FILE_DIR:transcribe-gui>/ui")
    else()
        add_executable(transcribe-gui WIN32 src/gui.cpp)
        target_link_libraries(transcribe-gui PRIVATE transcribe_gui_backend
            comdlg32 user32 gdi32 comctl32 shell32)
    endif()
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
        target_compile_options(transcribe_gui_backend PRIVATE /W4 /permissive-)
    else()
        target_compile_options(transcribe-gui PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(transcribe_gui_backend PRIVATE -Wall -Wextra -Wpedantic)
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
    if(MINGW)
        target_link_libraries(transcribe_core PUBLIC winpthread)
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

if(TRANSCRIBE_ENABLE_FFMPEG)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat libavcodec libavutil libswresample)
    target_sources(transcribe_core PRIVATE src/ffmpeg_audio.cpp)
    target_link_libraries(transcribe_core PUBLIC PkgConfig::FFMPEG)
    target_compile_definitions(transcribe_core PUBLIC TRANSCRIBE_ENABLE_FFMPEG=1)
endif()

if(TRANSCRIBE_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(Catch2)
    enable_testing()
    add_executable(transcribe-tests tests/transcribe_tests.cpp)
    target_link_libraries(transcribe-tests PRIVATE transcribe_core Catch2::Catch2WithMain)
    target_compile_features(transcribe-tests PRIVATE cxx_std_20)
    if(TRANSCRIBE_ENABLE_ONNX)
        file(TO_CMAKE_PATH
            "${CMAKE_CURRENT_SOURCE_DIR}/python/models/basic_pitch.onnx"
            BASIC_PITCH_MODEL_PATH)
        target_compile_definitions(transcribe-tests PRIVATE
            TRANSCRIBE_BASIC_PITCH_MODEL_PATH="${BASIC_PITCH_MODEL_PATH}")
    endif()
    if(MSVC)
        target_compile_options(transcribe-tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(transcribe-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    include(Catch)
    catch_discover_tests(transcribe-tests)
endif()
