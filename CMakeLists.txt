cmake_minimum_required(VERSION 3.10)
project(rgb2yuv C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ----- Library -----
add_library(rgb2yuv_lib STATIC src/rgb2yuv.c)
target_include_directories(rgb2yuv_lib PUBLIC include)
target_link_libraries(rgb2yuv_lib PRIVATE m)

if(NOT MSVC)
    target_compile_options(rgb2yuv_lib PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

# ----- CLI tool -----
add_executable(rgb2yuv_cli src/main.c)
target_link_libraries(rgb2yuv_cli PRIVATE rgb2yuv_lib m)
target_include_directories(rgb2yuv_cli PRIVATE third_party)
set_target_properties(rgb2yuv_cli PROPERTIES OUTPUT_NAME rgb2yuv)

if(NOT MSVC)
    target_compile_options(rgb2yuv_cli PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion)
endif()

# ----- Tests -----
enable_testing()
add_executable(test_rgb2yuv tests/test_rgb2yuv.c)
target_link_libraries(test_rgb2yuv PRIVATE rgb2yuv_lib m)
add_test(NAME unit_tests COMMAND test_rgb2yuv)

if(NOT MSVC)
    target_compile_options(test_rgb2yuv PRIVATE -Wall -Wextra)
endif()

# ----- Benchmark -----
add_executable(bench_rgb2yuv tests/bench_rgb2yuv.c)
target_link_libraries(bench_rgb2yuv PRIVATE rgb2yuv_lib m)

if(NOT MSVC)
    target_compile_options(bench_rgb2yuv PRIVATE -Wall -Wextra -O2)
endif()

# ----- Release optimization -----
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT MSVC)
    target_compile_options(rgb2yuv_lib PRIVATE -O2)
    target_compile_options(bench_rgb2yuv PRIVATE -O3 -ftree-vectorize)
endif()
