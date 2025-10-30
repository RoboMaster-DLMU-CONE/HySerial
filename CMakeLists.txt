cmake_minimum_required(VERSION 4.0)
project(HySerial)

set(CMAKE_CXX_STANDARD 20)

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

include(FindModules)

add_subdirectory(src)