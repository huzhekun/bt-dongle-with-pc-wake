# Standby HCI Host Mode

When `PWR_OK` and USB VBUS sense indicate the PC is off, the Pico detaches USB and owns the ESP32 controller directly.

Initial sequence:

```text
reset controller
HCI Reset
Read Local Version Information
Read BD_ADDR
Set Event Mask
LE Set Event Mask
Write Scan Enable
LE Set Scan Parameters
LE Set Scan Enable
```

Wake detection currently supports:

```text
Classic Connection Request event
BLE Advertising Report from a peer learned while the host was on
BLE Directed Advertising Report event when WAKE_ON_BLE_DIRECTED_ADV=ON
BLE advertisement or scan response with a local name starting with "Stadia"
```

BLE peers learned from successful host-side LE connection events, and Stadia
advertisement matches, are kept in a small flash-backed wake allowlist. Hold
BOOTSEL for about 5 seconds to clear that allowlist.

BLE Advertising Report event when one of the broader debug filters is enabled:

- `WAKE_ON_CONNECTABLE_BLE_ADV=ON`
- `WAKE_ON_BLE_HID=ON`
- `WAKE_ON_BLE_CONTROLLER_NAME=ON`
- `WAKE_ON_ANY_BLE_ADV=ON`

Production wake policy should narrow BLE matching to known service UUIDs, manufacturer data, or another tested controller-specific signature.
