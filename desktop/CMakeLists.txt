cmake_minimum_required(VERSION 3.20)
project(VMPDesktop VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_definitions(-DGL_GLEXT_PROTOTYPES)

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

# Find PkgConfig to locate FFmpeg, GLFW, OpenGL, and SDL2
find_package(PkgConfig REQUIRED)

pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(GLFW REQUIRED glfw3)
pkg_check_modules(GL REQUIRED gl)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(FRIBIDI REQUIRED fribidi)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${GLFW_INCLUDE_DIRS}
    ${GL_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${FRIBIDI_INCLUDE_DIRS}
)

link_directories(
    ${AVCODEC_LIBRARY_DIRS}
    ${AVFORMAT_LIBRARY_DIRS}
    ${SWSCALE_LIBRARY_DIRS}
    ${SWRESAMPLE_LIBRARY_DIRS}
    ${AVUTIL_LIBRARY_DIRS}
    ${GLFW_LIBRARY_DIRS}
    ${GL_LIBRARY_DIRS}
    ${SDL2_LIBRARY_DIRS}
    ${FREETYPE_LIBRARY_DIRS}
    ${FRIBIDI_LIBRARY_DIRS}
)

set(DESKTOP_SOURCES
    src/main_desktop.cpp
    src/shader_renderer.cpp
    src/hw_decoder.cpp
    src/audio_engine.cpp
    src/telemetry.cpp
    src/video_player.cpp
    src/subtitles.cpp
    src/resume_manager.cpp
    src/thumbnail_generator.cpp
)

# Target 1: Desktop GUI Executable (vmp_engine)
add_executable(vmp_engine
    ${DESKTOP_SOURCES}
)

target_link_libraries(vmp_engine
    PRIVATE
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${GLFW_LIBRARIES}
    ${GL_LIBRARIES}
    ${SDL2_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${FRIBIDI_LIBRARIES}
    pthread
    dl
)

# Target alias symlinks: vmp, vmp_desktop -> vmp_engine
add_custom_target(vmp_desktop ALL
    COMMAND ${CMAKE_COMMAND} -E create_symlink $<TARGET_FILE_NAME:vmp_engine> ${CMAKE_CURRENT_BINARY_DIR}/vmp_desktop
    COMMAND ${CMAKE_COMMAND} -E create_symlink $<TARGET_FILE_NAME:vmp_engine> ${CMAKE_CURRENT_BINARY_DIR}/vmp
    DEPENDS vmp_engine
)

# Target 2: Shared Library (libvmp.so)
add_library(vmp SHARED
    src/hw_decoder.cpp
    src/audio_engine.cpp
    src/telemetry.cpp
    src/video_player.cpp
    src/subtitles.cpp
    src/resume_manager.cpp
    src/thumbnail_generator.cpp
    src/shader_renderer.cpp
)

target_link_libraries(vmp
    PRIVATE
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${GLFW_LIBRARIES}
    ${GL_LIBRARIES}
    ${SDL2_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${FRIBIDI_LIBRARIES}
    pthread
    dl
)

set_target_properties(vmp PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 2)
