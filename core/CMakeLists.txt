cmake_minimum_required(VERSION 3.20)
project(VMPCore VERSION 0.0.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(LIBVA libva)

set(CORE_SOURCES
    src/hw_decoder.cpp
    src/audio_engine.cpp
    src/telemetry.cpp
    src/subtitles.cpp
    src/resume_manager.cpp
    src/thumbnail_generator.cpp
    src/config_manager.cpp
    src/video_player.cpp
    src/sonic.c
)

add_library(vmp_core STATIC
    ${CORE_SOURCES}
)
set_target_properties(vmp_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# SIMD & CPU optimizations conditionally enabled for x86_64
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(vmp_core PRIVATE -mavx2 -mfma)
endif()

target_compile_options(vmp_core PRIVATE
    -O3
    -flto
    -ffast-math
    -fomit-frame-pointer
    -pthread
)

target_include_directories(vmp_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        ${AVCODEC_INCLUDE_DIRS}
        ${AVFORMAT_INCLUDE_DIRS}
        ${SWSCALE_INCLUDE_DIRS}
        ${SWRESAMPLE_INCLUDE_DIRS}
        ${AVUTIL_INCLUDE_DIRS}
        ${SDL2_INCLUDE_DIRS}
)

target_link_directories(vmp_core
    PUBLIC
        ${AVCODEC_LIBRARY_DIRS}
        ${AVFORMAT_LIBRARY_DIRS}
        ${SWSCALE_LIBRARY_DIRS}
        ${SWRESAMPLE_LIBRARY_DIRS}
        ${AVUTIL_LIBRARY_DIRS}
        ${SDL2_LIBRARY_DIRS}
)

target_link_libraries(vmp_core
    PUBLIC
        ${AVCODEC_LIBRARIES}
        ${AVFORMAT_LIBRARIES}
        ${SWSCALE_LIBRARIES}
        ${SWRESAMPLE_LIBRARIES}
        ${AVUTIL_LIBRARIES}
        ${SDL2_LIBRARIES}
        pthread
        dl
)
