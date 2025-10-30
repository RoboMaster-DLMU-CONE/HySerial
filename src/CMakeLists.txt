add_library(HySerial)

file(GLOB_RECURSE HYSERIAL_SOURCES "${PROJECT_SOURCE_DIR}/src/*.cpp")

# Optional stasis/statistics sources
if (HS_ENABLE_STASIS)
    target_compile_definitions(HySerial PUBLIC HS_ENABLE_STASIS)
endif ()

target_sources(HySerial PRIVATE ${HYSERIAL_SOURCES})
target_include_directories(HySerial PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(HySerial PUBLIC
        tl::expected
        unordered_dense::unordered_dense
        ${LIBURING_LIBRARY}
)
