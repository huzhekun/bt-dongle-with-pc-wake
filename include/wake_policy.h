#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hci_packet.h"

void wake_policy_init(void);
void wake_policy_task(void);
void wake_policy_observe_host_packet(hci_packet_type_t type, const uint8_t *packet, uint16_t len);
void wake_policy_clear_known_peers(void);
bool wake_policy_packet_matches(hci_packet_type_t type, const uint8_t *packet, uint16_t len, const char **reason);
