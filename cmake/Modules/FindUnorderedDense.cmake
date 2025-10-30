execute_process(
        COMMAND git ls-remote --heads https://github.com/martinus/unordered_dense.git master
        RESULT_VARIABLE unordered_dense_primary_repo_ok
        OUTPUT_QUIET
        ERROR_QUIET
)

if (NOT unordered_dense_primary_repo_ok EQUAL 0)
    message(STATUS "Primary fetch failed, falling back to Gitee mirror")
    set(unordered_dense_repo https://gitee.com/dlmu-cone/unordered_dense.git)
else ()
    set(unordered_dense_repo https://github.com/martinus/unordered_dense.git)
endif ()


FetchContent_Declare(unordered_dense
        GIT_REPOSITORY ${unordered_dense_repo}
        GIT_TAG main
        GIT_SHALLOW true
)
FetchContent_MakeAvailable(unordered_dense)
