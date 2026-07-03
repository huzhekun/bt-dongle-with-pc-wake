# Pico 2 W USB Bluetooth HCI Wake Dongle

Firmware for a Raspberry Pi Pico 2 W that acts as a PC Bluetooth wake controller. By default the host keeps using its native Bluetooth adapter; the Pico only exposes a standby HID/CDC control interface and runs the Bluetooth controller during standby wake scanning. The main path uses the Pico 2 W's onboard CYW43 Bluetooth controller:

```text
PC USB host -> Pico TinyUSB Bluetooth HCI device -> onboard CYW43 Bluetooth controller
```

The original Pico + external ESP32 UART-HCI backend is still available, but it is now a secondary hardware path documented in [ESP32 HCI UART Backend](docs/esp32_hci_uart.md).

## What It Does

- Keeps the host Bluetooth data path on the native PC Bluetooth adapter by default.
- Provides an optional TinyUSB Bluetooth HCI bridge for descriptor/backend testing when `ENABLE_USB_BTH=ON`.
- Accepts a host-synced native adapter MAC address and paired-device allowlist over CDC for standby wake matching.
- Uses standby HCI host mode to scan for Bluetooth/BLE wake signals while the PC is off or asleep.
- Can pulse a PC power-button circuit for wake.
- Can expose a standby USB HID keyboard and send an F13 keypress for S3 wake.
- Persists the host-synced BLE wake peer allowlist in flash and lets you clear it by holding BOOTSEL.

SCO/iso endpoints are left in the USB descriptor because TinyUSB's premade BTH descriptor includes the companion interface expected by the Bluetooth USB transport shape. The bundled TinyUSB BTH class exposes command, ACL, and event callbacks/APIs; this project keeps packet parsing SCO-aware, but full SCO data pass-through will need either a TinyUSB BTH extension or a local BTH class driver with isochronous transfer callbacks.

## Pico 2 W Pins

The onboard wireless chip is connected internally. Do not use Pico 2 W GPIO `23`, `24`, `25`, or `29` for front-panel wiring; those are used by CYW43.

Recommended bench/front-panel pins:

| Signal | Pico 2 W GPIO | Notes |
| --- | ---: | --- |
| Power-button pulse | 10 | Released as input with pull-up; press briefly drives low, matching a switch that shorts 3.3 V to ground. |
| PC power LED sense | 11 | Input with pulldown; reads 3.3 V as PC-on. |
| USB VBUS sense | -1 | Disabled by default; USB is assumed present. |
| Status LED | Pico 2 W onboard LED | Blinks while standby wake is waiting to arm, solid while scanning, off in dongle mode. |

## Build

For the normal Pico 2 W front-panel build with native host Bluetooth handoff:

```powershell
cd C:\Users\zheku\pico-usb-bt-wake
cmake -S . -B build-pico2w -G Ninja -DHCI_BACKEND=cyw43 -DPICO_BOARD=pico2_w -DENABLE_USB_BTH=OFF -DENABLE_CDC_DEBUG=ON -DPIN_PWR_BUTTON_OUT=10 -DPIN_PWR_OK_SENSE=11 -DPIN_USB_VBUS_SENSE=-1
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
- `-DENABLE_USB_BTH=OFF` to avoid emulating a host Bluetooth dongle; this is now the default.
- `-DHCI_BACKEND=stub` for descriptor-only bring-up.
- `-DSYS_CLOCK_KHZ=200000` to run the RP2350 system clock at 200 MHz; default `0` keeps the board/SDK clock.
- `-DENABLE_CDC_DEBUG=ON` to expose USB CDC debug serial.
- `-DENABLE_POWER_BUTTON_WAKE=ON` to allow automatic PC power-button pulses.
- `-DENABLE_STANDBY_HID_KEYBOARD=ON` to expose a standby USB keyboard wake interface, enabled by default.
- `-DENABLE_CYW43_STATUS_LED=ON` to use the Pico W onboard LED for standby status, enabled by default for CYW43 builds.
- `-DSTANDBY_WAKE_ARM_DELAY_MS=60000` to delay standby Bluetooth scanning and wake pulses after PC-off detection; default is 60 seconds.
- `-DSTATUS_LED_BLINK_MS=1000` to set the status LED blink half-period while standby wake is waiting to arm.
- `-DSTANDBY_HID_WAKE_KEY=0x68` to choose the USB HID usage sent on wake; default is F13.
- `-DENABLE_ACL_DEBUG_LOG=ON` for verbose ACL packet logging during transport debugging.
- `-DWAKE_ON_KNOWN_BLE_PEER=ON` to wake when a host-synced paired BLE peer advertises again, enabled by default.
- `-DWAKE_PERSIST_BLE_PEERS=ON` to keep the host-synced BLE wake peer allowlist across power cycles, enabled by default.
- `-DENABLE_BOOTSEL_CLEAR_WAKE_PEERS=ON` to clear host-synced BLE wake peers by holding BOOTSEL, enabled by default.
- `-DWAKE_ON_BLE_DIRECTED_ADV=ON` to wake on BLE directed advertisements, enabled by default.
- `-DWAKE_ON_STADIA_ADV=ON` to wake on Stadia Controller BLE advertisements/scan responses.
- `-DWAKE_ON_CONNECTABLE_BLE_ADV=ON` to wake on any connectable BLE advertisement, noisy but useful for testing.
- `-DWAKE_ON_BLE_HID=ON` to wake on BLE HID service/appearance advertisements.
- `-DWAKE_ON_BLE_CONTROLLER_NAME=ON` to wake on controller-like BLE local names.
- `-DWAKE_ON_ANY_BLE_ADV=ON` for broad BLE wake testing.
- `-DPIN_PWR_OK_SENSE=<gpio>` for the PC-on sense input.
- `-DPIN_USB_VBUS_SENSE=<gpio>` for optional host VBUS sense.
- `-DPIN_PWR_BUTTON_OUT=<gpio>` for the isolated power-button pulse output.

The ESP32 UART backend has its own wiring and build options in [ESP32 HCI UART Backend](docs/esp32_hci_uart.md).

## Host Native Bluetooth Sync

Use [Host native Bluetooth wake sync](docs/host_native_bluetooth_sync.md) to install the systemd service that copies the native adapter address and the BlueZ pairing database device identities to the wake controller. After a wake event, the controller pulses the PC power button or, on USB-device-capable boards, sends the standby HID key; the host Bluetooth stack resumes on the PC's native adapter. The same serial sync idea works with ESP32-WROOM boards that only expose a USB-to-UART bridge, but those boards cannot provide USB HID or USB remote wake because the bridge chip is not native USB device hardware.

## Wake Behavior

By default, `PWR_OK` and USB VBUS are assumed present when their pins are `-1`, which makes bench USB dongle bring-up easier. The Pico 2 W front-panel build enables automatic power-button wake when `PIN_PWR_BUTTON_OUT` is set: it releases the pin with pull-up, simulates a press by driving it low for 200 ms, then waits 10 seconds before trusting the power LED/sense input again.

After the firmware detects that the PC is off, it waits `STANDBY_WAKE_ARM_DELAY_MS` before starting standby Bluetooth detection or allowing another wake pulse. The default is 60 seconds, which avoids immediately waking the PC again during shutdown or reboot transitions.

The Pico 2 W onboard LED is off in normal USB Bluetooth dongle mode, slow-blinks during that standby arm delay, and stays on once standby Bluetooth scanning is active.

When `ENABLE_STANDBY_HID_KEYBOARD=ON`, the USB device stays connected in standby as a composite Bluetooth HCI plus boot-keyboard device with remote wake advertised. The Bluetooth bridge is disabled while the Pico owns the controller for standby scanning, and a matching wake event requests USB remote wake plus an F13 key press. If HID wake is disabled, the older behavior is used: when the power LED/sense input reads low, the firmware detaches USB while it runs the standby HCI scanner. For pure USB dongle testing, either hold the sense pin high or build with `-DPIN_PWR_OK_SENSE=-1`.

The default standby BLE wake policy targets devices trying to return to this adapter: classic Bluetooth connection requests, BLE directed advertisements to the synced native adapter address, and BLE advertisements from peers synced from the host's paired-device database. The firmware does not learn or rotate this allowlist from observed Bluetooth traffic; rerunning the host sync helper sends the latest paired devices with `clear` plus `peer ...` commands. Broader BLE matching, including Stadia Controller local-name advertisements, is available through the wake options above.

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
