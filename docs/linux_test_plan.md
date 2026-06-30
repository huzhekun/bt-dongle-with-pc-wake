# Linux Test Plan

## Descriptor Bring-Up

Build with the CYW43 or stub backend and flash the UF2. For active Bluetooth descriptor testing, keep the power LED/sense pin high or disable it with `-DPIN_PWR_OK_SENSE=-1`; otherwise the firmware enters standby scan mode.

Use `ENABLE_CDC_DEBUG=OFF -DENABLE_STANDBY_HID_KEYBOARD=OFF` for the cleanest Bluetooth-only `btusb` check. The default standby-HID build is a composite device and should show both a Bluetooth interface and a boot-keyboard HID interface.

```bash
lsusb -v
dmesg -w
```

Expected:

```text
bInterfaceClass    0xe0 Wireless Controller
bInterfaceSubClass 0x01 RF Controller
bInterfaceProtocol 0x01 Bluetooth
default build also has a HID keyboard interface
btusb attempts to bind
```

## ESP32 UART Bring-Up

Flash the ESP32 with Espressif's `controller_hci_uart_esp32` or equivalent H4 controller firmware.

Build:

```bash
cmake -S . -B build -DHCI_BACKEND=esp32_uart -DPICO_BOARD=pico2
cmake --build build
```

Expected:

```text
BlueZ creates hciX
bluetoothctl list shows the adapter
bluetoothctl power on succeeds
bluetoothctl scan on shows BLE devices
```
