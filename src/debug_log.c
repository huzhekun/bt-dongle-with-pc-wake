#include "debug_log.h"
#include "wake_policy.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if ENABLE_CDC_DEBUG
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#endif

#define LOG_RING_SIZE 4096u
#define DEBUG_COMMAND_SIZE 96u

static char log_ring[LOG_RING_SIZE];
static volatile uint16_t log_head;
static volatile uint16_t log_tail;

#if ENABLE_CDC_DEBUG
static char command_buf[DEBUG_COMMAND_SIZE];
static uint8_t command_len;
#endif

static void log_ring_push_char(char ch) {
    uint16_t next = (uint16_t)((log_head + 1u) % LOG_RING_SIZE);
    if (next == log_tail) {
        log_tail = (uint16_t)((log_tail + 1u) % LOG_RING_SIZE);
    }
    log_ring[log_head] = ch;
    log_head = next;
}

static void log_ring_push(const char *s) {
    while (*s) log_ring_push_char(*s++);
}

#if ENABLE_CDC_DEBUG

static int hex_nibble(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool parse_mac_le(const char *text, uint8_t *addr) {
    uint8_t parsed[6];
    for (uint8_t i = 0; i < 6; i++) {
        int hi = hex_nibble(text[i * 3u]);
        int lo = hex_nibble(text[i * 3u + 1u]);
        if (hi < 0 || lo < 0) return false;
        parsed[5u - i] = (uint8_t)((hi << 4) | lo);
        if (i < 5u && text[i * 3u + 2u] != ':') return false;
    }
    addr[0] = parsed[0]; addr[1] = parsed[1]; addr[2] = parsed[2];
    addr[3] = parsed[3]; addr[4] = parsed[4]; addr[5] = parsed[5];
    return text[17] == '\0' || text[17] == ' ' || text[17] == '\t';
}

static const char *skip_spaces(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static bool debug_command_equals(const char *want) {
    const char *got = command_buf;
    while (*got && *want) {
        char got_ch = *got++;
        char want_ch = *want++;
        if (got_ch >= 'A' && got_ch <= 'Z') got_ch = (char)(got_ch - 'A' + 'a');
        if (got_ch != want_ch) return false;
    }
    return *got == '\0' && *want == '\0';
}

static void debug_handle_command(void) {
    command_buf[command_len] = '\0';

    if (debug_command_equals("bootsel") || debug_command_equals("boot")) {
        debug_log("Entering BOOTSEL");
        sleep_ms(50);
        reset_usb_boot(0, 0);
    } else if (debug_command_equals("clear")) {
        wake_policy_clear_known_peers();
    } else if (debug_command_equals("save")) {
        debug_log("Wake sync save requested");
    } else if (strncmp(command_buf, "addr ", 5) == 0) {
        uint8_t addr[6];
        if (parse_mac_le(skip_spaces(&command_buf[5]), addr)) {
            (void)wake_policy_set_local_adapter(addr);
        } else {
            debug_log("Bad addr command");
        }
    } else if (strncmp(command_buf, "peer ", 5) == 0) {
        uint8_t addr[6];
        if (parse_mac_le(skip_spaces(&command_buf[5]), addr)) {
            (void)wake_policy_add_known_peer(0, addr);
        } else {
            debug_log("Bad peer command");
        }
    } else if (command_len > 0u) {
        debug_log("Unknown debug command: %s", command_buf);
    }

    command_len = 0;
}

static void debug_poll_commands(void) {
    while (true) {
        int ch = getchar_timeout_us(0);
        if (ch < 0) break;

        if (ch == '\r' || ch == '\n') {
            debug_handle_command();
        } else if (ch == '\b' || ch == 127) {
            if (command_len > 0u) command_len--;
        } else if (command_len + 1u < DEBUG_COMMAND_SIZE) {
            command_buf[command_len++] = (char)ch;
        }
    }
}
#endif

void debug_log(const char *fmt, ...) {
    char line[160];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

#if !ENABLE_CDC_DEBUG
    puts(line);
#endif
    log_ring_push(line);
    log_ring_push("\r\n");
}

void debug_log_task(void) {
#if ENABLE_CDC_DEBUG
    if (!stdio_usb_connected()) return;
    debug_poll_commands();
    while (log_tail != log_head) {
        putchar(log_ring[log_tail]);
        log_tail = (uint16_t)((log_tail + 1u) % LOG_RING_SIZE);
    }
#endif
}
