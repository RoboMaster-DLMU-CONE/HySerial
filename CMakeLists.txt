cmake_minimum_required(VERSION 3.24)
project(HySerial VERSION 0.2.0)

set(CMAKE_CXX_STANDARD 20)

option(HS_BUILD_EXAMPLES "Build HySerial examples" OFF)
option(HS_BUILD_TESTS "Build HySerial tests" OFF)
option(HS_ENABLE_STASIS "Enable Stasis/statistics APIs for benchmarking" OFF)

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)

include(FindModules)

add_subdirectory(src)

if (HS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()

if (HS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

include(HySerialInstall)