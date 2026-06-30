# Pico USB Bluetooth HCI Wake

Firmware for a Raspberry Pi Pico-class board that exposes a native USB Bluetooth HCI dongle and PC wake controller.

Primary Pico 2 W path:

```text
PC USB host -> Pico TinyUSB Bluetooth HCI device -> onboard CYW43 Bluetooth controller
```

External ESP32 path:

```text
PC USB host -> Pico TinyUSB Bluetooth HCI device -> UART H4 -> ESP32 controller_hci_uart_esp32
```

## Current Focus

- Pico 2 W onboard CYW43 HCI backend.
- Pico + external ESP32 HCI UART backend retained for the original hardware.
- Native USB Bluetooth HCI class via TinyUSB's BTH device class.
- H4 UART parser for events, ACL, and SCO packet boundaries.
- USB command/event/ACL bridge.
- Standby HCI host scaffold for BLE advertisement wake.
- PC power-button pulse supervisor scaffold.
- Standby USB HID keyboard wake path for S3 resume without a power-button connection.

SCO/iso endpoints are left in the USB descriptor because TinyUSB's premade BTH descriptor includes the companion interface expected by the Bluetooth USB transport shape. The bundled TinyUSB BTH class exposes command, ACL, and event callbacks/APIs; this project keeps the H4 parser SCO-aware, but full SCO data pass-through will need either a TinyUSB BTH extension or a local BTH class driver with isochronous transfer callbacks.

## Pico 2 W Hardware Pins

The onboard wireless chip is connected internally. Do not use Pico 2 W GPIO `23`, `24`, `25`, or `29` for front-panel wiring; those are used by CYW43.

Recommended bench/front-panel pins:

| Signal | Pico 2 W GPIO | Notes |
| --- | ---: | --- |
| Power-button pulse | 10 | Released as input with pull-up; press briefly drives low, matching a switch that shorts 3.3 V to ground. |
| PC power LED sense | 11 | Input with pulldown; reads 3.3 V as PC-on. |
| USB VBUS sense | -1 | Disabled by default; USB is assumed present. |
| Status LED | -1 | Disabled by default; Pico 2 W onboard LED is not a normal GPIO. |

Build for the onboard CYW43 path:

```powershell
cd C:\Users\zheku\pico-usb-bt-wake
cmake -S . -B build-pico2w -G Ninja -DHCI_BACKEND=cyw43 -DPICO_BOARD=pico2_w -DPIN_PWR_BUTTON_OUT=10 -DPIN_PWR_OK_SENSE=11 -DPIN_USB_VBUS_SENSE=-1
cmake --build build-pico2w
```

## ESP32 Hardware Pins

The HCI pins match the existing repo defaults:

| Signal | Pico GPIO |
| --- | ---: |
| Pico UART TX -> ESP32 UART RX | 4 |
| Pico UART RX <- ESP32 UART TX | 5 |
| Pico CTS <- ESP32 RTS | 6 |
| Pico RTS -> ESP32 CTS | 7 |

UART defaults to `uart1`, `921600`, `8N1`, with RTS/CTS hardware flow control enabled.

The old SPI audio-offload pins may remain physically attached. This firmware does not configure GPIO `16-20` as SPI, so they remain idle unless you explicitly reuse them for supervisor pins.

## Build

```powershell
cd C:\Users\zheku\pico-usb-bt-wake
cmake -S . -B build -G Ninja -DHCI_BACKEND=cyw43 -DPICO_BOARD=pico2_w
cmake --build build
```

For the original Pico + ESP32 UART hardware:

```powershell
cmake -S . -B build-esp32 -G Ninja -DHCI_BACKEND=esp32_uart -DPICO_BOARD=pico2
cmake --build build-esp32
```

For descriptor-only bring-up:

```powershell
cmake -S . -B build-stub -G Ninja -DHCI_BACKEND=stub -DPICO_BOARD=pico2
cmake --build build-stub
```

If the Pico SDK is not found automatically, set `PICO_SDK_PATH`.

## Useful Options

- `-DHCI_BACKEND=esp32_uart`, `stub`, or `cyw43`
- `-DENABLE_CDC_DEBUG=ON`
- `-DENABLE_POWER_BUTTON_WAKE=ON` to allow automatic PC power-button pulses
- `-DENABLE_STANDBY_HID_KEYBOARD=ON` to expose a standby USB keyboard wake interface, enabled by default
- `-DSTANDBY_HID_WAKE_KEY=0x68` to choose the USB HID usage sent on wake; default is F13
- `-DENABLE_ACL_DEBUG_LOG=ON` for verbose ACL packet logging during transport debugging
- `-DENABLE_UART_FLOW_CONTROL=ON`
- `-DWAKE_ON_KNOWN_BLE_PEER=ON` to wake when a BLE peer learned while the host was on advertises again, enabled by default
- `-DWAKE_PERSIST_BLE_PEERS=ON` to keep learned BLE wake peers across power cycles, enabled by default
- `-DENABLE_BOOTSEL_CLEAR_WAKE_PEERS=ON` to clear learned BLE wake peers by holding BOOTSEL, enabled by default
- `-DWAKE_ON_BLE_DIRECTED_ADV=ON` to wake on BLE directed advertisements, enabled by default
- `-DWAKE_ON_STADIA_ADV=ON` to wake on Stadia Controller BLE advertisements/scan responses, enabled by default
- `-DWAKE_ON_CONNECTABLE_BLE_ADV=ON` to wake on any connectable BLE advertisement, noisy but useful for testing
- `-DWAKE_ON_BLE_HID=ON` to wake on BLE HID service/appearance advertisements
- `-DWAKE_ON_BLE_CONTROLLER_NAME=ON` to wake on controller-like BLE local names
- `-DWAKE_ON_ANY_BLE_ADV=ON`
- `-DPIN_ESP32_RESET=<gpio>`
- `-DPIN_PWR_OK_SENSE=<gpio>`
- `-DPIN_USB_VBUS_SENSE=<gpio>`
- `-DPIN_PWR_BUTTON_OUT=<gpio>`

By default, `PWR_OK` and USB VBUS are assumed present when their pins are `-1`, which makes bench USB dongle bring-up easier. The Pico 2 W front-panel build enables automatic power-button wake when `PIN_PWR_BUTTON_OUT` is set: it releases the pin with pull-up, simulates a press by driving it low for 200 ms, then waits 10 seconds before trusting the power LED/sense input again.

When `ENABLE_STANDBY_HID_KEYBOARD=ON`, the USB device stays connected in standby as a composite Bluetooth HCI plus boot-keyboard device with remote wake advertised. The Bluetooth bridge is disabled while the Pico owns the controller for standby scanning, and a matching wake event requests USB remote wake plus an F13 key press. If HID wake is disabled, the older behavior is used: when the power LED/sense input reads low, the firmware detaches USB while it runs the standby HCI scanner. For pure USB dongle testing, either hold the sense pin high or build with `-DPIN_PWR_OK_SENSE=-1`.

The default standby BLE wake policy targets devices trying to return to this adapter: classic Bluetooth connection requests, BLE directed advertisements, BLE advertisements from peers learned during earlier host-side BLE connections, and Stadia Controller advertisements whose local name starts with `Stadia`. Learned BLE peers are stored in flash so they survive power cycles. For a strict saved-address-only Stadia wake policy, disable `WAKE_ON_STADIA_ADV` after the controller has been learned. Broader BLE matching is available through the wake options above.

Hold the Pico BOOTSEL button for about 5 seconds to clear the persisted BLE wake peer list. This does not clear Windows or BlueZ Bluetooth pairing keys; it only resets the Pico's standby wake allowlist.

## Debug BOOTSEL Command

Debug CDC builds accept a serial command that jumps straight into the Pico USB bootloader:

```powershell
$p = New-Object System.IO.Ports.SerialPort 'COM15',115200,'None',8,'One'
$p.DtrEnable = $true
$p.RtsEnable = $true
$p.Open()
$p.WriteLine('bootsel')
Start-Sleep -Milliseconds 200
$p.Close()
```

Use the current debug COM port in place of `COM15`. The aliases `bootsel` and `boot` both work.

Debug CDC builds also enable the Pico SDK 1200-baud reset hook:

```powershell
$p = New-Object System.IO.Ports.SerialPort 'COM15',1200,'None',8,'One'
$p.Open()
Start-Sleep -Milliseconds 200
$p.Close()
```

## References

- [Linux HCI transport reference](docs/linux_hci_transport_reference.md)
