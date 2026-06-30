#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hci_transport.h"

typedef struct {
    uint16_t opcode;
    uint8_t status;
    bool complete;
} hci_cmd_result_t;

bool hci_host_minimal_init(const hci_transport_t *transport);
bool hci_host_send_cmd_sync(uint16_t opcode, const uint8_t *params, uint8_t param_len,
                            hci_cmd_result_t *result, uint32_t timeout_ms);
bool hci_host_configure_standby_wake(void);
bool hci_host_stop_standby_wake(void);
void hci_host_minimal_packet(hci_packet_type_t type, const uint8_t *packet, uint16_t len);
void hci_host_minimal_task(void);
