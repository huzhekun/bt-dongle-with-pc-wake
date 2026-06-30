# Linux HCI Transport Reference

This project is a USB-to-UART HCI transport bridge:

```text
Linux/Windows USB Bluetooth driver <-> Pico USB BTH class <-> UART H4 <-> ESP32 controller
```

Linux is useful as a reference because its USB and UART transports both register the same `hci_dev` core interface. The transport-specific code only decides how complete HCI packets move in and out.

## USB Path

Reference: Linux `btusb.c`

- Generic auto-binding is descriptor driven. `btusb_table` matches Bluetooth Controller class codes:
  - device match: `USB_DEVICE_INFO(0xe0, 0x01, 0x01)`
  - interface match: `USB_INTERFACE_INFO(0xe0, 0x01, 0x01)`
- During probe, `btusb` looks for the primary interrupt IN endpoint and bulk IN/OUT endpoints with `usb_find_common_endpoints`.
- Probe discovers Bluetooth USB endpoints, allocates an `hci_dev`, marks the bus as `HCI_USB`, installs `open`, `close`, `flush`, `send`, and then calls `hci_register_dev`.
- Open submits receive URBs for the interrupt event endpoint and bulk ACL IN endpoint.
- TX dispatch is packet-type based:
  - `HCI_COMMAND_PKT` goes to USB control transfer.
  - `HCI_ACLDATA_PKT` goes to USB bulk OUT.
  - `HCI_SCODATA_PKT` goes to USB isoch OUT.
  - `HCI_ISODATA_PKT` goes to USB isoch/ISO handling on newer controllers.
- RX reassembles complete HCI event, ACL, and SCO packets from USB transfers, tags the skb packet type, then passes complete packets to `hci_recv_frame`.

Relevant source anchors:

- `btusb_table` generic Bluetooth USB matches: https://github.com/torvalds/linux/blob/master/drivers/bluetooth/btusb.c#L71-L80
- `btusb_probe`: https://github.com/torvalds/linux/blob/master/drivers/bluetooth/btusb.c#L4076-L4205
- `hci_register_dev`: https://github.com/torvalds/linux/blob/master/drivers/bluetooth/btusb.c#L4438-L4441
- `btusb_open`: https://github.com/torvalds/linux/blob/master/drivers/bluetooth/btusb.c#L1993-L2029
- `btusb_send_frame`: https://github.com/torvalds/linux/blob/master/drivers/bluetooth/btusb.c#L2243-L2278
- Event/ACL/SCO RX framing: https://github.com/torvalds/linux/blob/master/drivers/bluetooth/btusb.c#L1248-L1464

## What BlueZ Expects

For a USB dongle, BlueZ does not normally parse the USB descriptors itself. The kernel `btusb` driver binds to the USB interface, creates an `hci_dev`, and exposes it as an HCI controller (`hci0`, `hci1`, etc.) through the kernel Bluetooth management API. BlueZ then sees and manages that HCI controller.

For a UART controller, BlueZ's `btattach` is only the attachment helper: it opens a serial port, sets the `N_HCI` line discipline, chooses a protocol such as H4, and asks the kernel for the created HCI device id. Once attached, the same kernel HCI core and BlueZ management path apply.

So auto-recognition for this project is primarily a Linux kernel USB descriptor problem:

- Device or interface class/subclass/protocol must be `0xe0/0x01/0x01`.
- The first Bluetooth interface must provide:
  - endpoint 0 class control requests for HCI commands,
  - interrupt IN for HCI events,
  - bulk OUT for host-to-controller ACL,
  - bulk IN for controller-to-host ACL.
- The optional second interface is for SCO isochronous bandwidth. Leaving it present but unused should not block command/event/ACL operation.
- The USB packets do not include H4 packet type bytes. Pico adds or strips H4 type bytes only on the ESP32 UART side.

## UART H4 Path

Reference: Linux `hci_ldisc.c`, `hci_serdev.c`, and `hci_h4.c`

There are two host attachment routes:

- TTY line discipline via BlueZ `btattach`, which sets `N_HCI` and selects a protocol such as `h4`.
- Serdev/device-tree registration for built-in UART controllers.

Both routes create an `hci_dev`, mark the bus as `HCI_UART`, and install `hci_uart_send_frame` as the core TX callback. H4 is then a protocol plugin:

- TX prepends the 1-byte H4 packet type before writing to UART.
- RX consumes the 1-byte H4 packet type, parses packet headers to find the payload length, and passes complete HCI packets to `hci_recv_frame`.

Relevant source anchors:

- BlueZ `btattach` switches the TTY to `N_HCI`, sets `HCIUARTSETPROTO`, and asks for the created HCI device id: https://github.com/bluez/bluez/blob/master/tools/btattach.c#L43-L122
- `hci_ldisc.c` TTY RX and HCI device registration: https://github.com/torvalds/linux/blob/master/drivers/bluetooth/hci_ldisc.c#L635-L724
- `hci_ldisc.c` protocol selection through `HCIUARTSETPROTO`: https://github.com/torvalds/linux/blob/master/drivers/bluetooth/hci_ldisc.c#L740-L824
- `hci_serdev.c` serdev HCI UART registration: https://github.com/torvalds/linux/blob/master/drivers/bluetooth/hci_serdev.c#L303-L373
- `hci_h4.c` H4 TX/RX plugin: https://github.com/torvalds/linux/blob/master/drivers/bluetooth/hci_h4.c#L87-L147
- `hci_h4.c` generic H4 receive framing: https://github.com/torvalds/linux/blob/master/drivers/bluetooth/hci_h4.c#L154-L274

## What This Means For Pico

For command/event/ACL, Pico should behave like Linux `btusb` on the USB side and like Linux `hci_h4` on the UART side:

| Direction | USB side | Pico bridge action | UART side |
| --- | --- | --- | --- |
| Host command TX | USB class control request | forward raw HCI command with H4 type `0x01` | ESP32 UART |
| Controller event RX | interrupt IN endpoint | strip H4 type `0x04`, send raw event | USB host |
| Host ACL TX | bulk OUT endpoint | forward raw ACL with H4 type `0x02` | ESP32 UART |
| Controller ACL RX | bulk IN endpoint | strip H4 type `0x02`, send raw ACL | USB host |

Important implementation checks:

- USB command/event/ACL packets do not include the H4 packet-type byte; UART H4 packets do.
- The bridge must preserve packet boundaries. It should not stream arbitrary bytes from one side to the other without re-framing.
- HCI events are not only "responses" to USB commands; controller-originated events must flow to USB at any time.
- ACL traffic can be larger than one USB packet or one UART ISR burst, so both sides need queues and packet-length-aware parsing.
- SCO/ISO can stay deferred. Linux clearly routes SCO over USB isoch endpoints, while H4 uses packet type `0x03`. Adding it later means adding TinyUSB BTH isoch callbacks and a SCO packet queue, not changing the command/event/ACL bridge shape.

## Current Descriptor Check

The current TinyUSB descriptor path follows the generic `btusb` match shape:

- Device descriptor class/subclass/protocol in `src/usb_descriptors.c` is `TUSB_CLASS_WIRELESS_CONTROLLER`, `TUD_BT_APP_SUBCLASS`, `TUD_BT_PROTOCOL_PRIMARY_CONTROLLER`.
- TinyUSB maps those to `0xe0`, `0x01`, `0x01`.
- `TUD_BTH_DESCRIPTOR` emits the Bluetooth interface association, primary interrupt/bulk interface, and companion isochronous interface.

