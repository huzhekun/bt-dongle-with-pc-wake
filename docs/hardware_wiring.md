# Hardware Wiring

## Pico 2 W Onboard CYW43 HCI

For the Pico 2 W path, do not connect an ESP32. The Bluetooth HCI controller is the Pico 2 W onboard CYW43 wireless chip, reached through the Pico SDK's low-level CYW43 HCI read/write API.

Avoid these Pico 2 W GPIOs for external wiring:

```text
GPIO23 WL_REG_ON
GPIO24 WL_DATA / HOST_WAKE
GPIO25 WL_CS
GPIO29 WL_CLOCK
```

Recommended front-panel GPIOs:

```text
GPIO10 power-button pulse output, released with pull-up, press drives low
GPIO11 PC power LED header sense input, pulldown, 3.3 V means PC-on
Pico 2 W onboard LED status output
```

GPIO10 is released as an input with pull-up, and simulates a power-button press by briefly driving low. That matches a front-panel switch line that is normally high and pressed by shorting to ground. The power-button output should still drive an isolated switch circuit when possible, not the motherboard header directly. The power LED header sense input should be protected/limited so the Pico pin only sees 0 V or 3.3 V.

Build example:

```powershell
cmake -S . -B build-pico2w -G Ninja -DHCI_BACKEND=cyw43 -DPICO_BOARD=pico2_w -DPIN_PWR_BUTTON_OUT=10 -DPIN_PWR_OK_SENSE=11 -DPIN_USB_VBUS_SENSE=-1
cmake --build build-pico2w
```

Automatic power-button wake is enabled by default when `PIN_PWR_BUTTON_OUT` is configured. The firmware waits 60 seconds after PC-off detection before arming standby Bluetooth scanning or wake pulses. During that wait the Pico 2 W onboard LED slow-blinks; once standby scanning is active it stays on; in normal USB Bluetooth dongle mode it is off. After a wake match, the firmware drives the power-button line low for 200 ms, releases it with pull-up, then waits 10 seconds before trusting the power LED/sense input again, so the PC has time to start and bring the LED/sense line high. Use `-DENABLE_POWER_BUTTON_WAKE=OFF` for bring-up if the power-button circuit or sense input is not ready. Use `-DSTANDBY_WAKE_ARM_DELAY_MS=<ms>` to tune the PC-off arm delay.

Standby HID keyboard wake is enabled by default with `-DENABLE_STANDBY_HID_KEYBOARD=ON`. In standby, USB remains connected as a composite Bluetooth HCI plus boot-keyboard device, but the Bluetooth bridge is disabled while the Pico runs the standby scanner. A matching wake event sends USB remote wake and an F13 key press, and also pulses GPIO10 if power-button wake is enabled. If standby HID wake is disabled, the firmware detaches USB while standby scanning is active. For pure USB dongle testing on another host, hold the sense pin at 3.3 V or build with `-DPIN_PWR_OK_SENSE=-1`.

## ESP32 Backend

The original Pico + ESP32 UART-HCI route is still supported, but it is not the main Pico 2 W hardware path. Its wiring and build notes live in [ESP32 HCI UART Backend](esp32_hci_uart.md).

## PC Wake Pins

Recommended future connections:

```text
PWR_OK sense       -> level-shifted Pico input
USB VBUS sense     -> Pico input, sense-only
Power-button pulse -> optocoupler or MOSFET circuit controlled by Pico output
```

Do not drive a motherboard power-button pin directly from a Pico GPIO.
