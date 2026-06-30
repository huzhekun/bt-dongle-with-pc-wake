#include "usb_hid_wake.h"

#include "debug_log.h"
#include "pico/time.h"
#include "tusb.h"

#ifndef STANDBY_HID_WAKE_KEY
#define STANDBY_HID_WAKE_KEY 0x68
#endif

#define HID_WAKE_KEYPRESS_MS 80u

static bool standby_mode;
static bool key_pending;
static bool key_down;
static absolute_time_t release_at;

void usb_hid_wake_init(void) {
    standby_mode = false;
    key_pending = false;
    key_down = false;
    release_at = nil_time;
}

void usb_hid_wake_set_standby(bool standby) {
    standby_mode = standby;
}

bool usb_hid_wake_keep_usb_connected(void) {
#if ENABLE_STANDBY_HID_KEYBOARD
    return true;
#else
    return false;
#endif
}

void usb_hid_wake_request_keypress(void) {
#if ENABLE_STANDBY_HID_KEYBOARD
    key_pending = true;
    if (tud_suspended()) {
        bool woke = tud_remote_wakeup();
        debug_log("HID remote wake %s", woke ? "requested" : "not enabled");
    }
#endif
}

void usb_hid_wake_task(void) {
#if ENABLE_STANDBY_HID_KEYBOARD
    if (!standby_mode && !key_pending && !key_down) return;
    if (!tud_mounted()) return;

    if (key_down) {
        if (time_reached(release_at) && tud_hid_ready()) {
            tud_hid_keyboard_report(0, 0, NULL);
            key_down = false;
            release_at = nil_time;
        }
        return;
    }

    if (key_pending && tud_hid_ready()) {
        uint8_t keycode[6] = {0};
        keycode[0] = (uint8_t)STANDBY_HID_WAKE_KEY;
        if (tud_hid_keyboard_report(0, 0, keycode)) {
            key_pending = false;
            key_down = true;
            release_at = make_timeout_time_ms(HID_WAKE_KEYPRESS_MS);
            debug_log("HID wake key sent: 0x%02x", (unsigned)keycode[0]);
        }
    }
#endif
}

#if ENABLE_STANDBY_HID_KEYBOARD
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}
#endif
