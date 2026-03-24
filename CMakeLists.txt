cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    else()
        message(FATAL_ERROR "PICO_SDK_PATH no está definido")
    endif()
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(bytebeat_machine VERSION 1.22 LANGUAGES C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico CACHE STRING "Pico board type")
endif()

pico_sdk_init()

# ── Shared source lists ───────────────────────────────────────────
set(AUDIO_SOURCES
    audio/audio_engine.cpp
    audio/operators.cpp
    audio/audio_output_pwm.cpp
    audio/audio_output_i2s.cpp
    audio/drums/drum_engine.cpp
)
set(SYNTH_SOURCES
    synth/bytebeat_graph.cpp
    synth/glide.cpp
    synth/quantizer.cpp
)
set(STATE_SOURCES
    state/state_manager.cpp
    state/flash_store.cpp
)
set(DSP_SOURCES
    dsp/dsp_chain.cpp
    dsp/stutter_fx.cpp
    dsp/reverb.cpp
)
set(SHARED_LIBS
    pico_stdlib
    pico_multicore
    hardware_pwm
    hardware_adc
    hardware_gpio
    hardware_timer
    hardware_sync
    hardware_irq
    hardware_flash
    hardware_uart
    hardware_pio
    hardware_clocks
    hardware_i2c
)

# ── bytebeat_machine (firmware completo) ─────────────────────────
add_executable(bytebeat_machine
    main.cpp
    ${AUDIO_SOURCES}
    ${SYNTH_SOURCES}
    ${STATE_SOURCES}
    ${DSP_SOURCES}
    sequencer/sequencer.cpp
    sequencer/clock_in.cpp
    sequencer/clock_out.cpp
    io/adc_handler.cpp
    io/cap_pad_handler.cpp
    io/encoder.cpp
    io/input_router.cpp
    midi/uart_midi.cpp
    midi/midi_router.cpp
    ui/oled_display.cpp
    ui/ui_renderer.cpp
)
pico_generate_pio_header(bytebeat_machine ${CMAKE_CURRENT_SOURCE_DIR}/led/ws2812.pio)
pico_generate_pio_header(bytebeat_machine ${CMAKE_CURRENT_SOURCE_DIR}/audio/pcm5102_i2s.pio)
target_include_directories(bytebeat_machine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(bytebeat_machine ${SHARED_LIBS})
target_compile_options(bytebeat_machine PRIVATE -O2 -ffast-math)
pico_enable_stdio_usb(bytebeat_machine 1)
pico_enable_stdio_uart(bytebeat_machine 0)
pico_add_extra_outputs(bytebeat_machine)

# ── bytebeat_machine_minimal (4 pads + 1 pot) ────────────────────
option(BYT3_MINIMAL_HW "Build the minimal prototype firmware" OFF)
if(BYT3_MINIMAL_HW)
    add_executable(bytebeat_machine_minimal
        main_minimal.cpp
        io/minimal_touch_pads.cpp
        ${AUDIO_SOURCES}
        ${SYNTH_SOURCES}
        ${STATE_SOURCES}
        ${DSP_SOURCES}
    )
    pico_generate_pio_header(bytebeat_machine_minimal ${CMAKE_CURRENT_SOURCE_DIR}/audio/pcm5102_i2s.pio)
    pico_generate_pio_header(bytebeat_machine_minimal ${CMAKE_CURRENT_SOURCE_DIR}/led/ws2812.pio)
    target_include_directories(bytebeat_machine_minimal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(bytebeat_machine_minimal ${SHARED_LIBS})
    target_compile_options(bytebeat_machine_minimal PRIVATE -O2 -ffast-math)
    pico_enable_stdio_usb(bytebeat_machine_minimal 1)
    pico_enable_stdio_uart(bytebeat_machine_minimal 0)
    pico_add_extra_outputs(bytebeat_machine_minimal)
endif()

# ── bytebeat_diag (diagnóstico por modos 1/2/3) ──────────────────
option(BYT3_DIAG "Build the diagnostic firmware" OFF)
if(BYT3_DIAG)
    add_executable(bytebeat_diag
        main_diag.cpp
        ${AUDIO_SOURCES}
        ${SYNTH_SOURCES}
        ${STATE_SOURCES}
        ${DSP_SOURCES}
    )
    pico_generate_pio_header(bytebeat_diag ${CMAKE_CURRENT_SOURCE_DIR}/audio/pcm5102_i2s.pio)
    pico_generate_pio_header(bytebeat_diag ${CMAKE_CURRENT_SOURCE_DIR}/led/ws2812.pio)
    target_include_directories(bytebeat_diag PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(bytebeat_diag ${SHARED_LIBS})
    target_compile_options(bytebeat_diag PRIVATE -O2 -ffast-math)
    pico_enable_stdio_usb(bytebeat_diag 1)
    pico_enable_stdio_uart(bytebeat_diag 0)
    pico_add_extra_outputs(bytebeat_diag)
endif()
