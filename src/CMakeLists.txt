cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(pico_entropy_gen C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(entropy_gen
    main.c
    health_tests.c
)

# 1. Generate PIO header
pico_generate_pio_header(entropy_gen ${CMAKE_CURRENT_SOURCE_DIR}/squarewave.pio)

# 2. Tell the compiler where to find "mbedtls_config.h" (THIS IS THE FIX)
target_include_directories(entropy_gen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 3. Link libraries
target_link_libraries(entropy_gen
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_adc
    hardware_vreg
    hardware_clocks
    pico_mbedtls
)

pico_enable_stdio_usb(entropy_gen 1)
pico_enable_stdio_uart(entropy_gen 0)

pico_add_extra_outputs(entropy_gen)