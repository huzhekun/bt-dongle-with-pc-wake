#!/bin/sh
set -eu

ACTION_AUTO_CONNECT=2

if ! command -v bluetoothctl >/dev/null 2>&1; then
	echo "bluetoothctl not found" >&2
	exit 1
fi

if ! command -v btmgmt >/dev/null 2>&1; then
	echo "btmgmt not found" >&2
	exit 1
fi

bluetoothctl devices Paired | while IFS= read -r line; do
	case "$line" in
		Device\ ??\:??\:??\:??\:??\:??\ *)
			addr=${line#Device }
			addr=${addr%% *}
			;;
		*)
			continue
			;;
	esac

	info=$(bluetoothctl info "$addr" 2>/dev/null || true)

	printf '%s\n' "$info" | grep -q '^	Paired: yes$' || continue
	printf '%s\n' "$info" | grep -q '^	Blocked: no$' || continue

	if printf '%s\n' "$info" | grep -q "^Device $addr (public)$"; then
		addr_type=1
	elif printf '%s\n' "$info" | grep -q "^Device $addr (random)$"; then
		addr_type=2
	else
		echo "Skipping $addr: no LE public/random address type in bluetoothctl info"
		continue
	fi

	name=$(printf '%s\n' "$info" | sed -n 's/^	Name: //p' | head -n 1)
	if [ -n "$name" ]; then
		label="$addr $name"
	else
		label="$addr"
	fi

	echo "Adding mgmt auto-connect for $label (type $addr_type)"
	if ! btmgmt add-device -a "$ACTION_AUTO_CONNECT" -t "$addr_type" "$addr"; then
		echo "Failed to add mgmt auto-connect for $label" >&2
	fi
done
