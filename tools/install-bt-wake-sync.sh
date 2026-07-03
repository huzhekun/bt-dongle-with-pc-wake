#!/usr/bin/env sh
set -eu

serial=""
adapter="0"
config="/var/lib/bluetooth"
start_now=1

usage() {
    cat <<EOF
Usage: $0 [options]

Install the host native Bluetooth wake sync helper and systemd units.

Options:
  --serial PATH     CDC serial device path for the wake controller
                   default: auto-detect under /dev/serial/by-id
  --adapter INDEX   Bluetooth adapter index for btmgmt
                   default: $adapter
  --config DIR      BlueZ config directory
                   default: $config
  --no-start        Install and enable the path unit, but do not run an initial sync
  -h, --help        Show this help
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --serial)
            [ "$#" -ge 2 ] || { echo "missing value for --serial" >&2; exit 2; }
            serial="$2"
            shift 2
            ;;
        --adapter)
            [ "$#" -ge 2 ] || { echo "missing value for --adapter" >&2; exit 2; }
            adapter="$2"
            shift 2
            ;;
        --config)
            [ "$#" -ge 2 ] || { echo "missing value for --config" >&2; exit 2; }
            config="$2"
            shift 2
            ;;
        --no-start)
            start_now=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [ "$(id -u)" -ne 0 ]; then
    echo "run this installer as root, for example: sudo $0" >&2
    exit 1
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "missing required command: $1" >&2
        exit 1
    fi
}

require_cmd install
require_cmd python3
require_cmd systemctl
require_cmd bluetoothctl

if ! command -v btmgmt >/dev/null 2>&1; then
    echo "warning: btmgmt not found; sync will rely on bluetoothctl for adapter discovery" >&2
fi

tmp_compile=$(mktemp)
BT_WAKE_SYNC_HELPER="$repo_dir/tools/bt-wake-sync.py" BT_WAKE_SYNC_PYC="$tmp_compile" python3 -c 'import os, py_compile; py_compile.compile(os.environ["BT_WAKE_SYNC_HELPER"], cfile=os.environ["BT_WAKE_SYNC_PYC"], doraise=True)'
rm -f "$tmp_compile"

detect_serial() {
    found=""
    found_list=""
    seen=" "
    count=0

    for path in \
        /dev/serial/by-id/*Codex*Bluetooth*USB*Wake*Device* \
        /dev/serial/by-id/*Bluetooth*USB*Wake*Device* \
        /dev/serial/by-id/*Pico*Wake*
    do
        [ -e "$path" ] || continue
        case "$seen" in
            *" $path "*) continue ;;
        esac
        seen="${seen}${path} "
        found="$path"
        found_list="${found_list}  $path
"
        count=$((count + 1))
    done

    if [ "$count" -eq 1 ]; then
        printf '%s\n' "$found"
        return 0
    fi

    if [ "$count" -eq 0 ]; then
        echo "could not auto-detect the wake controller CDC serial device under /dev/serial/by-id" >&2
        echo "plug in the flashed board, make sure it was built with -DENABLE_CDC_DEBUG=ON, or pass --serial PATH" >&2
        exit 1
    fi

    echo "multiple matching wake controller serial devices found; pass --serial PATH explicitly:" >&2
    printf '%s' "$found_list" >&2
    exit 1
}

if [ -z "$serial" ]; then
    serial=$(detect_serial)
    echo "Auto-detected serial device: $serial"
fi

install -m 0755 "$repo_dir/tools/bt-wake-sync.py" /usr/local/libexec/bt-wake-sync.py

tmp_service=$(mktemp)
trap 'rm -f "$tmp_service"' EXIT

cat >"$tmp_service" <<EOF
[Unit]
Description=Sync native Bluetooth wake peers to Pico wake controller
After=bluetooth.service
Requires=bluetooth.service

[Service]
Type=oneshot
TimeoutStartSec=30
ExecStart=/usr/local/libexec/bt-wake-sync.py --serial $serial --adapter $adapter --config $config
EOF

install -m 0644 "$tmp_service" /etc/systemd/system/bt-wake-sync.service
install -m 0644 "$repo_dir/systemd/bt-wake-sync.path" /etc/systemd/system/bt-wake-sync.path

systemctl daemon-reload
systemctl enable --now bt-wake-sync.path

if [ "$start_now" -eq 1 ]; then
    systemctl start bt-wake-sync.service
fi

echo "Installed bt-wake-sync.service and bt-wake-sync.path"
echo "Serial: $serial"
echo "Adapter: $adapter"
echo "BlueZ config: $config"
