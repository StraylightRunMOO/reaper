# Reaper examples

add_executable(reaper_basic basic.c)
add_executable(reaper_cooperative cooperative.c)
add_executable(reaper_basic_cpp basic.cpp)
add_executable(reaper_cooperative_cpp cooperative.cpp)

target_link_libraries(reaper_basic PRIVATE reaper)
target_link_libraries(reaper_cooperative PRIVATE reaper)
target_link_libraries(reaper_basic_cpp PRIVATE reaper)
target_link_libraries(reaper_cooperative_cpp PRIVATE reaper)

set_target_properties(reaper_basic_cpp reaper_cooperative_cpp
    PROPERTIES CXX_STANDARD 17)

if(MSVC)
    target_compile_options(reaper_basic PRIVATE /W4)
    target_compile_options(reaper_cooperative PRIVATE /W4)
    target_compile_options(reaper_basic_cpp PRIVATE /W4)
    target_compile_options(reaper_cooperative_cpp PRIVATE /W4)
else()
    target_compile_options(reaper_basic PRIVATE -Wall -Wextra)
    target_compile_options(reaper_cooperative PRIVATE -Wall -Wextra)
    target_compile_options(reaper_basic_cpp PRIVATE -Wall -Wextra)
    target_compile_options(reaper_cooperative_cpp PRIVATE -Wall -Wextra)
endif()
