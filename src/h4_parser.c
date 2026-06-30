#include "h4_parser.h"

#include <string.h>

static void h4_start_header(h4_parser_t *parser, hci_packet_type_t type, uint8_t header_len) {
    parser->type = type;
    parser->header_len = header_len;
    parser->header_pos = 0;
    parser->packet_pos = 0;
    parser->packet_len = 0;
}

void h4_parser_init(h4_parser_t *parser, uint8_t *packet_storage, uint16_t packet_capacity, hci_rx_cb_t cb) {
    memset(parser, 0, sizeof(*parser));
    parser->packet = packet_storage;
    parser->packet_capacity = packet_capacity;
    parser->callback = cb;
    parser->state = H4_WAIT_TYPE;
}

void h4_parser_set_callback(h4_parser_t *parser, hci_rx_cb_t cb) {
    parser->callback = cb;
}

void h4_parser_reset(h4_parser_t *parser) {
    parser->state = H4_WAIT_TYPE;
    parser->header_len = 0;
    parser->header_pos = 0;
    parser->packet_pos = 0;
    parser->packet_len = 0;
}

static bool h4_begin_payload(h4_parser_t *parser, h4_parser_state_t payload_state, uint16_t payload_len) {
    parser->packet_len = (uint16_t)(parser->header_len + payload_len);
    if (parser->packet_len > parser->packet_capacity) {
        parser->error_count++;
        h4_parser_reset(parser);
        return false;
    }
    memcpy(parser->packet, parser->header, parser->header_len);
    parser->packet_pos = parser->header_len;
    if (payload_len == 0) {
        if (parser->callback) parser->callback(parser->type, parser->packet, parser->packet_len);
        h4_parser_reset(parser);
    } else {
        parser->state = payload_state;
    }
    return true;
}

bool h4_parser_feed_byte(h4_parser_t *parser, uint8_t byte) {
    switch (parser->state) {
    case H4_WAIT_TYPE:
        if (byte == HCI_PKT_EVENT) {
            h4_start_header(parser, HCI_PKT_EVENT, 2);
            parser->state = H4_READ_EVENT_HEADER;
        } else if (byte == HCI_PKT_ACL) {
            h4_start_header(parser, HCI_PKT_ACL, 4);
            parser->state = H4_READ_ACL_HEADER;
        } else if (byte == HCI_PKT_SCO) {
            h4_start_header(parser, HCI_PKT_SCO, 3);
            parser->state = H4_READ_SCO_HEADER;
        } else {
            parser->error_count++;
            parser->state = H4_ERROR_RECOVERY;
        }
        return true;

    case H4_READ_EVENT_HEADER:
        parser->header[parser->header_pos++] = byte;
        if (parser->header_pos == parser->header_len) {
            return h4_begin_payload(parser, H4_READ_EVENT_PAYLOAD, parser->header[1]);
        }
        return true;

    case H4_READ_ACL_HEADER:
        parser->header[parser->header_pos++] = byte;
        if (parser->header_pos == parser->header_len) {
            const uint16_t payload_len = (uint16_t)parser->header[2] | ((uint16_t)parser->header[3] << 8u);
            return h4_begin_payload(parser, H4_READ_ACL_PAYLOAD, payload_len);
        }
        return true;

    case H4_READ_SCO_HEADER:
        parser->header[parser->header_pos++] = byte;
        if (parser->header_pos == parser->header_len) {
            return h4_begin_payload(parser, H4_READ_SCO_PAYLOAD, parser->header[2]);
        }
        return true;

    case H4_READ_EVENT_PAYLOAD:
    case H4_READ_ACL_PAYLOAD:
    case H4_READ_SCO_PAYLOAD:
        if (parser->packet_pos >= parser->packet_capacity) {
            parser->error_count++;
            h4_parser_reset(parser);
            return false;
        }
        parser->packet[parser->packet_pos++] = byte;
        if (parser->packet_pos == parser->packet_len) {
            if (parser->callback) parser->callback(parser->type, parser->packet, parser->packet_len);
            h4_parser_reset(parser);
        }
        return true;

    case H4_ERROR_RECOVERY:
    default:
        h4_parser_reset(parser);
        return false;
    }
}
