# Reaper tests

add_executable(reaper_tests
    main.c
    harness.h
)

# test_reaper.c is included by main.c after the harness macros are defined.
# List it as a header-only source so IDEs still show it in the project tree.
set_source_files_properties(test_reaper.c PROPERTIES HEADER_FILE_ONLY ON)
target_sources(reaper_tests PRIVATE test_reaper.c)

target_link_libraries(reaper_tests PRIVATE reaper)

# Include the tests directory so test_reaper.c and harness.h resolve.
target_include_directories(reaper_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(reaper_cpp_tests
    main.cpp
    harness.h
)

# test_reaper_cpp.cpp is included by main.cpp after the harness macros are defined.
set_source_files_properties(test_reaper_cpp.cpp PROPERTIES HEADER_FILE_ONLY ON)
target_sources(reaper_cpp_tests PRIVATE test_reaper_cpp.cpp)

target_link_libraries(reaper_cpp_tests PRIVATE reaper)
target_include_directories(reaper_cpp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(reaper_cpp_tests PROPERTIES CXX_STANDARD 17)

# Be strict about warnings in our own test/bench/example code.
if(MSVC)
    target_compile_options(reaper_tests PRIVATE /W4)
    target_compile_options(reaper_cpp_tests PRIVATE /W4)
else()
    target_compile_options(reaper_tests PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(reaper_cpp_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME reaper_tests COMMAND reaper_tests)
add_test(NAME reaper_cpp_tests COMMAND reaper_cpp_tests)
