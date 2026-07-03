#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    POWER_STATE_UNKNOWN = 0,
    POWER_STATE_HOST_OFF,
    POWER_STATE_STANDBY_HCI_HOST,
    POWER_STATE_WAKE_PULSE,
    POWER_STATE_WAIT_WAKE_SENSE_SETTLE,
    POWER_STATE_WAIT_HOST_BOOT,
    POWER_STATE_HOST_ON_USB_HCI,
    POWER_STATE_HOST_SHUTTING_DOWN,
    POWER_STATE_FAULT,
} power_state_t;

void power_supervisor_init(void);
void power_supervisor_task(void);
bool power_supervisor_pwr_ok(void);
bool power_supervisor_usb_vbus_present(void);
power_state_t power_supervisor_get_state(void);
void power_supervisor_pulse_power_button_ms(uint32_t ms);
void power_supervisor_request_wake(const char *reason);
const char *power_supervisor_last_wake_reason(void);
void power_supervisor_debug_force_standby(bool force);
