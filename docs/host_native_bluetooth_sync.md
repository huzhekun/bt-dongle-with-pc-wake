# Host native Bluetooth wake sync

This mode keeps the Pico wake controller out of the host Bluetooth data path. Build with
`-DENABLE_USB_BTH=OFF -DENABLE_CDC_DEBUG=ON` so the Pico exposes only the standby HID wake
keyboard plus the CDC control channel. The host continues to use its native Bluetooth adapter.

A systemd oneshot service runs `tools/bt-wake-sync.py`, reads the native adapter address with
`btmgmt info`, reads paired device addresses from BlueZ
(`/var/lib/bluetooth/<adapter>/<device>/info`), and also consults
`bluetoothctl paired-devices`, then sends them to the wake controller over CDC:

- `addr AA:BB:CC:DD:EE:FF` stores the PC adapter address used to match directed BLE wake attempts.
- `clear` replaces the old synced peer list.
- `peer AA:BB:CC:DD:EE:FF` appends a paired Bluetooth device identity address to the wake
  allowlist. The helper intentionally reads BlueZ pairing records, similar to Asahi Linux's
  Bluetooth sync flow, instead of trusting only currently visible devices.
- `save` is accepted as a synchronization barrier; flash writes are still debounced by firmware.

Install example:

```sh
sudo install -m 0755 tools/bt-wake-sync.py /usr/local/libexec/bt-wake-sync.py
sudo install -m 0644 systemd/bt-wake-sync.service /etc/systemd/system/bt-wake-sync.service
sudo install -m 0644 systemd/bt-wake-sync.path /etc/systemd/system/bt-wake-sync.path
sudo systemctl daemon-reload
sudo systemctl enable --now bt-wake-sync.path
sudo systemctl start bt-wake-sync.service
```

Adjust the serial path in the service if your board enumerates under a different
`/dev/serial/by-id` name. If your BlueZ state is not under `/var/lib/bluetooth`, pass
`--config /path/to/bluetooth` in the service file.

## ESP32-WROOM boards with USB-serial bridges

A typical ESP32-WROOM dev board whose USB connector is only a USB-to-UART bridge can use the
host-sync concept for the serial control channel, but it cannot provide every USB behavior this
Pico firmware uses:

- The USB-serial bridge can carry the same text commands (`addr`, `clear`, `peer`, `save`) from
  the systemd helper to firmware running on the ESP32.
- The ESP32-WROOM Bluetooth radio can scan during standby and can pulse the PC power-button
  circuit from a GPIO, so S5/soft-off wake through the front-panel header is practical.
- It cannot enumerate as a USB HID wake keyboard or issue USB remote wake, because ESP32-WROOM
  modules do not have native USB device hardware; the bridge chip only exposes a UART. Use a
  Pico/Pico 2 W, ESP32-S2/S3/C3-class part with native USB, or a separate USB-capable MCU if you
  need USB HID/remote-wake behavior.
- It also will not emulate the PC Bluetooth adapter to the host. That is fine for this native
  handoff flow: after the power-button wake, the PC's own Bluetooth adapter remains responsible
  for reconnecting paired devices.

In short, an ESP32-WROOM + USB-serial board is a good fit for "scan while the PC is off, then
pulse the power button". It is not a drop-in replacement for the Pico USB composite device path
unless the design only relies on serial sync and GPIO wake.
