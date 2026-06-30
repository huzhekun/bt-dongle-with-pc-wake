#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hci_transport.h"

typedef enum {
    H4_WAIT_TYPE = 0,
    H4_READ_EVENT_HEADER,
    H4_READ_EVENT_PAYLOAD,
    H4_READ_ACL_HEADER,
    H4_READ_ACL_PAYLOAD,
    H4_READ_SCO_HEADER,
    H4_READ_SCO_PAYLOAD,
    H4_ERROR_RECOVERY,
} h4_parser_state_t;

typedef struct {
    h4_parser_state_t state;
    hci_packet_type_t type;
    uint8_t header[4];
    uint8_t header_len;
    uint8_t header_pos;
    uint8_t *packet;
    uint16_t packet_capacity;
    uint16_t packet_pos;
    uint16_t packet_len;
    uint32_t error_count;
    hci_rx_cb_t callback;
} h4_parser_t;

void h4_parser_init(h4_parser_t *parser, uint8_t *packet_storage, uint16_t packet_capacity, hci_rx_cb_t cb);
void h4_parser_set_callback(h4_parser_t *parser, hci_rx_cb_t cb);
void h4_parser_reset(h4_parser_t *parser);
bool h4_parser_feed_byte(h4_parser_t *parser, uint8_t byte);
