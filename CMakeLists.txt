cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(rp2040_pcm5102_i2s_test C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(rp2040_pcm5102_i2s_test
    main.cpp
)

pico_generate_pio_header(rp2040_pcm5102_i2s_test
    ${CMAKE_CURRENT_LIST_DIR}/pcm5102_i2s.pio
)

target_link_libraries(rp2040_pcm5102_i2s_test
    pico_stdlib
    hardware_pio
    hardware_clocks
)

target_include_directories(rp2040_pcm5102_i2s_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(rp2040_pcm5102_i2s_test)
