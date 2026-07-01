#include "wake_policy.h"

#include "debug_log.h"

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"
#include "pico/time.h"

#include <stddef.h>
#include <string.h>

#define MAX_KNOWN_BLE_PEERS 16u
#define WAKE_PEER_MAGIC 0x57424c45u
#define WAKE_PEER_VERSION 1u
#define WAKE_PEER_SAVE_DELAY_MS 2000u

#ifndef WAKE_PEER_FLASH_OFFSET
#define WAKE_PEER_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - (4u * FLASH_SECTOR_SIZE))
#endif

_Static_assert(WAKE_PEER_FLASH_OFFSET % FLASH_SECTOR_SIZE == 0, "wake peer storage must be sector-aligned");

typedef struct {
    bool in_use;
    uint8_t addr_type;
    uint8_t addr[6];
} ble_peer_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t checksum;
    uint8_t count;
    uint8_t reserved[3];
    ble_peer_t peers[MAX_KNOWN_BLE_PEERS];
} wake_peer_store_t;

_Static_assert(sizeof(wake_peer_store_t) <= FLASH_PAGE_SIZE, "wake peer store must fit one flash page");

static ble_peer_t known_ble_peers[MAX_KNOWN_BLE_PEERS];
static bool peers_dirty;
static absolute_time_t peers_dirty_at;
static uint32_t standby_adv_reports;

static uint32_t checksum_bytes(const uint8_t *data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

static uint8_t known_ble_peer_count(void) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < MAX_KNOWN_BLE_PEERS; i++) {
        if (known_ble_peers[i].in_use) count++;
    }
    return count;
}

static uint32_t store_checksum(const wake_peer_store_t *store) {
    wake_peer_store_t tmp = *store;
    tmp.checksum = 0;
    return checksum_bytes((const uint8_t *)&tmp, sizeof(tmp));
}

static const wake_peer_store_t *flash_store(void) {
    return (const wake_peer_store_t *)(XIP_BASE + WAKE_PEER_FLASH_OFFSET);
}

static void load_known_ble_peers(void) {
#if WAKE_PERSIST_BLE_PEERS
    const wake_peer_store_t *store = flash_store();
    if (store->magic != WAKE_PEER_MAGIC ||
        store->version != WAKE_PEER_VERSION ||
        store->size != sizeof(wake_peer_store_t) ||
        store->count > MAX_KNOWN_BLE_PEERS ||
        store->checksum != store_checksum(store)) {
        debug_log("BLE wake peer list empty");
        return;
    }

    uint8_t loaded = 0;
    for (uint8_t i = 0; i < store->count && loaded < MAX_KNOWN_BLE_PEERS; i++) {
        if (!store->peers[i].in_use) continue;
        known_ble_peers[loaded++] = store->peers[i];
    }
    debug_log("Loaded %u BLE wake peer(s)", loaded);
#endif
}

static void save_known_ble_peers_flash_op(void *param) {
    const uint8_t *page = (const uint8_t *)param;
    uint32_t interrupts = save_and_disable_interrupts();
    flash_range_erase(WAKE_PEER_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(WAKE_PEER_FLASH_OFFSET, page, FLASH_PAGE_SIZE);
    restore_interrupts(interrupts);
}

static bool save_known_ble_peers(void) {
#if WAKE_PERSIST_BLE_PEERS
    wake_peer_store_t store;
    memset(&store, 0, sizeof(store));
    store.magic = WAKE_PEER_MAGIC;
    store.version = WAKE_PEER_VERSION;
    store.size = sizeof(store);
    store.count = known_ble_peer_count();
    memcpy(store.peers, known_ble_peers, sizeof(known_ble_peers));
    store.checksum = store_checksum(&store);

    uint8_t page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    memset(page, 0xff, sizeof(page));
    memcpy(page, &store, sizeof(store));

    int rc = flash_safe_execute(save_known_ble_peers_flash_op, page, 1000);
    if (rc != PICO_OK) {
        debug_log("BLE wake peer save failed: %d", rc);
        return false;
    }

    const wake_peer_store_t *verify = flash_store();
    if (verify->checksum != store.checksum) {
        debug_log("BLE wake peer save verify failed");
        return false;
    }

    debug_log("Saved %u BLE wake peer(s)", store.count);
    return true;
#else
    return true;
#endif
}

static void mark_peers_dirty(void) {
#if WAKE_PERSIST_BLE_PEERS
    peers_dirty = true;
    peers_dirty_at = make_timeout_time_ms(WAKE_PEER_SAVE_DELAY_MS);
#endif
}

void wake_policy_init(void) {
    memset(known_ble_peers, 0, sizeof(known_ble_peers));
    peers_dirty = false;
    load_known_ble_peers();
}

void wake_policy_task(void) {
#if WAKE_PERSIST_BLE_PEERS
    if (peers_dirty && time_reached(peers_dirty_at)) {
        peers_dirty = false;
        (void)save_known_ble_peers();
    }
#endif
}

static bool ble_addr_equal(const ble_peer_t *peer, uint8_t addr_type, const uint8_t *addr) {
    return peer->in_use && peer->addr_type == addr_type && memcmp(peer->addr, addr, 6) == 0;
}

static void remove_known_ble_peer_at(uint8_t index) {
    for (uint8_t i = index; i + 1u < MAX_KNOWN_BLE_PEERS; i++) {
        known_ble_peers[i] = known_ble_peers[i + 1u];
    }
    memset(&known_ble_peers[MAX_KNOWN_BLE_PEERS - 1u], 0, sizeof(known_ble_peers[0]));
}

static void append_known_ble_peer(uint8_t addr_type, const uint8_t *addr) {
    uint8_t count = known_ble_peer_count();
    if (count >= MAX_KNOWN_BLE_PEERS) {
        remove_known_ble_peer_at(0);
        count = MAX_KNOWN_BLE_PEERS - 1u;
    }

    ble_peer_t *peer = &known_ble_peers[count];
    peer->in_use = true;
    peer->addr_type = addr_type;
    memcpy(peer->addr, addr, 6);
}

static bool remember_ble_peer(uint8_t addr_type, const uint8_t *addr) {
#if WAKE_ON_KNOWN_BLE_PEER
    for (uint8_t i = 0; i < MAX_KNOWN_BLE_PEERS; i++) {
        if (!ble_addr_equal(&known_ble_peers[i], addr_type, addr)) continue;

        uint8_t count = known_ble_peer_count();
        if (count > 0u && i + 1u == count) return false;

        ble_peer_t peer = known_ble_peers[i];
        remove_known_ble_peer_at(i);
        count = known_ble_peer_count();
        known_ble_peers[count] = peer;
        mark_peers_dirty();
        debug_log("Refreshed BLE wake peer type=%u addr=%02x:%02x:%02x:%02x:%02x:%02x",
                  addr_type, addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
        return true;
    }

    append_known_ble_peer(addr_type, addr);
    mark_peers_dirty();
    debug_log("Learned BLE wake peer type=%u addr=%02x:%02x:%02x:%02x:%02x:%02x",
              addr_type, addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    return true;
#else
    (void)addr_type;
    (void)addr;
    return false;
#endif
}

static bool known_ble_peer_matches(uint8_t addr_type, const uint8_t *addr) {
#if WAKE_ON_KNOWN_BLE_PEER
    for (uint8_t i = 0; i < MAX_KNOWN_BLE_PEERS; i++) {
        if (ble_addr_equal(&known_ble_peers[i], addr_type, addr)) return true;
    }
#else
    (void)addr_type;
    (void)addr;
#endif

    return false;
}

void wake_policy_observe_host_packet(hci_packet_type_t type, const uint8_t *packet, uint16_t len) {
    if (type != HCI_PKT_EVENT || len < 2) return;

    if (packet[0] == HCI_EVENT_LE_META && len >= 14 &&
        (packet[2] == HCI_SUBEVENT_LE_CONNECTION_COMPLETE ||
         packet[2] == HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE) &&
        packet[3] == 0x00u) {
        (void)remember_ble_peer(packet[7], &packet[8]);
    }
}

void wake_policy_clear_known_peers(void) {
    memset(known_ble_peers, 0, sizeof(known_ble_peers));
    peers_dirty = false;
    (void)save_known_ble_peers();
    debug_log("Cleared BLE wake peer list");
}

static bool ascii_contains_token(const uint8_t *data, size_t len, const char *token) {
    size_t token_len = strlen(token);
    if (token_len == 0 || token_len > len) return false;

    for (size_t i = 0; i <= (size_t)len - token_len; i++) {
        bool match = true;
        for (size_t j = 0; j < token_len; j++) {
            char a = (char)data[i + j];
            char b = token[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }

    return false;
}

static bool ad_uuid16_list_has(const uint8_t *data, size_t len, uint16_t uuid) {
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint16_t found = (uint16_t)data[i] | ((uint16_t)data[i + 1] << 8u);
        if (found == uuid) return true;
    }
    return false;
}

static bool ad_name_looks_like_controller(const uint8_t *data, size_t len) {
    return ascii_contains_token(data, len, "controller") ||
           ascii_contains_token(data, len, "gamepad") ||
           ascii_contains_token(data, len, "dualsense") ||
           ascii_contains_token(data, len, "stadia") ||
           ascii_contains_token(data, len, "xbox") ||
           ascii_contains_token(data, len, "wireless controller");
}

static bool ad_name_is_stadia(const uint8_t *data, size_t len) {
    static const char stadia_prefix[] = "Stadia";
    if (len < sizeof(stadia_prefix) - 1u) return false;
    return memcmp(data, stadia_prefix, sizeof(stadia_prefix) - 1u) == 0;
}

static bool le_advertising_data_matches(const uint8_t *data, size_t len, const char **reason,
                                        bool *learn_peer) {
    size_t pos = 0;
    while (pos < len) {
        uint8_t field_len = data[pos++];
        if (field_len == 0) break;
        if ((size_t)field_len > (size_t)len - pos) break;

        uint8_t ad_type = data[pos];
        const uint8_t *ad_data = &data[pos + 1u];
        size_t ad_data_len = (size_t)field_len - 1u;

#if WAKE_ON_STADIA_ADV
        if ((ad_type == 0x08u || ad_type == 0x09u) && ad_name_is_stadia(ad_data, ad_data_len)) {
            if (reason) *reason = "Stadia BLE advertisement";
            if (learn_peer) *learn_peer = true;
            return true;
        }
#endif

#if WAKE_ON_BLE_HID
        if ((ad_type == 0x02u || ad_type == 0x03u) &&
            ad_uuid16_list_has(ad_data, ad_data_len, 0x1812u)) {
            if (reason) *reason = "BLE HID service advertisement";
            return true;
        }
        if (ad_type == 0x16u && ad_data_len >= 2) {
            uint16_t service_uuid = (uint16_t)ad_data[0] | ((uint16_t)ad_data[1] << 8u);
            if (service_uuid == 0x1812u) {
                if (reason) *reason = "BLE HID service data";
                return true;
            }
        }
        if (ad_type == 0x19u && ad_data_len >= 2) {
            uint16_t appearance = (uint16_t)ad_data[0] | ((uint16_t)ad_data[1] << 8u);
            uint16_t appearance_category = appearance >> 6u;
            if (appearance_category == 15u) {
                if (reason) *reason = "BLE HID appearance";
                return true;
            }
        }
#endif

#if WAKE_ON_BLE_CONTROLLER_NAME
        if ((ad_type == 0x08u || ad_type == 0x09u) &&
            ad_name_looks_like_controller(ad_data, ad_data_len)) {
            if (reason) *reason = "BLE controller name";
            return true;
        }
#endif

        pos += field_len;
    }

    return false;
}

static bool le_advertising_report_matches(const uint8_t *params, size_t len, const char **reason) {
    if (len < 2 || params[0] != HCI_SUBEVENT_LE_ADVERTISING_REPORT) return false;

    uint8_t reports = params[1];
    size_t pos = 2;
    for (uint8_t report = 0; report < reports; report++) {
        if (pos + 9u > len) break;
        uint8_t event_type = params[pos];
        uint8_t addr_type = params[pos + 1u];
        const uint8_t *addr = &params[pos + 2u];
        uint8_t data_len = params[pos + 8u];
        if (pos + 9u + data_len > len) break;
        standby_adv_reports++;
        if ((standby_adv_reports & 0x0fu) == 1u) {
            debug_log("standby adv #%lu evt=0x%02x type=%u addr=%02x:%02x:%02x:%02x:%02x:%02x data=%u",
                      standby_adv_reports, event_type, addr_type, addr[5], addr[4], addr[3],
                      addr[2], addr[1], addr[0], data_len);
        }

#if WAKE_ON_KNOWN_BLE_PEER
        if (known_ble_peer_matches(addr_type, addr)) {
            if (reason) *reason = "known BLE peer advertisement";
            return true;
        }
#endif

#if WAKE_ON_CONNECTABLE_BLE_ADV
        if (event_type == 0x00u || event_type == 0x01u) {
            if (reason) *reason = event_type == 0x01u
                                      ? "BLE directed advertisement"
                                      : "BLE connectable advertisement";
            return true;
        }
#endif

        bool learn_peer = false;
        if (le_advertising_data_matches(&params[pos + 9u], data_len, reason, &learn_peer)) {
            if (learn_peer) (void)remember_ble_peer(addr_type, addr);
            return true;
        }

        pos += 9u + data_len + 1u;
    }

    return false;
}

static bool le_directed_advertising_report_matches(const uint8_t *params, size_t len, const char **reason) {
    if (len < 2 || params[0] != HCI_SUBEVENT_LE_DIRECTED_ADVERTISING_REPORT) return false;

#if WAKE_ON_BLE_DIRECTED_ADV
    uint8_t reports = params[1];
    size_t pos = 2;
    for (uint8_t report = 0; report < reports; report++) {
        if (pos + 16u > len) break;
        uint8_t event_type = params[pos];
        if (event_type == 0x01u) {
            if (reason) *reason = "BLE directed advertisement";
            return true;
        }
        pos += 16u;
    }
#else
    (void)reason;
#endif

    return false;
}

static bool le_extended_advertising_report_matches(const uint8_t *params, size_t len, const char **reason) {
    if (len < 2 || params[0] != HCI_SUBEVENT_LE_EXTENDED_ADVERTISING_REPORT) return false;

    uint8_t reports = params[1];
    size_t pos = 2;
    for (uint8_t report = 0; report < reports; report++) {
        if (pos + 24u > len) break;

        uint16_t event_type = (uint16_t)params[pos] | ((uint16_t)params[pos + 1u] << 8u);
        uint8_t addr_type = params[pos + 2u];
        const uint8_t *addr = &params[pos + 3u];
        uint8_t data_len = params[pos + 23u];
        if (pos + 24u + data_len > len) break;
        standby_adv_reports++;
        if ((standby_adv_reports & 0x0fu) == 1u) {
            debug_log("standby ext adv #%lu evt=0x%04x type=%u addr=%02x:%02x:%02x:%02x:%02x:%02x data=%u",
                      standby_adv_reports, event_type, addr_type, addr[5], addr[4], addr[3],
                      addr[2], addr[1], addr[0], data_len);
        }

#if WAKE_ON_KNOWN_BLE_PEER
        if (known_ble_peer_matches(addr_type, addr)) {
            if (reason) *reason = "known BLE peer extended advertisement";
            return true;
        }
#endif

#if WAKE_ON_CONNECTABLE_BLE_ADV
        if (event_type & 0x0001u) {
            if (reason) *reason = "BLE connectable extended advertisement";
            return true;
        }
        if (event_type & 0x0004u) {
            if (reason) *reason = "BLE directed extended advertisement";
            return true;
        }
#endif

        bool learn_peer = false;
        if (le_advertising_data_matches(&params[pos + 24u], data_len, reason, &learn_peer)) {
            if (learn_peer) (void)remember_ble_peer(addr_type, addr);
            return true;
        }

        pos += 24u + data_len;
    }

    return false;
}

bool wake_policy_packet_matches(hci_packet_type_t type, const uint8_t *packet, uint16_t len, const char **reason) {
    if (reason) *reason = NULL;
    if (type != HCI_PKT_EVENT || len < 2) return false;

    if (packet[0] == HCI_EVENT_CONNECTION_REQUEST) {
        if (reason) *reason = "classic connection request";
        return true;
    }

    if (packet[0] == HCI_EVENT_LE_META && len >= 4 && packet[2] == HCI_SUBEVENT_LE_ADVERTISING_REPORT) {
#if WAKE_ON_ANY_BLE_ADV
        if (reason) *reason = "BLE advertisement";
        return true;
#else
        return le_advertising_report_matches(&packet[2], (size_t)len - 2u, reason);
#endif
    }

    if (packet[0] == HCI_EVENT_LE_META && len >= 4 &&
        packet[2] == HCI_SUBEVENT_LE_DIRECTED_ADVERTISING_REPORT) {
        return le_directed_advertising_report_matches(&packet[2], (size_t)len - 2u, reason);
    }

    if (packet[0] == HCI_EVENT_LE_META && len >= 4 &&
        packet[2] == HCI_SUBEVENT_LE_EXTENDED_ADVERTISING_REPORT) {
        return le_extended_advertising_report_matches(&packet[2], (size_t)len - 2u, reason);
    }

    return false;
}
