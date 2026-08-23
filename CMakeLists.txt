cmake_minimum_required(VERSION 3.15)

include(proj.conf)

if(DEFINED ENV{FORCE_FS})
    unset(CONFIG_USB_HS)
endif()

find_package(bouffalo_sdk REQUIRED HINTS $ENV{BL_SDK_BASE})

sdk_set_main_file(src/main.c)
target_sources(app PRIVATE
    src/board_status.c
    src/pro2_ble.c
    src/pro2_protocol.c
    src/pro2_rumble.c
    src/pro2_settings.c
    src/usb_nintendo.c
    src/vendor_protocol.c
)

sdk_add_include_directories(src)
sdk_add_include_directories(${BL_SDK_BASE}/components/wireless/bluetooth/blestack/src/include)
sdk_add_include_directories(${BL_SDK_BASE}/components/wireless/bluetooth/blestack/src/port/include)

if(DEFINED ENV{FORCE_FS})
    target_compile_definitions(app PRIVATE FORCE_FS_MODE=1)
    message(STATUS "USB mode: Full-Speed")
else()
    message(STATUS "USB mode: High-Speed")
endif()

target_compile_options(app PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

project(pro2dongle_bl616)
