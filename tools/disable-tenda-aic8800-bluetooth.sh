#!/bin/sh
set -eu

for iface in /sys/bus/usb/devices/*:*; do
	[ "$(cat "$iface/../idVendor" 2>/dev/null || true)" = "2604" ] || continue
	[ "$(cat "$iface/../idProduct" 2>/dev/null || true)" = "001f" ] || continue
	[ -e "$iface/driver" ] || continue
	[ "$(basename "$(readlink "$iface/driver")")" = "btusb" ] || continue
	echo "$(basename "$iface")" > /sys/bus/usb/drivers/btusb/unbind
done
