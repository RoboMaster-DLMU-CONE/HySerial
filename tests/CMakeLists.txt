# Tests CMake
add_executable(test_serial_app test_serial_app.cpp)
target_include_directories(test_serial_app PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(test_serial_app PRIVATE HySerial)

add_executable(benchmark_serial_app benchmark_serial_app.cpp)
target_include_directories(benchmark_serial_app PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(benchmark_serial_app PRIVATE HySerial)

add_test(NAME serial_connectivity
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/python/test_serial.py
        $<TARGET_FILE:test_serial_app>)

# Benchmark test: run the relay runner with benchmark binary and args (count 10000, payload 64)
add_test(NAME serial_benchmark
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/python/test_serial.py
        $<TARGET_FILE:benchmark_serial_app> 10000 64)
