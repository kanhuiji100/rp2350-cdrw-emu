cmake_minimum_required(VERSION 3.13)

# Local SDK by default; override with -DPICO_SDK_PATH=...
if (NOT DEFINED PICO_SDK_PATH AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/third_party/pico-sdk/pico_sdk_init.cmake")
    set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/third_party/pico-sdk" CACHE PATH "Pico SDK path")
endif()

if (NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2 CACHE STRING "Pico board target (pico2 / pico2_w / custom)")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/pico_sdk_import.cmake)
project(rp2350_cdrw_emulator VERSION 1.2.0 LANGUAGES C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_executable(cdrw_emulator
    src/main.c
    src/usb_descriptors.c
    src/usb_cdrom.c
    src/scsi.c
    src/media.c
    src/storage.c
    src/sd_spi.c
    src/oled.c
    src/encoder.c
    src/ui.c
    src/font5x7.c
    src/util.c
    third_party/fatfs/ff.c
    third_party/fatfs/ffunicode.c
    third_party/fatfs/diskio.c
)

target_include_directories(cdrw_emulator PRIVATE
    src
    third_party/fatfs
    third_party/pico-sdk/lib/tinyusb/src
)

target_compile_definitions(cdrw_emulator PRIVATE
    PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=0
    PICO_STDIO_USB_DEFAULT_CRLF=0
    PICO_STACK_SIZE=8192
    PICO_DEFAULT_UART_BAUD_RATE=115200
    CDRW_DEBUG_LOG=1
)

# Optional board overrides, e.g. -DCDRW_SD_CD=14 -DCDRW_OLED_ADDR=0x3D
foreach(cdrw_var CDRW_SD_CD CDRW_OLED_ADDR CDRW_SD_SPI_HZ CDRW_OLED_I2C_HZ)
    if (DEFINED ${cdrw_var} AND NOT "${${cdrw_var}}" STREQUAL "")
        target_compile_definitions(cdrw_emulator PRIVATE ${cdrw_var}=${${cdrw_var}})
    endif()
endforeach()

target_compile_options(cdrw_emulator PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -Wno-format
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
)

target_link_libraries(cdrw_emulator PRIVATE
    pico_stdlib
    pico_unique_id
    hardware_spi
    hardware_dma
    hardware_i2c
    hardware_gpio
    hardware_sync
    tinyusb_device
)

# We use native USB exclusively for the optical drive. Do not enable USB stdio.
set(CDRW_SD_READ_HZ "" CACHE STRING "Optional SD read clock override in Hz")
if(CDRW_SD_READ_HZ)
    target_compile_definitions(cdrw_emulator PRIVATE CDRW_SD_READ_HZ=${CDRW_SD_READ_HZ})
endif()

set(CDRW_SD_WRITE_HZ "" CACHE STRING "Optional SD write clock override in Hz")
if(CDRW_SD_WRITE_HZ)
    target_compile_definitions(cdrw_emulator PRIVATE CDRW_SD_WRITE_HZ=${CDRW_SD_WRITE_HZ})
endif()

option(CDRW_READ_TRACE "Log every READ and SD read timing" OFF)
if(CDRW_READ_TRACE)
    target_compile_definitions(cdrw_emulator PRIVATE CDRW_READ_TRACE=1)
endif()

pico_enable_stdio_usb(cdrw_emulator 0)
pico_enable_stdio_uart(cdrw_emulator 1)

pico_add_extra_outputs(cdrw_emulator)
