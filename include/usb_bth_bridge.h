#pragma once

#include <stdbool.h>

#include "hci_transport.h"

void usb_bth_bridge_init(const hci_transport_t *transport);
void usb_bth_bridge_set_enabled(bool enabled);
bool usb_bth_bridge_is_enabled(void);
void usb_bth_bridge_task(void);
void usb_bth_bridge_backend_packet(hci_packet_type_t type, const uint8_t *packet, uint16_t len);
