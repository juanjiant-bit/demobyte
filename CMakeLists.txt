cmake_minimum_required(VERSION 3.13)

# ── Pico SDK ──────────────────────────────────────────────────────────────────
if(NOT DEFINED PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    else()
        message(FATAL_ERROR "PICO_SDK_PATH no está definido. Exportalo con: export PICO_SDK_PATH=/ruta/al/sdk")
    endif()
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(byt3_validate VERSION 0.1 LANGUAGES C CXX ASM)
set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Board: pico para Ultimate Pi Pico (RP2040 estándar)
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico CACHE STRING "Pico board type")
endif()

pico_sdk_init()

# ── Sources mínimos ───────────────────────────────────────────────────────────
add_executable(byt3_validate
    main.cpp
    audio/audio_output_i2s.cpp
    audio/operators.cpp
    synth/bytebeat_graph.cpp
)

# ── PIO: solo I2S para PCM5102A (sin WS2812) ─────────────────────────────────
pico_generate_pio_header(byt3_validate
    ${CMAKE_CURRENT_SOURCE_DIR}/audio/pcm5102_i2s.pio
)

# ── Include dirs ──────────────────────────────────────────────────────────────
target_include_directories(byt3_validate PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

# ── Librerías SDK (solo lo que se usa) ────────────────────────────────────────
target_link_libraries(byt3_validate
    pico_stdlib
    pico_multicore
    hardware_adc
    hardware_gpio
    hardware_timer
    hardware_sync
    hardware_pio
    hardware_clocks
)

# ── Optimización ──────────────────────────────────────────────────────────────
target_compile_options(byt3_validate PRIVATE
    -O2
    -ffast-math
)

# ── USB serial para debug (uart deshabilitado) ────────────────────────────────
pico_enable_stdio_usb(byt3_validate 1)
pico_enable_stdio_uart(byt3_validate 0)

# ── Genera .uf2 .hex .bin ─────────────────────────────────────────────────────
pico_add_extra_outputs(byt3_validate)
