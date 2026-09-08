cmake_minimum_required(VERSION 3.20)
project(VMPCLI VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(VMP_HEADLESS_CLI)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# CPU & Hardware SIMD Optimizations (AVX2, FMA, LTO, Native Architecture)
add_compile_options(
    -O3
    -march=native
    -mtune=native
    -mavx2
    -mfma
    -flto
    -ffast-math
    -fomit-frame-pointer
    -pthread
)
add_link_options(-flto -pthread)

# Find PkgConfig to locate FFmpeg, SDL2, and system dependencies
find_package(PkgConfig REQUIRED)

pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SDL2 REQUIRED sdl2)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
)

link_directories(
    ${AVCODEC_LIBRARY_DIRS}
    ${AVFORMAT_LIBRARY_DIRS}
    ${SWSCALE_LIBRARY_DIRS}
    ${SWRESAMPLE_LIBRARY_DIRS}
    ${AVUTIL_LIBRARY_DIRS}
    ${SDL2_LIBRARY_DIRS}
)

set(CLI_SOURCES
    src/main_cli.cpp
    src/hw_decoder.cpp
    src/audio_engine.cpp
    src/telemetry.cpp
    src/video_player.cpp
    src/subtitles.cpp
    src/resume_manager.cpp
    src/thumbnail_generator.cpp
)

add_executable(vmp_cli
    ${CLI_SOURCES}
)

target_link_libraries(vmp_cli
    PRIVATE
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SDL2_LIBRARIES}
    pthread
    dl
)
