cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    else()
        message(FATAL_ERROR "PICO_SDK_PATH no está definido")
    endif()
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(pcm5102_probe_min C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico CACHE STRING "Pico board type")
endif()

pico_sdk_init()

add_executable(pcm5102_probe_min
    main.cpp
)

pico_generate_pio_header(pcm5102_probe_min
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_i2s.pio
)

target_include_directories(pcm5102_probe_min PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(pcm5102_probe_min
    pico_stdlib
    hardware_pio
    hardware_clocks
)

pico_enable_stdio_usb(pcm5102_probe_min 1)
pico_enable_stdio_uart(pcm5102_probe_min 0)

pico_add_extra_outputs(pcm5102_probe_min)
