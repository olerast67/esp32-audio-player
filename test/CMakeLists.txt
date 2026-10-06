# SPDX-License-Identifier: Apache-2.0
# Host tests: the portable core (decoders, DSP, library, player engine) and the pure-C parts of
# the ESP32 port, built with the system C compiler. No ESP-IDF needed.
#
#   cmake -S test -B build/test -G Ninja
#   cmake --build build/test
#   ctest --test-dir build/test --output-on-failure
#
# Windows without a C compiler: python -m pip install ziglang cmake ninja, then add
#   -DCMAKE_TOOLCHAIN_FILE=<absolute path>/cmake/zig-toolchain.cmake
# Sanitizers (Linux/macOS with GCC or Clang): -DAUDIO_PLAYER_SANITIZE=ON
cmake_minimum_required(VERSION 3.16)
project(esp32_audio_player_tests C)

option(AUDIO_PLAYER_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(AUDIO_PLAYER_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. audio_player)

set(TESTS
    test_base_util
    test_decoders_api
    test_decoders_vectors
    test_decoders_fuzz
    test_dsp_chain
    test_dsp_eq
    test_dsp_resampler
    test_library_browse
    test_library_index
    test_library_perf
    test_library_tags
    test_library_text
    test_player_engine
    test_player_queue
    test_player_state
    test_port_i2s)

enable_testing()
foreach(t ${TESTS})
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE audio_player)
    target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
    target_compile_definitions(${t} PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_LIST_DIR}/data")
    if(NOT MSVC)
        target_compile_options(${t} PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
        # The tests build paths with snprintf into fixed buffers on purpose; GCC flags each one.
        if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${t} PRIVATE -Wno-format-truncation)
        endif()
    endif()
    add_test(NAME ${t} COMMAND ${t} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()
# The fuzz pass and the 5000-file library benchmark take longest.
set_tests_properties(test_decoders_fuzz test_library_perf PROPERTIES TIMEOUT 900)

# Host command-line player: plays files through the real engine into a WAV file.
add_executable(player_cli ${CMAKE_CURRENT_LIST_DIR}/../tools/player_cli/player_cli.c)
target_link_libraries(player_cli PRIVATE audio_player)
target_include_directories(player_cli PRIVATE ${CMAKE_CURRENT_LIST_DIR}/support)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME gapless_cli
             COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:player_cli> -DPYTHON=${Python3_EXECUTABLE}
                     -DDATA=${CMAKE_CURRENT_LIST_DIR}/data -DTOOLS=${CMAKE_CURRENT_LIST_DIR}/../tools
                     -DWORK=${CMAKE_CURRENT_BINARY_DIR}/gapless_cli -P ${CMAKE_CURRENT_LIST_DIR}/gapless_cli.cmake)
endif()
