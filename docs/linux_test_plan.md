# Linux Test Plan

## Descriptor Bring-Up

Build with the CYW43 or stub backend and flash the UF2. For descriptor testing, keep the power LED/sense pin high or disable it with `-DPIN_PWR_OK_SENSE=-1`; otherwise the firmware intentionally disconnects USB in standby wake mode.

Use `ENABLE_CDC_DEBUG=OFF` for the cleanest `btusb` check.

```bash
lsusb -v
dmesg -w
```

Expected:

```text
bInterfaceClass    0xe0 Wireless Controller
bInterfaceSubClass 0x01 RF Controller
bInterfaceProtocol 0x01 Bluetooth
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
