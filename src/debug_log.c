#include "debug_log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if ENABLE_CDC_DEBUG
#include "power_supervisor.h"

#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#endif

#define LOG_RING_SIZE 4096u
#define DEBUG_COMMAND_SIZE 32u

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
    } else if (debug_command_equals("state")) {
        debug_log("debug state=%d pwr_ok=%u usb_vbus=%u last_wake=%s",
                  (int)power_supervisor_get_state(),
                  power_supervisor_pwr_ok() ? 1u : 0u,
                  power_supervisor_usb_vbus_present() ? 1u : 0u,
                  power_supervisor_last_wake_reason());
    } else if (debug_command_equals("standby")) {
        power_supervisor_debug_force_standby(true);
    } else if (debug_command_equals("host")) {
        power_supervisor_debug_force_standby(false);
    } else if (debug_command_equals("wake")) {
        debug_log("debug wake requested");
        power_supervisor_request_wake("debug command");
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
