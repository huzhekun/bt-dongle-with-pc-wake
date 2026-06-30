#include "usb_bth_bridge.h"

#include "debug_log.h"
#include "tusb.h"

#include "pico/time.h"

#include <string.h>

typedef struct {
    hci_packet_type_t type;
    uint16_t len;
    uint8_t data[HCI_ACL_MAX_LEN];
} hci_queue_item_t;

#define USB_QUEUE_DEPTH 32u

static const hci_transport_t *bridge_transport;
static bool bridge_enabled;
static hci_queue_item_t usb_queue[USB_QUEUE_DEPTH];
static volatile uint8_t usb_head;
static volatile uint8_t usb_tail;
static hci_queue_item_t backend_queue[USB_QUEUE_DEPTH];
static volatile uint8_t backend_head;
static volatile uint8_t backend_tail;
static volatile bool backend_tx_in_flight;
static uint32_t backend_tx_started_ms;
static uint32_t dropped_vendor_events;
static uint8_t usb_acl_buf[HCI_ACL_MAX_LEN];
static uint16_t usb_acl_len;
static uint16_t usb_acl_expected_len;
static uint32_t dropped_acl_fragments;

static uint16_t acl_packet_total_len(const uint8_t *packet) {
    return (uint16_t)(4u + (uint16_t)packet[2] + ((uint16_t)packet[3] << 8u));
}

static void log_acl_packet(const char *prefix, const uint8_t *packet, uint16_t len) {
#if ENABLE_CDC_DEBUG && ENABLE_ACL_DEBUG_LOG
    if (len >= 4u) {
        uint16_t handle_flags = (uint16_t)packet[0] | ((uint16_t)packet[1] << 8u);
        uint16_t handle = (uint16_t)(handle_flags & 0x0fffu);
        uint8_t pb = (uint8_t)((handle_flags >> 12u) & 0x03u);
        uint16_t data_len = (uint16_t)packet[2] | ((uint16_t)packet[3] << 8u);
        debug_log("%s acl handle=0x%04x pb=%u data_len=%u len=%u", prefix, handle, pb, data_len, len);
    }
#else
    (void)prefix;
    (void)packet;
    (void)len;
#endif
}

static void log_hci_command(const char *prefix, const uint8_t *packet, uint16_t len) {
#if ENABLE_CDC_DEBUG
    if (len >= 3) {
        uint16_t opcode = (uint16_t)packet[0] | ((uint16_t)packet[1] << 8u);
        debug_log("%s cmd opcode=0x%04x plen=%u q=%u/%u", prefix, opcode, packet[2], usb_tail, usb_head);
    }
#else
    (void)prefix;
    (void)packet;
    (void)len;
#endif
}

static void log_hci_event(const char *prefix, const uint8_t *packet, uint16_t len) {
#if ENABLE_CDC_DEBUG
    if (len < 2) return;
    if (packet[0] == HCI_EVENT_COMMAND_COMPLETE && len >= 6) {
        uint16_t opcode = (uint16_t)packet[3] | ((uint16_t)packet[4] << 8u);
        debug_log("%s evt CC opcode=0x%04x status=0x%02x len=%u", prefix, opcode, packet[5], len);
    } else if (packet[0] == HCI_EVENT_COMMAND_STATUS && len >= 6) {
        uint16_t opcode = (uint16_t)packet[4] | ((uint16_t)packet[5] << 8u);
        debug_log("%s evt CS opcode=0x%04x status=0x%02x len=%u", prefix, opcode, packet[2], len);
    } else if (packet[0] == 0x07u && len >= 255u) {
        debug_log("%s evt remote_name_complete status=0x%02x len=%u", prefix, packet[2], len);
    } else if (packet[0] == 0x13u) {
        return;
    } else if (packet[0] == 0x2fu && len >= 257u) {
        debug_log("%s evt extended_inquiry_result len=%u", prefix, len);
    } else if (packet[0] == 0x3du && len >= 16u) {
        debug_log("%s evt remote_host_supported_features len=%u", prefix, len);
    } else if (packet[0] == 0x03u && len >= 13u) {
        uint16_t handle = (uint16_t)packet[3] | ((uint16_t)packet[4] << 8u);
        debug_log("%s evt connection_complete status=0x%02x handle=0x%04x link=0x%02x enc=0x%02x",
                  prefix, packet[2], handle, packet[11], packet[12]);
    } else if (packet[0] == 0x05u && len >= 6u) {
        uint16_t handle = (uint16_t)packet[3] | ((uint16_t)packet[4] << 8u);
        debug_log("%s evt disconnect_complete status=0x%02x handle=0x%04x reason=0x%02x",
                  prefix, packet[2], handle, packet[5]);
    } else if (packet[0] == 0x06u && len >= 5u) {
        uint16_t handle = (uint16_t)packet[3] | ((uint16_t)packet[4] << 8u);
        debug_log("%s evt auth_complete status=0x%02x handle=0x%04x", prefix, packet[2], handle);
    } else if (packet[0] == 0x16u) {
        debug_log("%s evt pin_code_request", prefix);
    } else if (packet[0] == 0x17u) {
        debug_log("%s evt link_key_request", prefix);
    } else if (packet[0] == 0x18u && len >= 25u) {
        debug_log("%s evt link_key_notification type=0x%02x", prefix, packet[24]);
    } else if (packet[0] == 0x2cu && len >= 5u) {
        uint16_t handle = (uint16_t)packet[3] | ((uint16_t)packet[4] << 8u);
        debug_log("%s evt sync_connection_complete status=0x%02x handle=0x%04x", prefix, packet[2], handle);
    } else if (packet[0] == HCI_EVENT_LE_META && len >= 21u && packet[2] == 0x01u) {
        uint16_t handle = (uint16_t)packet[4] | ((uint16_t)packet[5] << 8u);
        debug_log("%s evt LE connection_complete status=0x%02x handle=0x%04x role=0x%02x",
                  prefix, packet[3], handle, packet[6]);
    } else if (packet[0] == HCI_EVENT_LE_META && len >= 21u && packet[2] == 0x0au) {
        uint16_t handle = (uint16_t)packet[4] | ((uint16_t)packet[5] << 8u);
        debug_log("%s evt LE enhanced_connection_complete status=0x%02x handle=0x%04x role=0x%02x",
                  prefix, packet[3], handle, packet[6]);
    } else if (packet[0] == HCI_EVENT_LE_META && len >= 12u && packet[2] == 0x03u) {
        uint16_t handle = (uint16_t)packet[4] | ((uint16_t)packet[5] << 8u);
        debug_log("%s evt LE connection_update status=0x%02x handle=0x%04x", prefix, packet[3], handle);
    } else if (packet[0] == HCI_EVENT_LE_META && len >= 13u && packet[2] == 0x04u) {
        uint16_t handle = (uint16_t)packet[4] | ((uint16_t)packet[5] << 8u);
        debug_log("%s evt LE read_remote_features status=0x%02x handle=0x%04x", prefix, packet[3], handle);
    } else if (packet[0] == HCI_EVENT_LE_META && len >= 13u && packet[2] == 0x07u) {
        uint16_t handle = (uint16_t)packet[3] | ((uint16_t)packet[4] << 8u);
        debug_log("%s evt LE data_length_change handle=0x%04x", prefix, handle);
    } else if (packet[0] == HCI_EVENT_LE_META && len >= 3u && packet[2] == 0x02u) {
        return;
    } else if (packet[0] == HCI_EVENT_LE_META && len >= 3u) {
        debug_log("%s evt LE subevent=0x%02x plen=%u len=%u", prefix, packet[2], packet[1], len);
    } else {
        debug_log("%s evt code=0x%02x plen=%u len=%u", prefix, packet[0], packet[1], len);
    }
#else
    (void)prefix;
    (void)packet;
    (void)len;
#endif
}

static bool queue_push(hci_queue_item_t *queue, volatile uint8_t *head, volatile uint8_t *tail,
                       hci_packet_type_t type, const uint8_t *packet, uint16_t len) {
    if (len > HCI_ACL_MAX_LEN) return false;
    uint8_t next = (uint8_t)((*head + 1u) % USB_QUEUE_DEPTH);
    if (next == *tail) return false;
    queue[*head].type = type;
    queue[*head].len = len;
    if (len) memcpy(queue[*head].data, packet, len);
    *head = next;
    return true;
}

static bool queue_pop(hci_queue_item_t *queue, volatile uint8_t *head, volatile uint8_t *tail,
                      hci_queue_item_t *item) {
    if (*tail == *head) return false;
    *item = queue[*tail];
    *tail = (uint8_t)((*tail + 1u) % USB_QUEUE_DEPTH);
    return true;
}

static bool queue_command_complete(uint16_t opcode, uint8_t status) {
    uint8_t event[] = {
        HCI_EVENT_COMMAND_COMPLETE,
        0x04,
        0x01,
        (uint8_t)(opcode & 0xffu),
        (uint8_t)(opcode >> 8u),
        status,
    };
    return queue_push(backend_queue, &backend_head, &backend_tail, HCI_PKT_EVENT, event, sizeof(event));
}

static bool hci_command_opcode_is(const uint8_t *packet, uint16_t len, uint16_t opcode) {
    if (len < 3u) return false;
    uint16_t packet_opcode = (uint16_t)packet[0] | ((uint16_t)packet[1] << 8u);
    return packet_opcode == opcode;
}

static void usb_bth_bridge_after_controller_send_quirk(const uint8_t *packet, uint16_t len) {
#if HCI_BACKEND_cyw43
    if (hci_command_opcode_is(packet, len, HCI_OPCODE_WRITE_SIMPLE_PAIRING_MODE)) {
        // The CYW43 firmware used here floods vendor events and never completes
        // this host-mode command. A normal success completion lets BTHUSB continue.
        debug_log("CYW43 quirk: sent Write Simple Pairing Mode, synthesize complete");
        if (!queue_command_complete(HCI_OPCODE_WRITE_SIMPLE_PAIRING_MODE, 0x00)) {
            debug_log("USB bridge backend queue full");
        }
    }
#else
    (void)packet;
    (void)len;
#endif
}

void usb_bth_bridge_init(const hci_transport_t *transport) {
    bridge_transport = transport;
}

void usb_bth_bridge_set_enabled(bool enabled) {
    bridge_enabled = enabled;
    if (!enabled) {
        usb_head = usb_tail = 0;
        backend_head = backend_tail = 0;
        backend_tx_in_flight = false;
        backend_tx_started_ms = 0;
        usb_acl_len = 0;
        usb_acl_expected_len = 0;
    }
}

bool usb_bth_bridge_is_enabled(void) {
    return bridge_enabled;
}

void usb_bth_bridge_backend_packet(hci_packet_type_t type, const uint8_t *packet, uint16_t len) {
    if (!bridge_enabled) return;
    if (type != HCI_PKT_EVENT && type != HCI_PKT_ACL) return;

    if (type == HCI_PKT_EVENT && len >= 2 && packet[0] == HCI_EVENT_VENDOR_SPECIFIC && packet[1] >= 64u) {
        dropped_vendor_events++;
#if ENABLE_CDC_DEBUG
        if ((dropped_vendor_events & 0x0fu) == 1u) {
            debug_log("drop noisy vendor evt len=%u count=%lu", len, dropped_vendor_events);
        }
#endif
        return;
    }

    if (!queue_push(backend_queue, &backend_head, &backend_tail, type, packet, len)) {
        debug_log("USB bridge backend queue full");
    }
}

static void usb_bth_bridge_reset_acl_reassembly(void) {
    usb_acl_len = 0;
    usb_acl_expected_len = 0;
}

static void usb_bth_bridge_push_completed_acl(void) {
    log_acl_packet("USB->BT", usb_acl_buf, usb_acl_len);
    if (!queue_push(usb_queue, &usb_head, &usb_tail, HCI_PKT_ACL, usb_acl_buf, usb_acl_len)) {
        debug_log("USB ACL OUT queue full");
    }
    usb_bth_bridge_reset_acl_reassembly();
}

static void usb_bth_bridge_acl_out_fragment(const uint8_t *fragment, uint16_t len) {
    while (len > 0u) {
        if (usb_acl_len == 0u && len < 4u) {
            dropped_acl_fragments++;
            debug_log("drop short ACL OUT fragment len=%u count=%lu", len, dropped_acl_fragments);
            return;
        }

        if (usb_acl_len == 0u) {
            usb_acl_expected_len = acl_packet_total_len(fragment);
            if (usb_acl_expected_len < 4u || usb_acl_expected_len > sizeof(usb_acl_buf)) {
                dropped_acl_fragments++;
                debug_log("drop bad ACL OUT packet expected=%u count=%lu", usb_acl_expected_len, dropped_acl_fragments);
                usb_bth_bridge_reset_acl_reassembly();
                return;
            }
        }

        uint16_t remaining = (uint16_t)(usb_acl_expected_len - usb_acl_len);
        uint16_t take = len < remaining ? len : remaining;
        memcpy(&usb_acl_buf[usb_acl_len], fragment, take);
        usb_acl_len = (uint16_t)(usb_acl_len + take);
        fragment += take;
        len = (uint16_t)(len - take);

        if (usb_acl_len == usb_acl_expected_len) {
            usb_bth_bridge_push_completed_acl();
        }
    }
}

void usb_bth_bridge_task(void) {
    if (!bridge_enabled || !tud_ready()) return;

    if (backend_tx_in_flight) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if ((uint32_t)(now - backend_tx_started_ms) > 1000u) {
            debug_log("USB bridge backend TX timeout, dropping packet");
            if (backend_tail != backend_head) {
                backend_tail = (uint8_t)((backend_tail + 1u) % USB_QUEUE_DEPTH);
            }
            backend_tx_in_flight = false;
        }
    }

    while (usb_tail != usb_head) {
        hci_queue_item_t *tx = &usb_queue[usb_tail];
        if (!bridge_transport || !bridge_transport->send(tx->type, tx->data, tx->len)) {
            break;
        }
        if (tx->type == HCI_PKT_COMMAND) {
            log_hci_command("USB->BT", tx->data, tx->len);
            usb_bth_bridge_after_controller_send_quirk(tx->data, tx->len);
        }
        usb_tail = (uint8_t)((usb_tail + 1u) % USB_QUEUE_DEPTH);
    }

    if (!backend_tx_in_flight && backend_tail != backend_head) {
        hci_queue_item_t *tx = &backend_queue[backend_tail];
        bool ok = false;
        if (tx->type == HCI_PKT_EVENT) {
            log_hci_event("BT->USB", tx->data, tx->len);
            ok = tud_bt_event_send(tx->data, tx->len);
        } else if (tx->type == HCI_PKT_ACL) {
            log_acl_packet("BT->USB", tx->data, tx->len);
            ok = tud_bt_acl_data_send(tx->data, tx->len);
        }
        backend_tx_in_flight = ok;
        if (ok) {
            backend_tx_started_ms = to_ms_since_boot(get_absolute_time());
        }
    }
}

void tud_bt_hci_cmd_cb(void *hci_cmd, size_t cmd_len) {
    if (!bridge_enabled) return;
    log_hci_command("USB cb", hci_cmd, (uint16_t)cmd_len);
    if (!queue_push(usb_queue, &usb_head, &usb_tail, HCI_PKT_COMMAND, hci_cmd, (uint16_t)cmd_len)) {
        debug_log("USB HCI command queue full");
    }
}

void tud_bt_acl_data_received_cb(void *acl_data, uint16_t data_len) {
    if (!bridge_enabled) return;
    usb_bth_bridge_acl_out_fragment(acl_data, data_len);
}

void tud_bt_event_sent_cb(uint16_t sent_bytes) {
    (void)sent_bytes;
    if (backend_tx_in_flight && backend_tail != backend_head) {
        backend_tail = (uint8_t)((backend_tail + 1u) % USB_QUEUE_DEPTH);
    }
    backend_tx_in_flight = false;
    backend_tx_started_ms = 0;
}

void tud_bt_acl_data_sent_cb(uint16_t sent_bytes) {
    (void)sent_bytes;
    if (backend_tx_in_flight && backend_tail != backend_head) {
        backend_tail = (uint8_t)((backend_tail + 1u) % USB_QUEUE_DEPTH);
    }
    backend_tx_in_flight = false;
    backend_tx_started_ms = 0;
}
