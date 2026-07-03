# Troubleshooting

## Linux Does Not Bind btusb

First confirm the device appears in `lsusb`. If it does not appear at all, check the power sense mode before chasing descriptors:

- With `PIN_PWR_OK_SENSE=11`, GP11 must read 3.3 V for the full USB Bluetooth bridge to be active.
- If GP11 is low or floating, the firmware assumes the PC is off and enters standby scan/wake mode. With `ENABLE_STANDBY_HID_KEYBOARD=ON`, USB remains connected for HID remote wake; with it off, USB disconnects.
- For pure Linux dongle testing, either hold GP11 high or build with `-DPIN_PWR_OK_SENSE=-1`.
- The non-debug `ENABLE_CDC_DEBUG=OFF -DENABLE_STANDBY_HID_KEYBOARD=OFF` build is the cleanest `btusb` test because it exposes only the Bluetooth controller function.

Check `lsusb -v` and confirm the Bluetooth interface class tuple:

```text
0xe0 / 0x01 / 0x01
```

Build the stub backend first to separate descriptor issues from controller/backend issues.

## ESP32 HCI UART Timeouts

For the secondary Pico + ESP32 UART-HCI route, first check [ESP32 HCI UART Backend](esp32_hci_uart.md). Verify the ESP32 controller firmware directly with a USB-UART adapter and `btattach` before inserting the Pico bridge.

Common causes:

- TX/RX swapped.
- ESP32 firmware not in controller-only HCI UART mode.
- Baud mismatch.
- Flow control enabled in firmware but not wired.

## Wake Pulses Repeatedly

The supervisor has a cooldown, but power-button hardware should still be tested with a dummy LED or meter before connecting a motherboard.

## Bluetooth Input Periodically Lags

For latency experiments, build with a modest RP2350 overclock:

```sh
cmake -S . -B build-pico2w-prod -G Ninja -DHCI_BACKEND=cyw43 -DPICO_BOARD=pico2_w -DPIN_PWR_BUTTON_OUT=10 -DPIN_PWR_OK_SENSE=11 -DPIN_USB_VBUS_SENSE=-1 -DSYS_CLOCK_KHZ=200000
cmake --build build-pico2w-prod
```

Leave `SYS_CLOCK_KHZ=0` to use the board/SDK default clock.
