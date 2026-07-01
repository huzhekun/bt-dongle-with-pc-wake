# Disable Tenda AIC8800 Bluetooth Only

The Tenda/AICSemi `2604:001f` adapter exposes both Bluetooth and Wi-Fi/vendor
USB interfaces:

- `3-1.3:1.0`, `3-1.3:1.1`: Bluetooth, bound by `btusb`
- `3-1.3:1.2`: vendor/Wi-Fi interface, bound by `aic8800_fdrv`

To keep the Wi-Fi/vendor interface while hiding only the Bluetooth controller
from BlueZ, install this udev rule:

```udev
ACTION=="add", SUBSYSTEM=="usb", DRIVERS=="btusb", ATTRS{idVendor}=="2604", ATTRS{idProduct}=="001f", RUN+="/bin/sh -c 'echo %k > /sys/bus/usb/drivers/btusb/unbind'"
```

Install and apply:

```sh
printf '%s\n' 'ACTION=="add", SUBSYSTEM=="usb", DRIVERS=="btusb", ATTRS{idVendor}=="2604", ATTRS{idProduct}=="001f", RUN+="/bin/sh -c '"'"'echo %k > /sys/bus/usb/drivers/btusb/unbind'"'"'"' |
  sudo tee /etc/udev/rules.d/80-disable-tenda-aic8800-bluetooth.rules >/dev/null

sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=2604 --attr-match=idProduct=001f
```

If the trigger does not detach the existing Bluetooth interfaces, unplug and
replug the Tenda adapter, or unbind the current interfaces once:

```sh
for iface in /sys/bus/usb/devices/*:*; do
  [ "$(cat "$iface/../idVendor" 2>/dev/null)" = "2604" ] || continue
  [ "$(cat "$iface/../idProduct" 2>/dev/null)" = "001f" ] || continue
  [ -e "$iface/driver" ] || continue
  [ "$(basename "$(readlink "$iface/driver")")" = "btusb" ] || continue
  echo "$(basename "$iface")" | sudo tee /sys/bus/usb/drivers/btusb/unbind
done
```

Verify:

```sh
bluetoothctl list
lsusb -t | rg '2604|AIC|btusb|aic8800'
```

Expected result: the `Tenda AIC 8800D80` no longer appears as a BlueZ
controller, and its `aic8800_fdrv` interface remains bound.

Rollback:

```sh
sudo rm /etc/udev/rules.d/80-disable-tenda-aic8800-bluetooth.rules
sudo udevadm control --reload-rules
```

Then unplug and replug the adapter, or rebind the current Bluetooth interfaces:

```sh
for iface in 3-1.3:1.0 3-1.3:1.1; do
  [ -e "/sys/bus/usb/devices/$iface" ] && echo "$iface" | sudo tee /sys/bus/usb/drivers/btusb/bind
done
```
