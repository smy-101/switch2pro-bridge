# sw2bridge — Switch 2 Pro Controller → DS4 bridge firmware for Pico 2 W
cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)

project(sw2bridge C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(sw2bridge
    main.c
    log.c
    usb_descriptors.c
    sw2_ble.c
    sw2_decode.c
    ds4.c
)

target_include_directories(sw2bridge PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(sw2bridge
    pico_stdlib
    pico_cyw43_arch_none
    pico_btstack_ble
    pico_btstack_cyw43
    pico_btstack_run_loop_async_context
    hardware_watchdog
    tinyusb_device
    tinyusb_board
)

# Bring-up defaults: verbose scanning + accept any Switch 2 controller family
# advertisement so first-contact logs are maximally informative on the Pro.
target_compile_definitions(sw2bridge PRIVATE
    SW2_SCAN_DEBUG=1
    SW2_ACCEPT_ANY_NS2=1
)

pico_add_extra_outputs(sw2bridge)
