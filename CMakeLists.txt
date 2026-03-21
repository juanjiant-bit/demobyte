cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    else()
        message(FATAL_ERROR "PICO_SDK_PATH no está definido")
    endif()
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(yuy0_simple_validation_v3 LANGUAGES C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico CACHE STRING "Pico board type")
endif()

pico_sdk_init()

add_executable(yuy0_simple_validation_v3
    main.cpp
    audio/audio_output_i2s.cpp
    io/adc_handler.cpp
    io/cap_pad_hybrid.cpp
    synth/simple_synth.cpp
)

pico_generate_pio_header(yuy0_simple_validation_v3
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/pcm5102_i2s.pio
)

target_include_directories(yuy0_simple_validation_v3 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(yuy0_simple_validation_v3
    pico_stdlib
    hardware_adc
    hardware_gpio
    hardware_timer
    hardware_pio
    hardware_clocks
)

target_compile_options(yuy0_simple_validation_v3 PRIVATE
    -O2
    -ffast-math
)

pico_enable_stdio_usb(yuy0_simple_validation_v3 1)
pico_enable_stdio_uart(yuy0_simple_validation_v3 0)

pico_add_extra_outputs(yuy0_simple_validation_v3)
