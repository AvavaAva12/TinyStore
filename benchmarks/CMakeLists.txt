find_package(benchmark QUIET)

if(NOT benchmark_FOUND)
    message(STATUS "Google Benchmark not found; benchmarks will be skipped.")
    return()
endif()

function(tinystore_add_benchmark name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name}
        PRIVATE
            tinystore
            benchmark::benchmark
            tinystore_options
    )
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
endfunction()

tinystore_add_benchmark(arena_bench)
tinystore_add_benchmark(db_bench)
