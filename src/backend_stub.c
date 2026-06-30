#include "hci_transport.h"

#include <string.h>

static hci_rx_cb_t rx_cb;

static void stub_set_rx_callback(hci_rx_cb_t cb) {
    rx_cb = cb;
}

static bool stub_init(void) {
    return true;
}

static bool stub_reset_controller(void) {
    return true;
}

static void stub_command_complete(uint16_t opcode, const uint8_t *return_params, uint8_t return_len) {
    uint8_t event[HCI_EVENT_MAX_LEN];
    event[0] = HCI_EVENT_COMMAND_COMPLETE;
    event[1] = (uint8_t)(4u + return_len);
    event[2] = 1;
    event[3] = (uint8_t)(opcode & 0xffu);
    event[4] = (uint8_t)(opcode >> 8u);
    event[5] = 0;
    if (return_len && return_params) memcpy(&event[6], return_params, return_len);
    if (rx_cb) rx_cb(HCI_PKT_EVENT, event, (uint16_t)(6u + return_len));
}

static bool stub_send(hci_packet_type_t type, const uint8_t *packet, uint16_t len) {
    if (type == HCI_PKT_COMMAND && len >= 3) {
        uint16_t opcode = (uint16_t)packet[0] | ((uint16_t)packet[1] << 8u);
        if (opcode == HCI_OPCODE_READ_BD_ADDR) {
            static const uint8_t bd_addr[6] = {0x56, 0x34, 0x12, 0xef, 0xcd, 0xab};
            stub_command_complete(opcode, bd_addr, sizeof(bd_addr));
        } else {
            stub_command_complete(opcode, NULL, 0);
        }
    }
    return true;
}

static void stub_task(void) {
}

static const hci_transport_t transport = {
    .init = stub_init,
    .reset_controller = stub_reset_controller,
    .send = stub_send,
    .set_rx_callback = stub_set_rx_callback,
    .task = stub_task,
};

const hci_transport_t *backend_stub_get_transport(void) {
    return &transport;
}
