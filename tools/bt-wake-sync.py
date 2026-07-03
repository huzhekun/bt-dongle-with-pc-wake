#!/usr/bin/env python3
"""Sync the host Bluetooth adapter identity and paired devices to the wake controller."""
import argparse
import configparser
import fcntl
import os
import re
import subprocess
import sys
import time
from pathlib import Path

MAC_RE = re.compile(r"\b([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})\b")
BLUEZ_CONFIG = Path("/var/lib/bluetooth")
COMMAND_TIMEOUT_SECONDS = 10


def run(cmd):
    return subprocess.check_output(
        cmd,
        text=True,
        stderr=subprocess.STDOUT,
        timeout=COMMAND_TIMEOUT_SECONDS,
    )


def normalize_mac(mac):
    m = MAC_RE.search(mac)
    if not m:
        raise ValueError(f"invalid Bluetooth address: {mac}")
    return m.group(1).upper()


def sysfs_adapter_mac(adapter):
    if not str(adapter).isdigit():
        raise ValueError("sysfs adapter lookup requires a numeric adapter index")

    path = Path("/sys/class/bluetooth") / f"hci{adapter}" / "address"
    return normalize_mac(path.read_text(encoding="ascii"))


def bluetoothctl_adapter_macs():
    out = run(["bluetoothctl", "list"])
    return [normalize_mac(m.group(1)) for m in MAC_RE.finditer(out)]


def bluetoothctl_adapter_mac(adapter):
    if MAC_RE.fullmatch(str(adapter)):
        return normalize_mac(adapter)

    adapters = bluetoothctl_adapter_macs()
    if str(adapter).isdigit() and int(adapter) < len(adapters):
        return adapters[int(adapter)]

    out = run(["bluetoothctl", "show"])
    m = re.search(r"\bController\s+([0-9A-Fa-f:]{17})\b", out)
    if m:
        return normalize_mac(m.group(1))

    raise RuntimeError("could not find adapter address in bluetoothctl output")


def btmgmt_adapter_mac(adapter):
    out = run(["btmgmt", "--index", str(adapter), "info"])
    m = re.search(r"\baddr\s+([0-9A-Fa-f:]{17})\b", out)
    if not m:
        m = MAC_RE.search(out)
    if not m:
        raise RuntimeError("could not find adapter address in btmgmt output")
    return normalize_mac(m.group(1))


def native_adapter_mac(adapter):
    for lookup in (sysfs_adapter_mac, bluetoothctl_adapter_mac, btmgmt_adapter_mac):
        try:
            return lookup(adapter)
        except (FileNotFoundError, ValueError, RuntimeError, subprocess.CalledProcessError,
                subprocess.TimeoutExpired):
            continue
    raise RuntimeError(f"could not determine Bluetooth adapter address for adapter {adapter}")


def bluetoothctl_paired_device_macs():
    try:
        out = run(["bluetoothctl", "devices", "Paired"])
    except subprocess.CalledProcessError:
        out = run(["bluetoothctl", "paired-devices"])
    return {normalize_mac(m.group(1)) for m in MAC_RE.finditer(out)}


def bluez_info_has_pairing_material(info_path):
    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str
    parser.read(info_path)
    return any(
        parser.has_section(section)
        for section in (
            "LinkKey",
            "LongTermKey",
            "PeripheralLongTermKey",
            "SlaveLongTermKey",
            "IdentityResolvingKey",
        )
    )


def bluez_paired_device_macs(config_dir, adapter_mac):
    adapter_dir = Path(config_dir) / adapter_mac
    if not adapter_dir.is_dir():
        return set()

    peers = set()
    for child in adapter_dir.iterdir():
        if not child.is_dir():
            continue
        try:
            mac = normalize_mac(child.name)
        except ValueError:
            continue
        info_path = child / "info"
        if info_path.is_file() and bluez_info_has_pairing_material(info_path):
            peers.add(mac)
    return peers


def paired_device_macs(config_dir, adapter_mac):
    peers = bluez_paired_device_macs(config_dir, adapter_mac)
    try:
        peers.update(bluetoothctl_paired_device_macs())
    except FileNotFoundError:
        if not peers:
            raise
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        if not peers:
            raise
    return sorted(peers)


def write_line(dev, line):
    dev.write((line + "\n").encode("ascii"))
    dev.flush()
    time.sleep(0.05)


def open_serial(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags & ~os.O_NONBLOCK)
    return os.fdopen(fd, "r+b", buffering=0)


def sync(serial_path, adapter, config_dir, dry_run=False):
    local = native_adapter_mac(adapter)
    peers = paired_device_macs(config_dir, local)
    commands = [f"addr {local}", "clear"] + [f"peer {mac}" for mac in peers] + ["save"]
    if dry_run:
        print("\n".join(commands))
        return
    with open_serial(serial_path) as dev:
        for command in commands:
            write_line(dev, command)
    print(f"synced adapter {local} and {len(peers)} paired device(s) to {serial_path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", default="/dev/serial/by-id/usb-Codex_Bluetooth_USB_Wake_Device_0009-if00", help="wake-controller CDC serial device")
    parser.add_argument("--adapter", default="0", help="btmgmt adapter index")
    parser.add_argument("--config", default=str(BLUEZ_CONFIG), help="BlueZ config directory")
    parser.add_argument("--dry-run", action="store_true", help="print commands instead of writing serial")
    args = parser.parse_args()
    try:
        sync(args.serial, args.adapter, args.config, args.dry_run)
    except subprocess.CalledProcessError as exc:
        print(exc.output, file=sys.stderr, end="")
        return exc.returncode or 1
    except subprocess.TimeoutExpired as exc:
        print(f"bt-wake-sync: command timed out: {' '.join(exc.cmd)}", file=sys.stderr)
        return 1
    except Exception as exc:
        print(f"bt-wake-sync: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
