cmake_minimum_required(VERSION 3.20)
project(VMPSuite VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Subdirectories for modular CLI and Desktop components
add_subdirectory(cli)
add_subdirectory(desktop)
