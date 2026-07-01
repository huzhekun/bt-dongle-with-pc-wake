cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PICO_BOARD pico2 CACHE STRING "Pico board type")

include(pico_sdk_import.cmake)

project(pico_usb_bt_wake C CXX ASM)
pico_sdk_init()

set(HCI_BACKEND "esp32_uart" CACHE STRING "HCI backend: esp32_uart, stub, cyw43")
set_property(CACHE HCI_BACKEND PROPERTY STRINGS esp32_uart stub cyw43)

option(ENABLE_USB_BTH "Expose TinyUSB Bluetooth HCI class" ON)
option(ENABLE_CDC_DEBUG "Expose USB CDC debug serial" OFF)
option(ENABLE_POWER_SUPERVISOR "Enable PC power supervisor state machine" ON)
option(ENABLE_STANDBY_HCI_HOST "Enable Pico-owned standby HCI host mode" ON)
option(ENABLE_POWER_BUTTON_WAKE "Allow firmware to pulse the PC power-button output for wake" ON)
option(ENABLE_STANDBY_HID_KEYBOARD "Expose a USB HID keyboard for standby S3 wake" ON)
option(ENABLE_USB_SCO "Expose SCO/iso endpoints required by TinyUSB BTH helper" OFF)
option(ENABLE_UART_FLOW_CONTROL "Enable ESP32 UART RTS/CTS" ON)
option(ENABLE_ACL_DEBUG_LOG "Log every ACL packet in CDC debug builds" OFF)
option(WAKE_ON_KNOWN_BLE_PEER "Wake when standby scan sees a BLE peer learned while the host was on" ON)
option(WAKE_PERSIST_BLE_PEERS "Persist the learned BLE wake peer list in flash" ON)
option(ENABLE_BOOTSEL_CLEAR_WAKE_PEERS "Hold BOOTSEL to clear the persisted BLE wake peer list" ON)
option(WAKE_ON_BLE_DIRECTED_ADV "Wake when standby scan sees BLE directed advertisements" ON)
option(WAKE_ON_STADIA_ADV "Wake when standby scan sees a Stadia Controller advertisement" ON)
option(WAKE_ON_CONNECTABLE_BLE_ADV "Wake when standby scan sees any connectable BLE advertisement" OFF)
option(WAKE_ON_BLE_HID "Wake when standby scan sees BLE HID/game-controller advertisements" OFF)
option(WAKE_ON_BLE_CONTROLLER_NAME "Wake when standby scan sees controller-like BLE local names" OFF)
option(WAKE_ON_ANY_BLE_ADV "Debug: wake on any BLE advertisement" OFF)

set(PIN_ESP32_UART_TX 4 CACHE STRING "Pico TX pin connected to ESP32 HCI RX")
set(PIN_ESP32_UART_RX 5 CACHE STRING "Pico RX pin connected to ESP32 HCI TX")
set(PIN_ESP32_UART_CTS 6 CACHE STRING "Pico CTS pin connected to ESP32 HCI RTS")
set(PIN_ESP32_UART_RTS 7 CACHE STRING "Pico RTS pin connected to ESP32 HCI CTS")
set(PIN_ESP32_RESET -1 CACHE STRING "Pico GPIO connected to ESP32 EN/reset, or -1")
set(PIN_PWR_OK_SENSE -1 CACHE STRING "Pico GPIO for ATX PWR_OK sense, or -1 to assume on")
set(PIN_USB_VBUS_SENSE -1 CACHE STRING "Pico GPIO for host USB VBUS sense, or -1 to assume present")
set(PIN_PWR_BUTTON_OUT -1 CACHE STRING "Pico GPIO for isolated PC power-button pulse, or -1")
set(PIN_LED_STATUS -1 CACHE STRING "Status LED GPIO, or -1")
set(PIN_LED_FAULT -1 CACHE STRING "Fault LED GPIO, or -1")

set(ESP32_HCI_UART_ID 1 CACHE STRING "UART peripheral used by ESP32 HCI transport")
set(ESP32_HCI_UART_BAUD 921600 CACHE STRING "ESP32 HCI UART baud rate")
set(HCI_ACL_MAX_LEN 2048 CACHE STRING "Maximum ACL packet bytes kept in firmware queues")
set(USB_BTH_EVENT_EPSIZE 64 CACHE STRING "USB HCI event interrupt endpoint size")
set(USB_BTH_DATA_EPSIZE 64 CACHE STRING "USB HCI ACL bulk endpoint size")
set(POWER_BUTTON_PULSE_MS 200 CACHE STRING "PC power-button pulse duration")
set(POST_WAKE_SENSE_SETTLE_MS 10000 CACHE STRING "Delay after power-button pulse before trusting power sense")
set(BOOTSEL_CLEAR_HOLD_MS 5000 CACHE STRING "BOOTSEL hold time for clearing persisted BLE wake peers")
set(STANDBY_HID_WAKE_KEY 0x68 CACHE STRING "USB HID key usage sent for standby wake, default F13")

if (NOT (ESP32_HCI_UART_ID STREQUAL "0" OR ESP32_HCI_UART_ID STREQUAL "1"))
    message(FATAL_ERROR "ESP32_HCI_UART_ID must be 0 or 1")
endif ()

if (HCI_BACKEND STREQUAL "cyw43" AND NOT TARGET pico_cyw43_arch_none)
    message(FATAL_ERROR "HCI_BACKEND=cyw43 requires a Pico W-class board such as -DPICO_BOARD=pico2_w")
endif ()

add_executable(pico-usb-bt-wake
    src/main.c
    src/usb_descriptors.c
    src/usb_bth_bridge.c
    src/h4_parser.c
    src/hci_host_minimal.c
    src/backend_stub.c
    src/power_supervisor.c
    src/wake_policy.c
    src/usb_hid_wake.c
    src/bootsel_button.c
    src/debug_log.c
)

if (ENABLE_USB_BTH)
    target_sources(pico-usb-bt-wake PRIVATE
        ${PICO_TINYUSB_PATH}/src/class/bth/bth_device.c
    )
endif ()

if (HCI_BACKEND STREQUAL "esp32_uart")
    target_sources(pico-usb-bt-wake PRIVATE src/backend_esp32_uart.c)
endif ()
if (HCI_BACKEND STREQUAL "cyw43")
    target_sources(pico-usb-bt-wake PRIVATE src/backend_cyw43.c)
endif ()

target_include_directories(pico-usb-bt-wake PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(pico-usb-bt-wake PRIVATE
    HCI_BACKEND_${HCI_BACKEND}=1
    ENABLE_USB_BTH=$<BOOL:${ENABLE_USB_BTH}>
    ENABLE_CDC_DEBUG=$<BOOL:${ENABLE_CDC_DEBUG}>
    PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=$<BOOL:${ENABLE_CDC_DEBUG}>
    PICO_STDIO_USB_RESET_MAGIC_BAUD_RATE=1200
    PICO_STDIO_USB_RESET_BOOTSEL_INTERFACE_DISABLE_MASK=0
    ENABLE_POWER_SUPERVISOR=$<BOOL:${ENABLE_POWER_SUPERVISOR}>
    ENABLE_STANDBY_HCI_HOST=$<BOOL:${ENABLE_STANDBY_HCI_HOST}>
    ENABLE_POWER_BUTTON_WAKE=$<BOOL:${ENABLE_POWER_BUTTON_WAKE}>
    ENABLE_STANDBY_HID_KEYBOARD=$<BOOL:${ENABLE_STANDBY_HID_KEYBOARD}>
    ENABLE_USB_SCO=$<BOOL:${ENABLE_USB_SCO}>
    ENABLE_UART_FLOW_CONTROL=$<BOOL:${ENABLE_UART_FLOW_CONTROL}>
    ENABLE_ACL_DEBUG_LOG=$<BOOL:${ENABLE_ACL_DEBUG_LOG}>
    WAKE_ON_KNOWN_BLE_PEER=$<BOOL:${WAKE_ON_KNOWN_BLE_PEER}>
    WAKE_PERSIST_BLE_PEERS=$<BOOL:${WAKE_PERSIST_BLE_PEERS}>
    ENABLE_BOOTSEL_CLEAR_WAKE_PEERS=$<BOOL:${ENABLE_BOOTSEL_CLEAR_WAKE_PEERS}>
    WAKE_ON_BLE_DIRECTED_ADV=$<BOOL:${WAKE_ON_BLE_DIRECTED_ADV}>
    WAKE_ON_STADIA_ADV=$<BOOL:${WAKE_ON_STADIA_ADV}>
    WAKE_ON_CONNECTABLE_BLE_ADV=$<BOOL:${WAKE_ON_CONNECTABLE_BLE_ADV}>
    WAKE_ON_BLE_HID=$<BOOL:${WAKE_ON_BLE_HID}>
    WAKE_ON_BLE_CONTROLLER_NAME=$<BOOL:${WAKE_ON_BLE_CONTROLLER_NAME}>
    WAKE_ON_ANY_BLE_ADV=$<BOOL:${WAKE_ON_ANY_BLE_ADV}>
    CFG_TUD_BTH=$<BOOL:${ENABLE_USB_BTH}>
    CFG_TUD_HID=$<BOOL:${ENABLE_STANDBY_HID_KEYBOARD}>
    CFG_TUD_BTH_ISO_ALT_COUNT=1
    CFG_TUD_BTH_EVENT_EPSIZE=${USB_BTH_EVENT_EPSIZE}
    CFG_TUD_BTH_DATA_EPSIZE=${USB_BTH_DATA_EPSIZE}
    PIN_ESP32_UART_TX=${PIN_ESP32_UART_TX}
    PIN_ESP32_UART_RX=${PIN_ESP32_UART_RX}
    PIN_ESP32_UART_CTS=${PIN_ESP32_UART_CTS}
    PIN_ESP32_UART_RTS=${PIN_ESP32_UART_RTS}
    PIN_ESP32_RESET=${PIN_ESP32_RESET}
    PIN_PWR_OK_SENSE=${PIN_PWR_OK_SENSE}
    PIN_USB_VBUS_SENSE=${PIN_USB_VBUS_SENSE}
    PIN_PWR_BUTTON_OUT=${PIN_PWR_BUTTON_OUT}
    PIN_LED_STATUS=${PIN_LED_STATUS}
    PIN_LED_FAULT=${PIN_LED_FAULT}
    ESP32_HCI_UART_ID=${ESP32_HCI_UART_ID}
    ESP32_HCI_UART_BAUD=${ESP32_HCI_UART_BAUD}
    HCI_ACL_MAX_LEN=${HCI_ACL_MAX_LEN}
    POWER_BUTTON_PULSE_MS=${POWER_BUTTON_PULSE_MS}
    POST_WAKE_SENSE_SETTLE_MS=${POST_WAKE_SENSE_SETTLE_MS}
    BOOTSEL_CLEAR_HOLD_MS=${BOOTSEL_CLEAR_HOLD_MS}
    STANDBY_HID_WAKE_KEY=${STANDBY_HID_WAKE_KEY}
)

if (HCI_BACKEND STREQUAL "cyw43")
    target_compile_definitions(pico-usb-bt-wake PRIVATE
        CYW43_ENABLE_BLUETOOTH=1
    )
endif ()

pico_enable_stdio_uart(pico-usb-bt-wake 1)
pico_enable_stdio_usb(pico-usb-bt-wake ${ENABLE_CDC_DEBUG})

target_link_libraries(pico-usb-bt-wake
    pico_stdlib
    hardware_gpio
    hardware_uart
    hardware_flash
    tinyusb_device
    tinyusb_board
    pico_flash
)

if (HCI_BACKEND STREQUAL "cyw43")
    target_link_libraries(pico-usb-bt-wake
        pico_cyw43_arch_none
    )
endif ()

pico_set_program_name(pico-usb-bt-wake "pico-usb-bt-wake")
pico_set_program_version(pico-usb-bt-wake "0.1.0")
pico_add_extra_outputs(pico-usb-bt-wake)
