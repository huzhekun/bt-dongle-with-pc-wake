# Troubleshooting

## Linux Does Not Bind btusb

First confirm the device appears in `lsusb`. If it does not appear at all, check the power sense mode before chasing descriptors:

- With `PIN_PWR_OK_SENSE=11`, GP11 must read 3.3 V for the USB Bluetooth adapter to be connected.
- If GP11 is low or floating, the firmware assumes the PC is off, disconnects USB, and enters standby scan/wake mode.
- For pure Linux dongle testing, either hold GP11 high or build with `-DPIN_PWR_OK_SENSE=-1`.
- The non-debug `ENABLE_CDC_DEBUG=OFF` build is the cleanest `btusb` test because it exposes only the Bluetooth controller function.

Check `lsusb -v` and confirm the Bluetooth interface class tuple:

```text
0xe0 / 0x01 / 0x01
```

Build the stub backend first to separate descriptor issues from UART/controller issues.

## HCI Timeouts

Verify the ESP32 controller firmware directly with a USB-UART adapter and `btattach` before inserting the Pico bridge.

Common causes:

- TX/RX swapped.
- ESP32 firmware not in controller-only HCI UART mode.
- Baud mismatch.
- Flow control enabled in firmware but not wired.

## Wake Pulses Repeatedly

The supervisor has a cooldown, but power-button hardware should still be tested with a dummy LED or meter before connecting a motherboard.
