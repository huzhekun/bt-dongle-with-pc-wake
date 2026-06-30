#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hci_packet.h"

typedef void (*hci_rx_cb_t)(hci_packet_type_t type, const uint8_t *packet, uint16_t len);

typedef struct {
    bool (*init)(void);
    bool (*reset_controller)(void);
    bool (*send)(hci_packet_type_t type, const uint8_t *packet, uint16_t len);
    void (*set_rx_callback)(hci_rx_cb_t cb);
    void (*task)(void);
} hci_transport_t;

const hci_transport_t *backend_esp32_uart_get_transport(void);
const hci_transport_t *backend_stub_get_transport(void);
const hci_transport_t *backend_cyw43_get_transport(void);
