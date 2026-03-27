cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    else()
        message(FATAL_ERROR "PICO_SDK_PATH no está definido")
    endif()
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(dem0byt3_v13_clean LANGUAGES C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico CACHE STRING "Pico board type")
endif()

pico_sdk_init()

add_executable(dem0byt3_v13_clean
    main.cpp
    io/pads.cpp
    synth/bytebeat_engine.cpp
    drums/drum_engine.cpp
    master/master.cpp
    audio/audio_output_i2s.cpp
)

pico_generate_pio_header(dem0byt3_v13_clean
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/pcm5102_i2s.pio
)

target_include_directories(dem0byt3_v13_clean PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(dem0byt3_v13_clean
    pico_stdlib
    hardware_adc
    hardware_gpio
    hardware_pio
    hardware_clocks
    hardware_dma
    hardware_irq
)

target_compile_options(dem0byt3_v13_clean PRIVATE
    -O2
    -ffast-math
)

pico_enable_stdio_usb(dem0byt3_v13_clean 1)
pico_enable_stdio_uart(dem0byt3_v13_clean 0)
pico_add_extra_outputs(dem0byt3_v13_clean)
