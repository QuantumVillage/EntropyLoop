cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(fiberranger_build2 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_executable(fiberranger_build2
    src/main.c
)

pico_generate_pio_header(fiberranger_build2
    ${CMAKE_CURRENT_LIST_DIR}/src/squarewave.pio
)

target_link_libraries(fiberranger_build2
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_clocks
    hardware_vreg
    hardware_adc
    hardware_spi
    hardware_dma
    hardware_gpio
    hardware_irq
    pico_mbedtls
)

pico_enable_stdio_usb(fiberranger_build2 1)
pico_enable_stdio_uart(fiberranger_build2 0)
pico_add_extra_outputs(fiberranger_build2)
