cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(dem0byt3_v2 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_executable(dem0byt3_v2
    main.cpp
    audio/drums/drum_engine.cpp
)

pico_generate_pio_header(dem0byt3_v2
    ${CMAKE_CURRENT_LIST_DIR}/pcm5102_i2s.pio
)

target_include_directories(dem0byt3_v2 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(dem0byt3_v2
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_clocks
    hardware_adc
    hardware_timer
)

target_compile_options(dem0byt3_v2 PRIVATE -O2 -ffast-math)
pico_enable_stdio_usb(dem0byt3_v2 0)  # OFF: USB IRQs interfieren con timing de pads
pico_enable_stdio_uart(dem0byt3_v2 0)
pico_add_extra_outputs(dem0byt3_v2)
