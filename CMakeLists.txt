cmake_minimum_required(VERSION 3.20)
project(transcriber VERSION 0.1.4 LANGUAGES CXX)

option(TRANSCRIBE_ENABLE_ASIO "Build real-time capture using RtAudio's ASIO backend" OFF)
option(TRANSCRIBE_ENABLE_ONNX "Build ONNX Runtime model inference support" OFF)
option(TRANSCRIBE_ONNX_ENABLE_CUDA "Enable ONNX Runtime CUDA execution provider selection" OFF)
option(TRANSCRIBE_ONNX_ENABLE_DIRECTML "Enable ONNX Runtime DirectML execution provider selection" OFF)
option(TRANSCRIBE_ONNX_ENABLE_ROCM "Enable ONNX Runtime ROCm execution provider selection" OFF)
set(ONNXRUNTIME_ROOT "" CACHE PATH "ONNX Runtime installation root")
set(TRANSCRIBE_DIRECTML_SDK_ROOT "" CACHE PATH
    "Root of the DirectML SDK containing DirectML.h")
option(TRANSCRIBE_ENABLE_FFMPEG "Enable FFmpeg decoding for compressed audio formats" OFF)
option(TRANSCRIBE_ENABLE_WEBVIEW2 "Use the WebView2 GUI instead of the native Win32 form" OFF)
option(TRANSCRIBE_BUILD_TESTS "Build Catch2-based unit tests" OFF)

if(TRANSCRIBE_ENABLE_WEBVIEW2 AND NOT WIN32)
    message(FATAL_ERROR "The WebView2 GUI is supported only on Windows.")
endif()
if((TRANSCRIBE_ONNX_ENABLE_CUDA OR TRANSCRIBE_ONNX_ENABLE_DIRECTML OR
    TRANSCRIBE_ONNX_ENABLE_ROCM) AND NOT TRANSCRIBE_ENABLE_ONNX)
    message(FATAL_ERROR "ONNX GPU providers require TRANSCRIBE_ENABLE_ONNX=ON.")
endif()
if(TRANSCRIBE_ONNX_ENABLE_DIRECTML AND NOT WIN32)
    message(FATAL_ERROR "The ONNX DirectML provider is supported only on Windows.")
endif()
if(TRANSCRIBE_ONNX_ENABLE_ROCM AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The ONNX ROCm provider is supported only on Linux.")
endif()

add_library(transcriber_core STATIC
    src/audio_file.cpp
    src/audio_ring_buffer.cpp
    src/asio_capture.cpp
    src/transcriber.cpp
    src/musicxml.cpp
    src/guitar_tab.cpp
)

target_include_directories(transcriber_core PUBLIC include)
target_compile_features(transcriber_core PUBLIC cxx_std_20)

add_executable(transcriber src/main.cpp)
target_link_libraries(transcriber PRIVATE transcriber_core)

if(WIN32)
    add_library(transcriber_gui_backend STATIC src/gui_transcription.cpp)
    target_link_libraries(transcriber_gui_backend PUBLIC transcriber_core)
    target_compile_features(transcriber_gui_backend PUBLIC cxx_std_20)

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
        add_executable(transcriber-gui WIN32 src/gui_webview.cpp)
        target_include_directories(transcriber-gui SYSTEM PRIVATE "${WEBVIEW2_INCLUDE_DIR}")
        target_compile_definitions(transcriber-gui PRIVATE TRANSCRIBE_ENABLE_WEBVIEW2=1)
        set_property(SOURCE src/gui_webview.cpp APPEND PROPERTY OBJECT_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/styles.css"
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/app.js"
            "${CMAKE_CURRENT_SOURCE_DIR}/ui/editor.js")
        target_link_libraries(transcriber-gui PRIVATE transcriber_gui_backend
            comdlg32 user32 gdi32 shell32 ole32 winmm)
        add_custom_command(TARGET transcriber-gui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${WEBVIEW2_LOADER_DLL}" "$<TARGET_FILE_DIR:transcriber-gui>/WebView2Loader.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${WEBVIEW2_LICENSE_FILE}" "$<TARGET_FILE_DIR:transcriber-gui>/WebView2-LICENSE.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${WEBVIEW2_NOTICE_FILE}" "$<TARGET_FILE_DIR:transcriber-gui>/WebView2-NOTICE.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_webview2_bootstrapper}" "$<TARGET_FILE_DIR:transcriber-gui>/MicrosoftEdgeWebView2Setup.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/ui" "$<TARGET_FILE_DIR:transcriber-gui>/ui")
    else()
        add_executable(transcriber-gui WIN32 src/gui.cpp)
        target_link_libraries(transcriber-gui PRIVATE transcriber_gui_backend
            comdlg32 user32 gdi32 comctl32 shell32)
    endif()
    target_compile_definitions(transcriber-gui PRIVATE UNICODE _UNICODE)
endif()

foreach(target transcriber_core transcriber)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
if(WIN32)
    if(MSVC)
        target_compile_options(transcriber-gui PRIVATE /W4 /permissive-)
        target_compile_options(transcriber_gui_backend PRIVATE /W4 /permissive-)
    else()
        target_compile_options(transcriber-gui PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(transcriber_gui_backend PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(TRANSCRIBE_ENABLE_ASIO)
    if(NOT WIN32)
        message(FATAL_ERROR "The ASIO capture option is supported only on Windows.")
    endif()
    find_package(RtAudio CONFIG REQUIRED)
    if(TARGET RtAudio::rtaudio)
        target_link_libraries(transcriber_core PUBLIC RtAudio::rtaudio)
    elseif(TARGET rtaudio)
        target_link_libraries(transcriber_core PUBLIC rtaudio)
    else()
        message(FATAL_ERROR "RtAudio was found, but its CMake package exports no supported target.")
    endif()
    if(MINGW)
        target_link_libraries(transcriber_core PUBLIC winpthread)
    endif()
    target_compile_definitions(transcriber_core PUBLIC TRANSCRIBE_ENABLE_ASIO=1)
endif()

if(TRANSCRIBE_ENABLE_ONNX)
    find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
        HINTS "${ONNXRUNTIME_ROOT}" ENV ONNXRUNTIME_ROOT
        PATH_SUFFIXES include include/onnxruntime)
    find_library(ONNXRUNTIME_LIBRARY onnxruntime
        HINTS "${ONNXRUNTIME_ROOT}" ENV ONNXRUNTIME_ROOT
        PATH_SUFFIXES lib lib64)
    if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
        message(FATAL_ERROR "ONNX Runtime was not found. Set ONNXRUNTIME_ROOT to its installation directory.")
    endif()
    target_include_directories(transcriber_core PRIVATE "${ONNXRUNTIME_INCLUDE_DIR}")
    target_link_libraries(transcriber_core PUBLIC "${ONNXRUNTIME_LIBRARY}")
    target_compile_definitions(transcriber_core PUBLIC TRANSCRIBE_ENABLE_ONNX=1)
    if(TRANSCRIBE_ONNX_ENABLE_CUDA)
        target_compile_definitions(transcriber_core PRIVATE
            TRANSCRIBE_ONNX_ENABLE_CUDA=1)
    endif()
    if(TRANSCRIBE_ONNX_ENABLE_ROCM)
        target_compile_definitions(transcriber_core PRIVATE
            TRANSCRIBE_ONNX_ENABLE_ROCM=1)
    endif()
    if(TRANSCRIBE_ONNX_ENABLE_DIRECTML)
        find_path(ONNXRUNTIME_DML_INCLUDE_DIR dml_provider_factory.h
            HINTS "${ONNXRUNTIME_ROOT}" ENV ONNXRUNTIME_ROOT
            PATH_SUFFIXES include/onnxruntime/core/providers/dml
                          include/core/providers/dml)
        find_path(TRANSCRIBE_DIRECTML_INCLUDE_DIR DirectML.h
            HINTS "${TRANSCRIBE_DIRECTML_SDK_ROOT}" ENV DIRECTML_ROOT
            PATH_SUFFIXES include)
        if(NOT ONNXRUNTIME_DML_INCLUDE_DIR OR
           NOT TRANSCRIBE_DIRECTML_INCLUDE_DIR)
            message(FATAL_ERROR
                "DirectML headers were not found. Use a DirectML-enabled ONNX "
                "Runtime SDK and DirectML SDK; set ONNXRUNTIME_DML_INCLUDE_DIR "
                "and TRANSCRIBE_DIRECTML_SDK_ROOT.")
        endif()
        target_include_directories(transcriber_core SYSTEM PRIVATE
            "${ONNXRUNTIME_DML_INCLUDE_DIR}"
            "${TRANSCRIBE_DIRECTML_INCLUDE_DIR}")
        target_compile_definitions(transcriber_core PRIVATE
            TRANSCRIBE_ONNX_ENABLE_DIRECTML=1)
    endif()
endif()

if(TRANSCRIBE_ENABLE_FFMPEG)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat libavcodec libavutil libswresample)
    target_sources(transcriber_core PRIVATE src/ffmpeg_audio.cpp)
    target_link_libraries(transcriber_core PUBLIC PkgConfig::FFMPEG)
    target_compile_definitions(transcriber_core PUBLIC TRANSCRIBE_ENABLE_FFMPEG=1)
endif()

if(TRANSCRIBE_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(Catch2)
    enable_testing()
    add_executable(transcriber-tests tests/transcribe_tests.cpp)
    target_link_libraries(transcriber-tests PRIVATE transcriber_core Catch2::Catch2WithMain)
    target_compile_features(transcriber-tests PRIVATE cxx_std_20)
    if(TRANSCRIBE_ENABLE_ONNX)
        file(TO_CMAKE_PATH
            "${CMAKE_CURRENT_SOURCE_DIR}/python/models/basic_pitch.onnx"
            BASIC_PITCH_MODEL_PATH)
        target_compile_definitions(transcriber-tests PRIVATE
            TRANSCRIBE_BASIC_PITCH_MODEL_PATH="${BASIC_PITCH_MODEL_PATH}")
    endif()
    if(MSVC)
        target_compile_options(transcriber-tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(transcriber-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    include(Catch)
    catch_discover_tests(transcriber-tests)

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME cli_export_validation
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_export_smoke.py"
                "$<TARGET_FILE:transcriber>"
                "${CMAKE_CURRENT_SOURCE_DIR}/samples/single_a4.wav")
        add_test(NAME ui_asset_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui_smoke.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/ui")
        add_test(NAME real_audio_smoke
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/real_audio_smoke.py"
                "$<TARGET_FILE:transcriber>")
        set_tests_properties(real_audio_smoke PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

install(TARGETS transcriber RUNTIME DESTINATION bin)
install(FILES README.md DESTINATION share/doc/transcriber)
install(DIRECTORY samples/ DESTINATION share/transcriber/samples
    FILES_MATCHING PATTERN "*.wav" PATTERN "*.expected.txt" PATTERN "README.md")
if(WIN32)
    install(TARGETS transcriber-gui RUNTIME DESTINATION bin)
    if(TRANSCRIBE_ENABLE_WEBVIEW2)
        install(FILES
            "${WEBVIEW2_LOADER_DLL}"
            "${WEBVIEW2_LICENSE_FILE}"
            "${WEBVIEW2_NOTICE_FILE}"
            "${_webview2_bootstrapper}"
            DESTINATION bin)
        install(DIRECTORY ui/ DESTINATION bin/ui)
    endif()
endif()
if(TRANSCRIBE_ENABLE_ONNX)
    install(FILES
        python/models/basic_pitch.onnx
        python/models/LICENSE-basic-pitch.txt
        DESTINATION share/transcriber/models)
endif()

set(CPACK_PACKAGE_VENDOR "Transcriber contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Audio-to-score transcription CLI and optional Windows workstation UI")
include(CPack)
