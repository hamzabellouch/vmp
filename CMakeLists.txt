cmake_minimum_required(VERSION 3.20)
project(VMPSuite VERSION 0.0.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(VMP_VERSION_STRING="v0.0.2-beta")

# Subdirectories for modular CLI and Desktop components
add_subdirectory(cli)
add_subdirectory(desktop)
