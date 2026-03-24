cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(demobyte_v3 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(demobyte_v3
    main.cpp
    synth/bytebeat_graph.cpp
)

pico_generate_pio_header(demobyte_v3
    ${CMAKE_CURRENT_LIST_DIR}/pcm5102_i2s.pio
)

target_include_directories(demobyte_v3 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/synth
    ${CMAKE_CURRENT_LIST_DIR}/utils
)

target_link_libraries(demobyte_v3
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_clocks
    hardware_adc
    hardware_timer
)

target_compile_options(demobyte_v3 PRIVATE -O2 -ffast-math)
pico_enable_stdio_usb(demobyte_v3 1)
pico_enable_stdio_uart(demobyte_v3 0)
pico_add_extra_outputs(demobyte_v3)
