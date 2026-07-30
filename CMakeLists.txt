cmake_minimum_required(VERSION 3.24)

project(ascii-player VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "ascii-player currently targets the Windows Console API.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FFMPEG_ROOT "" CACHE PATH "Path to an FFmpeg development bundle")
if(NOT FFMPEG_ROOT AND DEFINED ENV{FFMPEG_ROOT})
    file(TO_CMAKE_PATH "$ENV{FFMPEG_ROOT}" FFMPEG_ROOT)
endif()
if(NOT FFMPEG_ROOT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg")
    set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg")
endif()

find_path(FFMPEG_INCLUDE_DIR
    NAMES libavformat/avformat.h
    HINTS "${FFMPEG_ROOT}"
    PATH_SUFFIXES include
    REQUIRED
)

set(FFMPEG_LIBRARIES)
foreach(component avformat avcodec avutil swscale swresample)
    string(TOUPPER "${component}" component_upper)
    find_library(FFMPEG_${component_upper}_LIBRARY
        NAMES "${component}"
        HINTS "${FFMPEG_ROOT}"
        PATH_SUFFIXES lib
        REQUIRED
    )
    list(APPEND FFMPEG_LIBRARIES "${FFMPEG_${component_upper}_LIBRARY}")
endforeach()

add_executable(ascii-player
    src/main.cpp
    src/VideoDecoder.cpp
    src/AudioPlayer.cpp
    src/AsciiConverter.cpp
    src/Renderer.cpp
    src/Timer.cpp
)

target_include_directories(ascii-player PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${FFMPEG_INCLUDE_DIR}"
)

target_compile_definitions(ascii-player PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(ascii-player PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(ascii-player PRIVATE -Wall -Wextra -Wpedantic)
    target_link_options(ascii-player PRIVATE -municode)
endif()

target_link_libraries(ascii-player PRIVATE
    ${FFMPEG_LIBRARIES}
    winmm
)

if(FFMPEG_ROOT AND EXISTS "${FFMPEG_ROOT}/bin")
    file(GLOB FFMPEG_RUNTIME_DLLS "${FFMPEG_ROOT}/bin/*.dll")
    foreach(runtime_dll IN LISTS FFMPEG_RUNTIME_DLLS)
        add_custom_command(TARGET ascii-player POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${runtime_dll}" "$<TARGET_FILE_DIR:ascii-player>"
            VERBATIM
        )
    endforeach()
endif()

install(TARGETS ascii-player RUNTIME DESTINATION bin)
