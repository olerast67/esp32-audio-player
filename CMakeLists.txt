# SPDX-License-Identifier: Apache-2.0
# esp32-audio-player
#
# ESP-IDF: this directory is the component (Component Registry: olerast67/esp32-audio-player).
# Host (Linux, macOS, Windows): add_subdirectory() gives the static library `audio_player`
# with the portable sources only; test/ builds the host tests on top of it.

set(AUDIO_PLAYER_CORE_DIRS src/util src/dsp src/decoders src/library src/player)
# Decoders built from the vendored code expect two's-complement wrap on corrupt input.
set(AUDIO_PLAYER_WRAPV_SOURCES src/decoders/dec_flac.c src/decoders/dec_mp3.c src/decoders/dec_vorbis.c)

if(ESP_PLATFORM)
    if(IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR LESS 3)
        set(_ap_drivers driver)
    else()
        set(_ap_drivers esp_driver_i2s esp_driver_gpio)
    endif()
    idf_component_register(SRC_DIRS ${AUDIO_PLAYER_CORE_DIRS} src/port
                           INCLUDE_DIRS src
                           PRIV_REQUIRES ${_ap_drivers} esp_timer esp_hw_support heap)
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-parameter -Wno-missing-field-initializers)
    set_source_files_properties(${AUDIO_PLAYER_WRAPV_SOURCES} PROPERTIES COMPILE_OPTIONS -fwrapv)
    return()
endif()

cmake_minimum_required(VERSION 3.16)
project(esp32_audio_player C)

set(_ap_sources)
foreach(_dir ${AUDIO_PLAYER_CORE_DIRS})
    file(GLOB _dir_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/${_dir}/*.c)
    list(APPEND _ap_sources ${_dir_sources})
endforeach()
# Pure-C parts of the ESP32 port (clock planning, output FIFO): portable and host-tested.
list(APPEND _ap_sources ${CMAKE_CURRENT_LIST_DIR}/src/port/i2s_plan.c ${CMAKE_CURRENT_LIST_DIR}/src/port/i2s_fifo.c)

add_library(audio_player STATIC ${_ap_sources})
target_include_directories(audio_player PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
set_target_properties(audio_player PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
foreach(_f ${AUDIO_PLAYER_WRAPV_SOURCES})
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/${_f} PROPERTIES COMPILE_OPTIONS -fwrapv)
endforeach()
if(NOT MSVC)
    target_compile_options(audio_player PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()
if(NOT WIN32)
    target_link_libraries(audio_player PUBLIC m)
endif()
