# Reaper benchmarks

add_executable(reaper_bench
    bench_reaper.c
)

add_executable(reaper_cpp_bench
    bench_reaper_cpp.cpp
)

target_link_libraries(reaper_bench PRIVATE reaper)
target_link_libraries(reaper_cpp_bench PRIVATE reaper)
set_target_properties(reaper_cpp_bench PROPERTIES CXX_STANDARD 17)

if(MSVC)
    target_compile_options(reaper_bench PRIVATE /W4 /O2)
    target_compile_options(reaper_cpp_bench PRIVATE /W4 /O2)
else()
    target_compile_options(reaper_bench PRIVATE -Wall -Wextra -O2)
    target_compile_options(reaper_cpp_bench PRIVATE -Wall -Wextra -O2)
endif()
