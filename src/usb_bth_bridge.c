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
static uint8_t suppress_write_simple_pairing_mode_complete;
static uint8_t suppress_le_set_random_address_complete;
static uint8_t suppress_le_set_scan_parameters_complete;
static uint8_t suppress_le_set_scan_enable_complete;
static uint32_t le_adv_reports_seen;

static uint16_t le16_at(const uint8_t *packet, uint16_t offset) {
    return (uint16_t)packet[offset] | ((uint16_t)packet[offset + 1u] << 8u);
}

static const char *addr_type_name(uint8_t type) {
    switch (type) {
    case 0x00: return "public";
    case 0x01: return "random";
    case 0x02: return "public-id";
    case 0x03: return "random-id";
    default: return "unknown";
    }
}

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
        if (opcode == HCI_OPCODE_LE_SET_SCAN_PARAMETERS && len >= 10u) {
            debug_log("%s cmd LE Set Scan Params type=0x%02x int=0x%04x win=0x%04x own=0x%02x policy=0x%02x q=%u/%u",
                      prefix, packet[3], le16_at(packet, 4), le16_at(packet, 6),
                      packet[8], packet[9], usb_tail, usb_head);
        } else if (opcode == HCI_OPCODE_LE_SET_SCAN_ENABLE && len >= 5u) {
            debug_log("%s cmd LE Set Scan Enable enable=%u filter_dup=%u q=%u/%u",
                      prefix, packet[3], packet[4], usb_tail, usb_head);
        } else if (opcode == HCI_OPCODE_LE_CREATE_CONNECTION && len >= 28u) {
            debug_log("%s cmd LE Create Conn scan_int=0x%04x scan_win=0x%04x policy=0x%02x peer_type=%s peer=%02x:%02x:%02x:%02x:%02x:%02x",
                      prefix, le16_at(packet, 3), le16_at(packet, 5), packet[7],
                      addr_type_name(packet[8]), packet[14], packet[13], packet[12],
                      packet[11], packet[10], packet[9]);
        } else if (opcode == HCI_OPCODE_LE_EXTENDED_CREATE_CONNECTION && len >= 13u) {
            debug_log("%s cmd LE Ext Create Conn filter_policy=0x%02x own=0x%02x peer_type=%s peer=%02x:%02x:%02x:%02x:%02x:%02x phys=0x%02x",
                      prefix, packet[3], packet[4], addr_type_name(packet[5]),
                      packet[11], packet[10], packet[9], packet[8], packet[7], packet[6],
                      packet[12]);
        } else {
            debug_log("%s cmd opcode=0x%04x plen=%u q=%u/%u", prefix, opcode, packet[2], usb_tail, usb_head);
        }
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
    } else if (packet[0] == HCI_EVENT_LE_META && len >= 3u &&
               packet[2] == HCI_SUBEVENT_LE_ADVERTISING_REPORT) {
        le_adv_reports_seen++;
        if ((le_adv_reports_seen & 0x0fu) == 1u) {
            debug_log("%s evt LE adv_report count=%lu reports=%u len=%u",
                      prefix, le_adv_reports_seen, len >= 4u ? packet[3] : 0u, len);
        }
    } else if (packet[0] == HCI_EVENT_LE_META && len >= 3u &&
               packet[2] == HCI_SUBEVENT_LE_EXTENDED_ADVERTISING_REPORT) {
        le_adv_reports_seen++;
        if ((le_adv_reports_seen & 0x0fu) == 1u) {
            debug_log("%s evt LE ext_adv_report count=%lu reports=%u len=%u",
                      prefix, le_adv_reports_seen, len >= 4u ? packet[3] : 0u, len);
        }
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

static bool queue_read_local_extended_features_complete(uint8_t page_number) {
    uint8_t event[] = {
        HCI_EVENT_COMMAND_COMPLETE,
        0x0e,
        0x01,
        (uint8_t)(HCI_OPCODE_READ_LOCAL_EXTENDED_FEATURES & 0xffu),
        (uint8_t)(HCI_OPCODE_READ_LOCAL_EXTENDED_FEATURES >> 8u),
        0x00,
        page_number,
        0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    return queue_push(backend_queue, &backend_head, &backend_tail, HCI_PKT_EVENT, event, sizeof(event));
}

static bool hci_command_opcode_is(const uint8_t *packet, uint16_t len, uint16_t opcode) {
    if (len < 3u) return false;
    uint16_t packet_opcode = (uint16_t)packet[0] | ((uint16_t)packet[1] << 8u);
    return packet_opcode == opcode;
}

static bool hci_event_command_complete_opcode_is(const uint8_t *packet, uint16_t len, uint16_t opcode) {
    if (len < 6u || packet[0] != HCI_EVENT_COMMAND_COMPLETE) return false;
    uint16_t event_opcode = (uint16_t)packet[3] | ((uint16_t)packet[4] << 8u);
    return event_opcode == opcode;
}

static bool usb_bth_bridge_should_drop_delayed_quirk_complete(const uint8_t *packet, uint16_t len) {
#if HCI_BACKEND_cyw43
    if (suppress_write_simple_pairing_mode_complete &&
        hci_event_command_complete_opcode_is(packet, len, HCI_OPCODE_WRITE_SIMPLE_PAIRING_MODE)) {
        suppress_write_simple_pairing_mode_complete--;
        debug_log("CYW43 quirk: drop delayed Write Simple Pairing Mode complete");
        return true;
    }
    if (suppress_le_set_random_address_complete &&
        hci_event_command_complete_opcode_is(packet, len, HCI_OPCODE_LE_SET_RANDOM_ADDRESS)) {
        suppress_le_set_random_address_complete--;
        debug_log("CYW43 quirk: drop delayed LE Set Random Address complete");
        return true;
    }
    if (suppress_le_set_scan_parameters_complete &&
        hci_event_command_complete_opcode_is(packet, len, HCI_OPCODE_LE_SET_SCAN_PARAMETERS)) {
        suppress_le_set_scan_parameters_complete--;
        debug_log("CYW43 quirk: drop delayed LE Set Scan Parameters complete");
        return true;
    }
    if (suppress_le_set_scan_enable_complete &&
        hci_event_command_complete_opcode_is(packet, len, HCI_OPCODE_LE_SET_SCAN_ENABLE)) {
        suppress_le_set_scan_enable_complete--;
        debug_log("CYW43 quirk: drop delayed LE Set Scan Enable complete");
        return true;
    }
#else
    (void)packet;
    (void)len;
#endif
    return false;
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
        if (suppress_write_simple_pairing_mode_complete != UINT8_MAX) suppress_write_simple_pairing_mode_complete++;
    } else if (hci_command_opcode_is(packet, len, HCI_OPCODE_LE_SET_RANDOM_ADDRESS)) {
        debug_log("CYW43 quirk: sent LE Set Random Address, synthesize complete");
        if (!queue_command_complete(HCI_OPCODE_LE_SET_RANDOM_ADDRESS, 0x00)) {
            debug_log("USB bridge backend queue full");
        }
        if (suppress_le_set_random_address_complete != UINT8_MAX) suppress_le_set_random_address_complete++;
    } else if (hci_command_opcode_is(packet, len, HCI_OPCODE_LE_SET_SCAN_PARAMETERS)) {
        debug_log("CYW43 quirk: sent LE Set Scan Parameters, synthesize complete");
        if (!queue_command_complete(HCI_OPCODE_LE_SET_SCAN_PARAMETERS, 0x00)) {
            debug_log("USB bridge backend queue full");
        }
        if (suppress_le_set_scan_parameters_complete != UINT8_MAX) suppress_le_set_scan_parameters_complete++;
    } else if (hci_command_opcode_is(packet, len, HCI_OPCODE_LE_SET_SCAN_ENABLE)) {
        debug_log("CYW43 quirk: sent LE Set Scan Enable, synthesize complete");
        if (!queue_command_complete(HCI_OPCODE_LE_SET_SCAN_ENABLE, 0x00)) {
            debug_log("USB bridge backend queue full");
        }
        if (suppress_le_set_scan_enable_complete != UINT8_MAX) suppress_le_set_scan_enable_complete++;
    }
#else
    (void)packet;
    (void)len;
#endif
}

static bool usb_bth_bridge_pre_controller_send_quirk(const uint8_t *packet, uint16_t len) {
#if HCI_BACKEND_cyw43
    if (hci_command_opcode_is(packet, len, HCI_OPCODE_READ_LOCAL_EXTENDED_FEATURES)) {
        uint8_t page_number = len >= 4u ? packet[3] : 0x00u;
        debug_log("CYW43 quirk: synthesize Read Local Extended Features page=%u", page_number);
        if (!queue_read_local_extended_features_complete(page_number)) {
            debug_log("USB bridge backend queue full");
        }
        return true;
    }
#else
    (void)packet;
    (void)len;
#endif
    return false;
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

    if (type == HCI_PKT_EVENT && usb_bth_bridge_should_drop_delayed_quirk_complete(packet, len)) {
        return;
    }

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
        if (tx->type == HCI_PKT_COMMAND && usb_bth_bridge_pre_controller_send_quirk(tx->data, tx->len)) {
            usb_tail = (uint8_t)((usb_tail + 1u) % USB_QUEUE_DEPTH);
            continue;
        }
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
    if (usb_bth_bridge_pre_controller_send_quirk(hci_cmd, (uint16_t)cmd_len)) return;
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
