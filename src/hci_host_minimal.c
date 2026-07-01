#include "hci_host_minimal.h"

#include "debug_log.h"
#include "pico/time.h"

#include <string.h>

static const hci_transport_t *host_transport;
static volatile bool waiting;
static volatile uint16_t waiting_opcode;
static volatile bool got_complete;
static volatile uint8_t got_status;

bool hci_host_minimal_init(const hci_transport_t *transport) {
    host_transport = transport;
    waiting = false;
    got_complete = false;
    return host_transport != NULL;
}

static void send_cmd(uint16_t opcode, const uint8_t *params, uint8_t param_len) {
    uint8_t cmd[HCI_CMD_MAX_LEN];
    cmd[0] = (uint8_t)(opcode & 0xffu);
    cmd[1] = (uint8_t)(opcode >> 8u);
    cmd[2] = param_len;
    if (param_len && params) memcpy(&cmd[3], params, param_len);
    host_transport->send(HCI_PKT_COMMAND, cmd, (uint16_t)(3u + param_len));
}

bool hci_host_send_cmd_sync(uint16_t opcode, const uint8_t *params, uint8_t param_len,
                            hci_cmd_result_t *result, uint32_t timeout_ms) {
    if (!host_transport) return false;

    waiting_opcode = opcode;
    got_status = 0xff;
    got_complete = false;
    waiting = true;
    send_cmd(opcode, params, param_len);

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!time_reached(deadline)) {
        host_transport->task();
        if (got_complete) {
            waiting = false;
            if (result) {
                result->opcode = opcode;
                result->status = got_status;
                result->complete = true;
            }
            debug_log("standby HCI cmd 0x%04x status=0x%02x", opcode, got_status);
            return got_status == 0;
        }
        tight_loop_contents();
    }

    waiting = false;
    if (result) {
        result->opcode = opcode;
        result->status = 0xff;
        result->complete = false;
    }
    debug_log("standby HCI cmd 0x%04x timeout", opcode);
    return false;
}

bool hci_host_configure_standby_wake(void) {
    hci_cmd_result_t result;
    static const uint8_t event_mask[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f};
    static const uint8_t le_event_mask[8] = {0x1f, 0x14, 0, 0, 0, 0, 0, 0};
    static const uint8_t le_scan_params[] = {
        0x01,       // active scan
        0x60, 0x00, // interval
        0x30, 0x00, // window
        0x00,       // public own address
        0x00        // accept all advertisements
    };
    static const uint8_t le_scan_enable[] = {0x01, 0x00};
    static const uint8_t classic_scan_enable[] = {0x02};

    debug_log("standby HCI host init");
    if (!host_transport->reset_controller()) return false;
    (void)hci_host_send_cmd_sync(HCI_OPCODE_READ_LOCAL_VERSION, NULL, 0, &result, 1000);
    (void)hci_host_send_cmd_sync(HCI_OPCODE_READ_BD_ADDR, NULL, 0, &result, 1000);
    if (!hci_host_send_cmd_sync(HCI_OPCODE_SET_EVENT_MASK, event_mask, sizeof(event_mask), &result, 1000)) return false;
    (void)hci_host_send_cmd_sync(HCI_OPCODE_LE_SET_EVENT_MASK, le_event_mask, sizeof(le_event_mask), &result, 1000);
    (void)hci_host_send_cmd_sync(HCI_OPCODE_WRITE_SCAN_ENABLE, classic_scan_enable, sizeof(classic_scan_enable), &result, 1000);
    if (!hci_host_send_cmd_sync(HCI_OPCODE_LE_SET_SCAN_PARAMETERS, le_scan_params, sizeof(le_scan_params), &result, 1000)) return false;
    if (!hci_host_send_cmd_sync(HCI_OPCODE_LE_SET_SCAN_ENABLE, le_scan_enable, sizeof(le_scan_enable), &result, 1000)) return false;
    debug_log("standby BLE scan enabled");
    return true;
}

bool hci_host_stop_standby_wake(void) {
    hci_cmd_result_t result;
    static const uint8_t le_scan_disable[] = {0x00, 0x00};
    static const uint8_t classic_scan_disable[] = {0x00};
    (void)hci_host_send_cmd_sync(HCI_OPCODE_LE_SET_SCAN_ENABLE, le_scan_disable, sizeof(le_scan_disable), &result, 500);
    (void)hci_host_send_cmd_sync(HCI_OPCODE_WRITE_SCAN_ENABLE, classic_scan_disable, sizeof(classic_scan_disable), &result, 500);
    return true;
}

void hci_host_minimal_packet(hci_packet_type_t type, const uint8_t *packet, uint16_t len) {
    if (!waiting || type != HCI_PKT_EVENT || len < 3) return;

    if (packet[0] == HCI_EVENT_COMMAND_COMPLETE && len >= 6) {
        uint16_t opcode = (uint16_t)packet[3] | ((uint16_t)packet[4] << 8u);
        if (opcode == waiting_opcode) {
            got_status = packet[5];
            got_complete = true;
        }
    } else if (packet[0] == HCI_EVENT_COMMAND_STATUS && len >= 6) {
        uint16_t opcode = (uint16_t)packet[4] | ((uint16_t)packet[5] << 8u);
        if (opcode == waiting_opcode) {
            got_status = packet[2];
            got_complete = true;
        }
    }
}

void hci_host_minimal_task(void) {
    if (host_transport) host_transport->task();
}
