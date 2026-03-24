cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(demobyte_i2s_pot_pads C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(demobyte_i2s_pot_pads
    main.cpp
    io/cap_pad_handler.cpp
)

pico_generate_pio_header(demobyte_i2s_pot_pads
    ${CMAKE_CURRENT_LIST_DIR}/pcm5102_i2s.pio
)

target_link_libraries(demobyte_i2s_pot_pads
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_clocks
    hardware_adc
)

target_include_directories(demobyte_i2s_pot_pads PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/io
)

target_compile_options(demobyte_i2s_pot_pads PRIVATE -O2 -ffast-math)
pico_enable_stdio_usb(demobyte_i2s_pot_pads 1)
pico_enable_stdio_uart(demobyte_i2s_pot_pads 0)
pico_add_extra_outputs(demobyte_i2s_pot_pads)
