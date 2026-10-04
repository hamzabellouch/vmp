cmake_minimum_required(VERSION 3.20)
project(VMPDesktop VERSION 0.0.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_definitions(-DGL_GLEXT_PROTOTYPES)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT TARGET vmp_core)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_CURRENT_BINARY_DIR}/core)
endif()

find_package(PkgConfig REQUIRED)

pkg_check_modules(GLFW REQUIRED glfw3)
pkg_check_modules(GL REQUIRED gl)
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(FRIBIDI REQUIRED fribidi)
pkg_check_modules(DBUS dbus-1)

set(DESKTOP_SOURCES
    src/main_desktop.cpp
    src/shader_renderer.cpp
)

if(DBUS_FOUND)
    list(APPEND DESKTOP_SOURCES src/mpris_manager.cpp)
    add_compile_definitions(VMP_HAS_DBUS)
endif()

add_executable(vmp_engine
    ${DESKTOP_SOURCES}
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(vmp_engine PRIVATE -mavx2 -mfma)
endif()

target_compile_options(vmp_engine PRIVATE
    -O3
    -flto
    -ffast-math
    -fomit-frame-pointer
    -pthread
)
target_link_options(vmp_engine PRIVATE -flto -pthread)

target_include_directories(vmp_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${GLFW_INCLUDE_DIRS}
    ${GL_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${FRIBIDI_INCLUDE_DIRS}
    ${DBUS_INCLUDE_DIRS}
)

target_link_directories(vmp_engine PRIVATE
    ${GLFW_LIBRARY_DIRS}
    ${GL_LIBRARY_DIRS}
    ${FREETYPE_LIBRARY_DIRS}
    ${FRIBIDI_LIBRARY_DIRS}
    ${DBUS_LIBRARY_DIRS}
)

target_link_libraries(vmp_engine
    PRIVATE
    vmp_core
    ${GLFW_LIBRARIES}
    ${GL_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${FRIBIDI_LIBRARIES}
    ${DBUS_LIBRARIES}
)

add_custom_target(vmp_desktop ALL
    COMMAND ${CMAKE_COMMAND} -E create_symlink $<TARGET_FILE_NAME:vmp_engine> ${CMAKE_CURRENT_BINARY_DIR}/vmp_desktop
    COMMAND ${CMAKE_COMMAND} -E create_symlink $<TARGET_FILE_NAME:vmp_engine> ${CMAKE_CURRENT_BINARY_DIR}/vmp
    DEPENDS vmp_engine
)

# Target 2: Shared Library (libvmp.so)
add_library(vmp SHARED
    src/shader_renderer.cpp
)
target_include_directories(vmp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${GLFW_INCLUDE_DIRS}
    ${GL_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${FRIBIDI_INCLUDE_DIRS}
)
target_link_libraries(vmp
    PRIVATE
    vmp_core
    ${GLFW_LIBRARIES}
    ${GL_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${FRIBIDI_LIBRARIES}
)
set_target_properties(vmp PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 2)

install(TARGETS vmp_engine DESTINATION bin)
install(TARGETS vmp DESTINATION lib)
