cmake_minimum_required(VERSION 3.20)
project(VMPCLI VERSION 0.0.4 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(VMP_HEADLESS_CLI)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT TARGET vmp_core)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_CURRENT_BINARY_DIR}/core)
endif()

add_executable(vmp_cli
    src/main_cli.cpp
)

target_link_libraries(vmp_cli
    PRIVATE
    vmp_core
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(vmp_cli PRIVATE -mavx2 -mfma)
endif()

target_compile_options(vmp_cli PRIVATE
    -O3
    -flto
    -ffast-math
    -fomit-frame-pointer
    -pthread
)
target_link_options(vmp_cli PRIVATE -flto -pthread)

install(TARGETS vmp_cli DESTINATION bin)
