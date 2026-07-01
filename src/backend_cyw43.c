#include "hci_transport.h"

#include "cyw43.h"
#include "debug_log.h"
#include "pico/btstack_cyw43.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include <string.h>

#define CYW43_HCI_HEADER_SIZE 4u
#define CYW43_HCI_RX_BUF_SIZE (CYW43_HCI_HEADER_SIZE + HCI_ACL_MAX_LEN + 8u)
#define HCI_OPCODE_BCM_SET_BD_ADDR HCI_OPCODE(0x3fu, 0x0001u)
#define CYW43_LOCAL_INIT_TIMEOUT_MS 500u
#define CYW43_MAX_RX_PACKETS_PER_TASK 8u

static hci_rx_cb_t rx_cb;
static bool arch_ready;
static uint8_t rx_buf[CYW43_HCI_RX_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t tx_buf[CYW43_HCI_RX_BUF_SIZE] __attribute__((aligned(4)));

static bool cyw43_backend_send_command_sync(const uint8_t *command, uint16_t len, uint16_t opcode);

static void cyw43_backend_set_rx_callback(hci_rx_cb_t cb) {
    rx_cb = cb;
}

static bool cyw43_backend_send(hci_packet_type_t type, const uint8_t *packet, uint16_t len) {
    if (len + CYW43_HCI_HEADER_SIZE > sizeof(tx_buf)) return false;

    memset(tx_buf, 0, CYW43_HCI_HEADER_SIZE);
    tx_buf[3] = (uint8_t)type;
    if (len) memcpy(&tx_buf[CYW43_HCI_HEADER_SIZE], packet, len);

    cyw43_arch_lwip_begin();
    int err = cyw43_bluetooth_hci_write(tx_buf, (size_t)(len + CYW43_HCI_HEADER_SIZE));
    cyw43_arch_lwip_end();

    if (err) {
        debug_log("CYW43 HCI write failed: %d", err);
        return false;
    }
    return true;
}

static bool cyw43_backend_reset_controller(void) {
    static const uint8_t hci_reset[] = {
        (uint8_t)(HCI_OPCODE_RESET & 0xffu),
        (uint8_t)(HCI_OPCODE_RESET >> 8u),
        0x00
    };
    return cyw43_backend_send_command_sync(hci_reset, sizeof(hci_reset), HCI_OPCODE_RESET);
}

static bool cyw43_backend_wait_command_complete(uint16_t opcode, uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

    while (!time_reached(deadline)) {
        uint32_t len = 0;
        int err = cyw43_bluetooth_hci_read(rx_buf, sizeof(rx_buf), &len);
        if (err || len == 0u) {
            sleep_ms(1);
            continue;
        }
        if (len < CYW43_HCI_HEADER_SIZE + 2u) continue;

        hci_packet_type_t type = (hci_packet_type_t)rx_buf[3];
        const uint8_t *packet = &rx_buf[CYW43_HCI_HEADER_SIZE];
        uint16_t packet_len = (uint16_t)(len - CYW43_HCI_HEADER_SIZE);
        if (type != HCI_PKT_EVENT || packet_len < 2u) continue;

        if (packet[0] == HCI_EVENT_COMMAND_COMPLETE && packet_len >= 6u) {
            uint16_t event_opcode = (uint16_t)packet[3] | ((uint16_t)packet[4] << 8u);
            if (event_opcode == opcode) {
                if (packet[5] != 0x00u) {
                    debug_log("CYW43 local init opcode=0x%04x status=0x%02x", opcode, packet[5]);
                    return false;
                }
                return true;
            }
        }
    }

    debug_log("CYW43 local init opcode=0x%04x timed out", opcode);
    return false;
}

static bool cyw43_backend_send_command_sync(const uint8_t *command, uint16_t len, uint16_t opcode) {
    if (!cyw43_backend_send(HCI_PKT_COMMAND, command, len)) return false;
    return cyw43_backend_wait_command_complete(opcode, CYW43_LOCAL_INIT_TIMEOUT_MS);
}

static void cyw43_backend_local_init(void) {
    // Windows treats this as a generic USB controller, so run the CYW43
    // chipset hook BTstack would normally use to assign a stable BD_ADDR.
    static const uint8_t hci_reset[] = {
        (uint8_t)(HCI_OPCODE_RESET & 0xffu),
        (uint8_t)(HCI_OPCODE_RESET >> 8u),
        0x00
    };
    uint8_t set_bd_addr[] = {
        (uint8_t)(HCI_OPCODE_BCM_SET_BD_ADDR & 0xffu),
        (uint8_t)(HCI_OPCODE_BCM_SET_BD_ADDR >> 8u),
        0x06,
        0, 0, 0, 0, 0, 0
    };
    uint8_t addr[6];

    cyw43_hal_get_mac(CYW43_HAL_MAC_WLAN0, addr);
    addr[5]++;
    set_bd_addr[3] = addr[5];
    set_bd_addr[4] = addr[4];
    set_bd_addr[5] = addr[3];
    set_bd_addr[6] = addr[2];
    set_bd_addr[7] = addr[1];
    set_bd_addr[8] = addr[0];

    if (!cyw43_backend_send_command_sync(hci_reset, sizeof(hci_reset), HCI_OPCODE_RESET)) {
        debug_log("CYW43 local reset failed");
        return;
    }
    if (!cyw43_backend_send_command_sync(set_bd_addr, sizeof(set_bd_addr), HCI_OPCODE_BCM_SET_BD_ADDR)) {
        debug_log("CYW43 local BD_ADDR init failed");
        return;
    }

    debug_log("CYW43 local BD_ADDR %02x:%02x:%02x:%02x:%02x:%02x",
              addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

static bool cyw43_backend_init(void) {
    if (!arch_ready) {
        int err = cyw43_arch_init();
        if (err) {
            debug_log("CYW43 arch init failed: %d", err);
            return false;
        }
        arch_ready = true;
    }

    int err = cyw43_bluetooth_hci_init();
    if (err) {
        debug_log("CYW43 Bluetooth HCI init failed: %d", err);
        return false;
    }

    cyw43_backend_local_init();
    debug_log("CYW43 Bluetooth HCI backend active");
    return true;
}

static void cyw43_backend_task(void) {
    if (!arch_ready) return;

    uint32_t len = 0;
    bool had_packet;
    uint8_t packets_read = 0;
    do {
        had_packet = false;

        int err = cyw43_bluetooth_hci_read(rx_buf, sizeof(rx_buf), &len);

        if (err || len == 0) continue;
        if (len < CYW43_HCI_HEADER_SIZE) continue;

        hci_packet_type_t type = (hci_packet_type_t)rx_buf[3];
        uint16_t packet_len = (uint16_t)(len - CYW43_HCI_HEADER_SIZE);
        if (rx_cb) rx_cb(type, &rx_buf[CYW43_HCI_HEADER_SIZE], packet_len);
        had_packet = true;
        packets_read++;
    } while (had_packet && packets_read < CYW43_MAX_RX_PACKETS_PER_TASK);
}

static const hci_transport_t transport = {
    .init = cyw43_backend_init,
    .reset_controller = cyw43_backend_reset_controller,
    .send = cyw43_backend_send,
    .set_rx_callback = cyw43_backend_set_rx_callback,
    .task = cyw43_backend_task,
};

const hci_transport_t *backend_cyw43_get_transport(void) {
    return &transport;
}

void cyw43_bluetooth_hci_process(void) {
}

// The cyw43 arch layer calls these hooks when Bluetooth support is enabled.
// We bypass BTstack and expose the controller directly as USB HCI.
bool btstack_cyw43_init(async_context_t *context) {
    (void)context;
    return true;
}

void btstack_cyw43_deinit(async_context_t *context) {
    (void)context;
}
