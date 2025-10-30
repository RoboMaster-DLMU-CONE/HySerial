add_library(HySerial)

file(GLOB_RECURSE HYSERIAL_SOURCES "${PROJECT_SOURCE_DIR}/src/*.cpp")

target_sources(HySerial PRIVATE ${HYSERIAL_SOURCES})
target_include_directories(HySerial PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(HySerial PRIVATE
        tl::expected
        unordered_dense::unordered_dense
        TBB::tbb
        ${LIBURING_LIBRARY}
)
