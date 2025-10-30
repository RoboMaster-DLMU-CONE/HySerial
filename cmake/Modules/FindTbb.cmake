include(FetchContent)
find_package(TBB QUIET)
if (NOT TBB_FOUND)
    execute_process(
            COMMAND git ls-remote --heads https://github.com/uxlfoundation/oneTBB master
            RESULT_VARIABLE tbb_primary_repo_ok
            OUTPUT_QUIET
            ERROR_QUIET
    )
    if (NOT tbb_primary_repo_ok EQUAL 0)
        message(STATUS "Primary fetch failed, falling back to Gitee mirror")
        set(tbb_repo https://gitee.com/dlmu-cone/oneTBB)
    else ()
        set(tbb_repo https://github.com/uxlfoundation/oneTBB)
    endif ()

    FetchContent_Declare(tbb
            GIT_REPOSITORY ${tbb_repo}
            GIT_TAG master
            GIT_SHALLOW true
    )
    FetchContent_MakeAvailable(tbb)
endif ()