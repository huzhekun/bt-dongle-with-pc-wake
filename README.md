# Pico 2 W USB Bluetooth HCI Wake Dongle

Firmware for a Raspberry Pi Pico 2 W that acts as a USB Bluetooth HCI dongle and PC wake controller. The main path uses the Pico 2 W's onboard CYW43 Bluetooth controller:

```text
PC USB host -> Pico TinyUSB Bluetooth HCI device -> onboard CYW43 Bluetooth controller
```

The original Pico + external ESP32 UART-HCI backend is still available, but it is now a secondary hardware path documented in [ESP32 HCI UART Backend](docs/esp32_hci_uart.md).

## What It Does

- Exposes a native USB Bluetooth HCI controller using TinyUSB's BTH device class.
- Bridges USB HCI command, event, and ACL traffic to the Pico 2 W onboard CYW43 Bluetooth controller.
- Uses standby HCI host mode to scan for Bluetooth/BLE wake signals while the PC is off or asleep.
- Can pulse a PC power-button circuit for wake.
- Can expose a standby USB HID keyboard and send an F13 keypress for S3 wake.
- Persists learned BLE wake peers in flash and lets you clear that allowlist by holding BOOTSEL.

SCO/iso endpoints are left in the USB descriptor because TinyUSB's premade BTH descriptor includes the companion interface expected by the Bluetooth USB transport shape. The bundled TinyUSB BTH class exposes command, ACL, and event callbacks/APIs; this project keeps packet parsing SCO-aware, but full SCO data pass-through will need either a TinyUSB BTH extension or a local BTH class driver with isochronous transfer callbacks.

## Pico 2 W Pins

The onboard wireless chip is connected internally. Do not use Pico 2 W GPIO `23`, `24`, `25`, or `29` for front-panel wiring; those are used by CYW43.

Recommended bench/front-panel pins:

| Signal | Pico 2 W GPIO | Notes |
| --- | ---: | --- |
| Power-button pulse | 10 | Released as input with pull-up; press briefly drives low, matching a switch that shorts 3.3 V to ground. |
| PC power LED sense | 11 | Input with pulldown; reads 3.3 V as PC-on. |
| USB VBUS sense | -1 | Disabled by default; USB is assumed present. |
| Status LED | -1 | Disabled by default; Pico 2 W onboard LED is not a normal GPIO. |

## Build

For the normal Pico 2 W front-panel build:

```powershell
cd C:\Users\zheku\pico-usb-bt-wake
cmake -S . -B build-pico2w -G Ninja -DHCI_BACKEND=cyw43 -DPICO_BOARD=pico2_w -DPIN_PWR_BUTTON_OUT=10 -DPIN_PWR_OK_SENSE=11 -DPIN_USB_VBUS_SENSE=-1
cmake --build build-pico2w
```

For pure USB Bluetooth descriptor bring-up, disable the power sense input so the dongle stays attached:

```powershell
cmake -S . -B build-pico2w-linux-test -G Ninja -DHCI_BACKEND=cyw43 -DPICO_BOARD=pico2_w -DPIN_PWR_OK_SENSE=-1 -DPIN_PWR_BUTTON_OUT=-1 -DENABLE_POWER_BUTTON_WAKE=OFF
cmake --build build-pico2w-linux-test
```

For descriptor-only firmware without a real Bluetooth controller backend:

```powershell
cmake -S . -B build-stub -G Ninja -DHCI_BACKEND=stub -DPICO_BOARD=pico2
cmake --build build-stub
```

If the Pico SDK is not found automatically, set `PICO_SDK_PATH`.

## Useful Options

- `-DHCI_BACKEND=cyw43` for the Pico 2 W onboard controller.
- `-DHCI_BACKEND=stub` for descriptor-only bring-up.
- `-DENABLE_CDC_DEBUG=ON` to expose USB CDC debug serial.
- `-DENABLE_POWER_BUTTON_WAKE=ON` to allow automatic PC power-button pulses.
- `-DENABLE_STANDBY_HID_KEYBOARD=ON` to expose a standby USB keyboard wake interface, enabled by default.
- `-DSTANDBY_WAKE_ARM_DELAY_MS=60000` to delay standby Bluetooth scanning and wake pulses after PC-off detection; default is 60 seconds.
- `-DSTANDBY_HID_WAKE_KEY=0x68` to choose the USB HID usage sent on wake; default is F13.
- `-DENABLE_ACL_DEBUG_LOG=ON` for verbose ACL packet logging during transport debugging.
- `-DWAKE_ON_KNOWN_BLE_PEER=ON` to wake when a BLE peer learned while the host was on advertises again, enabled by default.
- `-DWAKE_PERSIST_BLE_PEERS=ON` to keep learned BLE wake peers across power cycles, enabled by default.
- `-DENABLE_BOOTSEL_CLEAR_WAKE_PEERS=ON` to clear learned BLE wake peers by holding BOOTSEL, enabled by default.
- `-DWAKE_ON_BLE_DIRECTED_ADV=ON` to wake on BLE directed advertisements, enabled by default.
- `-DWAKE_ON_STADIA_ADV=ON` to wake on Stadia Controller BLE advertisements/scan responses, enabled by default.
- `-DWAKE_ON_CONNECTABLE_BLE_ADV=ON` to wake on any connectable BLE advertisement, noisy but useful for testing.
- `-DWAKE_ON_BLE_HID=ON` to wake on BLE HID service/appearance advertisements.
- `-DWAKE_ON_BLE_CONTROLLER_NAME=ON` to wake on controller-like BLE local names.
- `-DWAKE_ON_ANY_BLE_ADV=ON` for broad BLE wake testing.
- `-DPIN_PWR_OK_SENSE=<gpio>` for the PC-on sense input.
- `-DPIN_USB_VBUS_SENSE=<gpio>` for optional host VBUS sense.
- `-DPIN_PWR_BUTTON_OUT=<gpio>` for the isolated power-button pulse output.

The ESP32 UART backend has its own wiring and build options in [ESP32 HCI UART Backend](docs/esp32_hci_uart.md).

## Wake Behavior

By default, `PWR_OK` and USB VBUS are assumed present when their pins are `-1`, which makes bench USB dongle bring-up easier. The Pico 2 W front-panel build enables automatic power-button wake when `PIN_PWR_BUTTON_OUT` is set: it releases the pin with pull-up, simulates a press by driving it low for 200 ms, then waits 10 seconds before trusting the power LED/sense input again.

After the firmware detects that the PC is off, it waits `STANDBY_WAKE_ARM_DELAY_MS` before starting standby Bluetooth detection or allowing another wake pulse. The default is 60 seconds, which avoids immediately waking the PC again during shutdown or reboot transitions.

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

- [Hardware wiring](docs/hardware_wiring.md)
- [Linux HCI transport reference](docs/linux_hci_transport_reference.md)
- [Linux test plan](docs/linux_test_plan.md)
- [Standby HCI host mode](docs/standby_hci_host_mode.md)
- [Troubleshooting](docs/troubleshooting.md)
