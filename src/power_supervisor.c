#include "power_supervisor.h"

#include "board_config.h"
#include "debug_log.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "usb_hid_wake.h"

#if HCI_BACKEND_cyw43 && ENABLE_CYW43_STATUS_LED
#include "pico/cyw43_arch.h"
#endif

static power_state_t state = POWER_STATE_UNKNOWN;
static bool wake_requested;
static bool debug_force_standby;
static bool pwr_ok_filtered;
static bool pwr_ok_last_raw;
static bool usb_vbus_filtered;
static bool usb_vbus_last_raw;
static const char *wake_reason = "none";
static absolute_time_t state_deadline;
static absolute_time_t cooldown_until;
static absolute_time_t pwr_ok_changed_at;
static absolute_time_t usb_vbus_changed_at;
static absolute_time_t standby_armed_at;

#define POWER_SENSE_DEBOUNCE_MS 250u
#ifndef STANDBY_WAKE_ARM_DELAY_MS
#define STANDBY_WAKE_ARM_DELAY_MS 60000u
#endif
#ifndef POWER_BUTTON_PULSE_MS
#define POWER_BUTTON_PULSE_MS 200u
#endif
#ifndef POST_WAKE_SENSE_SETTLE_MS
#define POST_WAKE_SENSE_SETTLE_MS 10000u
#endif
#ifndef STATUS_LED_BLINK_MS
#define STATUS_LED_BLINK_MS 1000u
#endif

static bool pin_read_or_default(int pin, bool default_value) {
    if (pin < 0) return default_value;
    return gpio_get((uint)pin);
}

static bool debounce_bool(bool raw, bool *last_raw, bool *filtered, absolute_time_t *changed_at) {
    if (raw != *last_raw) {
        *last_raw = raw;
        *changed_at = make_timeout_time_ms(POWER_SENSE_DEBOUNCE_MS);
    }
    if (raw != *filtered && time_reached(*changed_at)) {
        *filtered = raw;
    }
    return *filtered;
}

static absolute_time_t standby_arm_deadline(void) {
    if (debug_force_standby || STANDBY_WAKE_ARM_DELAY_MS == 0u) return nil_time;
    return make_timeout_time_ms(STANDBY_WAKE_ARM_DELAY_MS);
}

static bool standby_wake_armed(void) {
    return is_nil_time(standby_armed_at) || time_reached(standby_armed_at);
}

static void status_led_put(bool on) {
#if PIN_LED_STATUS >= 0
    gpio_put(PIN_LED_STATUS, on);
#endif
#if HCI_BACKEND_cyw43 && ENABLE_CYW43_STATUS_LED && defined(CYW43_WL_GPIO_LED_PIN)
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
#else
    (void)on;
#endif
}

static void update_status_led(void) {
    bool on = false;
    if (state == POWER_STATE_STANDBY_HCI_HOST) {
        on = true;
    } else if (state == POWER_STATE_HOST_OFF && !standby_wake_armed()) {
        uint32_t blink_ms = STATUS_LED_BLINK_MS == 0u ? 1000u : STATUS_LED_BLINK_MS;
        on = ((to_ms_since_boot(get_absolute_time()) / blink_ms) & 1u) == 0u;
    }
    status_led_put(on);
}

static void update_power_inputs(void) {
    if (debug_force_standby) {
        pwr_ok_last_raw = pwr_ok_filtered = false;
        usb_vbus_last_raw = usb_vbus_filtered = false;
        return;
    }

    (void)debounce_bool(pin_read_or_default(PIN_PWR_OK_SENSE, true),
                        &pwr_ok_last_raw, &pwr_ok_filtered, &pwr_ok_changed_at);
    (void)debounce_bool(pin_read_or_default(PIN_USB_VBUS_SENSE, true),
                        &usb_vbus_last_raw, &usb_vbus_filtered, &usb_vbus_changed_at);
}

static void power_button_release(void) {
#if PIN_PWR_BUTTON_OUT >= 0
    gpio_put(PIN_PWR_BUTTON_OUT, 0);
    gpio_set_dir(PIN_PWR_BUTTON_OUT, GPIO_IN);
    gpio_pull_up(PIN_PWR_BUTTON_OUT);
#endif
}

static void power_button_press(void) {
#if PIN_PWR_BUTTON_OUT >= 0
    gpio_put(PIN_PWR_BUTTON_OUT, 0);
    gpio_set_dir(PIN_PWR_BUTTON_OUT, GPIO_OUT);
#endif
}

void power_supervisor_init(void) {
#if PIN_PWR_OK_SENSE >= 0
    gpio_init(PIN_PWR_OK_SENSE);
    gpio_set_dir(PIN_PWR_OK_SENSE, GPIO_IN);
    gpio_pull_down(PIN_PWR_OK_SENSE);
#endif
#if PIN_USB_VBUS_SENSE >= 0
    gpio_init(PIN_USB_VBUS_SENSE);
    gpio_set_dir(PIN_USB_VBUS_SENSE, GPIO_IN);
    gpio_pull_down(PIN_USB_VBUS_SENSE);
#endif
#if PIN_PWR_BUTTON_OUT >= 0
    gpio_init(PIN_PWR_BUTTON_OUT);
    power_button_release();
#endif
#if PIN_LED_STATUS >= 0
    gpio_init(PIN_LED_STATUS);
    gpio_set_dir(PIN_LED_STATUS, GPIO_OUT);
    gpio_put(PIN_LED_STATUS, 0);
#endif
#if PIN_LED_FAULT >= 0
    gpio_init(PIN_LED_FAULT);
    gpio_set_dir(PIN_LED_FAULT, GPIO_OUT);
#endif
    pwr_ok_filtered = pwr_ok_last_raw = pin_read_or_default(PIN_PWR_OK_SENSE, true);
    usb_vbus_filtered = usb_vbus_last_raw = pin_read_or_default(PIN_USB_VBUS_SENSE, true);
    pwr_ok_changed_at = make_timeout_time_ms(0);
    usb_vbus_changed_at = make_timeout_time_ms(0);

    state = (pwr_ok_filtered && usb_vbus_filtered)
                ? POWER_STATE_HOST_ON_USB_HCI
                : POWER_STATE_HOST_OFF;
    standby_armed_at = state == POWER_STATE_HOST_OFF ? standby_arm_deadline() : nil_time;
}

bool power_supervisor_pwr_ok(void) {
    return pwr_ok_filtered;
}

bool power_supervisor_usb_vbus_present(void) {
    return usb_vbus_filtered;
}

power_state_t power_supervisor_get_state(void) {
    return state;
}

const char *power_supervisor_last_wake_reason(void) {
    return wake_reason;
}

void power_supervisor_debug_force_standby(bool force) {
    debug_force_standby = force;
    if (force) {
        pwr_ok_last_raw = pwr_ok_filtered = false;
        usb_vbus_last_raw = usb_vbus_filtered = false;
        standby_armed_at = nil_time;
        wake_requested = false;
        debug_log("debug force standby enabled");
    } else {
        pwr_ok_last_raw = pwr_ok_filtered = pin_read_or_default(PIN_PWR_OK_SENSE, true);
        usb_vbus_last_raw = usb_vbus_filtered = pin_read_or_default(PIN_USB_VBUS_SENSE, true);
        debug_log("debug force standby disabled");
    }
}

void power_supervisor_pulse_power_button_ms(uint32_t ms) {
#if PIN_PWR_BUTTON_OUT >= 0
    power_button_press();
    sleep_ms(ms);
    power_button_release();
#else
    (void)ms;
#endif
}

void power_supervisor_request_wake(const char *reason) {
    if (state != POWER_STATE_STANDBY_HCI_HOST && state != POWER_STATE_HOST_OFF) return;
    if (!time_reached(cooldown_until)) return;
    if (!standby_wake_armed()) return;
    wake_reason = reason ? reason : "wake policy";
    wake_requested = true;
}

static void set_state(power_state_t next) {
    if (state == next) return;
    debug_log("power state %d -> %d", state, next);
    state = next;
    if (state == POWER_STATE_HOST_OFF) {
        standby_armed_at = standby_arm_deadline();
        wake_requested = false;
        if (!is_nil_time(standby_armed_at)) {
            debug_log("standby wake arm delay: %lu ms", (unsigned long)STANDBY_WAKE_ARM_DELAY_MS);
        }
    } else if (state != POWER_STATE_STANDBY_HCI_HOST) {
        standby_armed_at = nil_time;
    }
}

void power_supervisor_task(void) {
    update_power_inputs();

    switch (state) {
    case POWER_STATE_HOST_OFF:
        if (power_supervisor_pwr_ok() && power_supervisor_usb_vbus_present()) {
            set_state(POWER_STATE_HOST_ON_USB_HCI);
        } else if (standby_wake_armed()) {
            set_state(POWER_STATE_STANDBY_HCI_HOST);
        }
        break;

    case POWER_STATE_STANDBY_HCI_HOST:
        if (power_supervisor_pwr_ok() && power_supervisor_usb_vbus_present()) {
            set_state(POWER_STATE_HOST_ON_USB_HCI);
        } else if (wake_requested) {
            wake_requested = false;
#if ENABLE_POWER_BUTTON_WAKE || ENABLE_STANDBY_HID_KEYBOARD
            set_state(POWER_STATE_WAKE_PULSE);
#else
            debug_log("wake ignored with wake outputs disabled: %s", wake_reason);
#endif
        }
        break;

    case POWER_STATE_WAKE_PULSE:
        usb_hid_wake_request_keypress();
#if ENABLE_POWER_BUTTON_WAKE
        if (!power_supervisor_pwr_ok()) {
            debug_log("wake pulse: %s", wake_reason);
            power_supervisor_pulse_power_button_ms(POWER_BUTTON_PULSE_MS);
        } else {
            debug_log("wake pulse skipped: sense already high");
        }
#else
        debug_log("HID wake key requested: %s", wake_reason);
#endif
        cooldown_until = make_timeout_time_ms(POST_WAKE_SENSE_SETTLE_MS);
        state_deadline = make_timeout_time_ms(POST_WAKE_SENSE_SETTLE_MS);
        set_state(POWER_STATE_WAIT_WAKE_SENSE_SETTLE);
        break;

    case POWER_STATE_WAIT_WAKE_SENSE_SETTLE:
        if (power_supervisor_pwr_ok() && power_supervisor_usb_vbus_present()) {
            set_state(POWER_STATE_HOST_ON_USB_HCI);
        } else if (time_reached(state_deadline)) {
            state_deadline = make_timeout_time_ms(120000);
            set_state(POWER_STATE_WAIT_HOST_BOOT);
        }
        break;

    case POWER_STATE_WAIT_HOST_BOOT:
        if (power_supervisor_pwr_ok() && power_supervisor_usb_vbus_present()) {
            set_state(POWER_STATE_HOST_ON_USB_HCI);
        } else if (time_reached(state_deadline)) {
            set_state(POWER_STATE_HOST_OFF);
        }
        break;

    case POWER_STATE_HOST_ON_USB_HCI:
        if (!power_supervisor_pwr_ok()) {
            set_state(POWER_STATE_HOST_SHUTTING_DOWN);
            state_deadline = make_timeout_time_ms(1500);
            wake_requested = false;
        }
        break;

    case POWER_STATE_HOST_SHUTTING_DOWN:
        if (time_reached(state_deadline)) set_state(POWER_STATE_HOST_OFF);
        break;

    case POWER_STATE_FAULT:
#if PIN_LED_FAULT >= 0
        gpio_put(PIN_LED_FAULT, (to_ms_since_boot(get_absolute_time()) / 100u) & 1u);
#endif
        break;

    case POWER_STATE_UNKNOWN:
    default:
        set_state(POWER_STATE_HOST_OFF);
        break;
    }

    update_status_led();
}
