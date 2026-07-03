# Disable Tenda AIC8800 Bluetooth Only

The Tenda/AICSemi `2604:001f` adapter exposes both Bluetooth and Wi-Fi/vendor
USB interfaces:

- `3-1.3:1.0`, `3-1.3:1.1`: Bluetooth, bound by `btusb`
- `3-1.3:1.2`: vendor/Wi-Fi interface, bound by `aic8800_fdrv`

To keep the Wi-Fi/vendor interface while hiding only the Bluetooth controller
from BlueZ, install a small unbind helper plus a udev rule that runs after USB
enumeration. This is still needed even with the Wi-Fi-only AIC8800 DKMS package:
the Tenda device exposes standard Bluetooth USB interfaces, so the kernel
`btusb` driver can bind them independently of the AIC Wi-Fi driver.

Install the helper:

```sh
sudo install -D -m 0755 tools/disable-tenda-aic8800-bluetooth.sh /usr/local/libexec/disable-tenda-aic8800-bluetooth
```

Install the udev rule:

```udev
ACTION=="add|bind", SUBSYSTEM=="usb", ATTRS{idVendor}=="2604", ATTRS{idProduct}=="001f", ENV{DEVTYPE}=="usb_interface", RUN+="/usr/bin/systemd-run --no-block --property=Type=oneshot /bin/sh -c 'sleep 1; /usr/local/libexec/disable-tenda-aic8800-bluetooth'"
```

Install and apply:

```sh
printf '%s\n' 'ACTION=="add|bind", SUBSYSTEM=="usb", ATTRS{idVendor}=="2604", ATTRS{idProduct}=="001f", ENV{DEVTYPE}=="usb_interface", RUN+="/usr/bin/systemd-run --no-block --property=Type=oneshot /bin/sh -c '"'"'sleep 1; /usr/local/libexec/disable-tenda-aic8800-bluetooth'"'"'"' |
  sudo tee /etc/udev/rules.d/80-disable-tenda-aic8800-bluetooth.rules >/dev/null

sudo udevadm control --reload-rules
sudo /usr/local/libexec/disable-tenda-aic8800-bluetooth
```

Fish shell install/apply:

```fish
sudo install -D -m 0755 tools/disable-tenda-aic8800-bluetooth.sh /usr/local/libexec/disable-tenda-aic8800-bluetooth
printf '%s\n' 'ACTION=="add|bind", SUBSYSTEM=="usb", ATTRS{idVendor}=="2604", ATTRS{idProduct}=="001f", ENV{DEVTYPE}=="usb_interface", RUN+="/usr/bin/systemd-run --no-block --property=Type=oneshot /bin/sh -c '"'"'sleep 1; /usr/local/libexec/disable-tenda-aic8800-bluetooth'"'"'"' | sudo tee /etc/udev/rules.d/80-disable-tenda-aic8800-bluetooth.rules >/dev/null
sudo udevadm control --reload-rules
sudo /usr/local/libexec/disable-tenda-aic8800-bluetooth
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
sudo rm /usr/local/libexec/disable-tenda-aic8800-bluetooth
sudo udevadm control --reload-rules
```

Then unplug and replug the adapter, or rebind the current Bluetooth interfaces:

```sh
for iface in 3-1.3:1.0 3-1.3:1.1; do
  [ -e "/sys/bus/usb/devices/$iface" ] && echo "$iface" | sudo tee /sys/bus/usb/drivers/btusb/bind
done
```
