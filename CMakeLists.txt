cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    else()
        message(FATAL_ERROR "PICO_SDK_PATH no está definido")
    endif()
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(Dem0Byt3_V13_2 LANGUAGES C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico CACHE STRING "Pico board type")
endif()

pico_sdk_init()

add_executable(demobyte_v13_2
    main.cpp
    audio/audio_output_i2s.cpp
    io/pads.cpp
    synth/bytebeat_engine.cpp
    drums/drum_engine.cpp
    master/master.cpp
)

pico_generate_pio_header(demobyte_v13_2
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/pcm5102_i2s.pio
)

target_include_directories(demobyte_v13_2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(demobyte_v13_2
    pico_stdlib
    hardware_adc
    hardware_gpio
    hardware_pio
    hardware_clocks
)

target_compile_options(demobyte_v13_2 PRIVATE
    -O2
    -ffast-math
)

pico_enable_stdio_usb(demobyte_v13_2 0)
pico_enable_stdio_uart(demobyte_v13_2 0)
pico_add_extra_outputs(demobyte_v13_2)
