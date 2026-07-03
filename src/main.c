#include "pico/stdlib.h"
#include "bsp/board_api.h"
#include "hardware/clocks.h"
#include "tusb.h"

#include "bootsel_button.h"
#include "debug_log.h"
#include "hci_host_minimal.h"
#include "hci_transport.h"
#include "power_supervisor.h"
#include "usb_bth_bridge.h"
#include "usb_hid_wake.h"
#include "wake_policy.h"

static const hci_transport_t *transport;
static power_state_t last_state = POWER_STATE_UNKNOWN;
static bool standby_configured;

static void configure_system_clock(void) {
#if SYS_CLOCK_KHZ > 0
    if (!set_sys_clock_khz(SYS_CLOCK_KHZ, false)) {
        panic("Cannot set SYS_CLOCK_KHZ=%u", (unsigned)SYS_CLOCK_KHZ);
    }
#endif
}

static const hci_transport_t *select_transport(void) {
#if HCI_BACKEND_esp32_uart
    return backend_esp32_uart_get_transport();
#elif HCI_BACKEND_cyw43
    return backend_cyw43_get_transport();
#else
    return backend_stub_get_transport();
#endif
}

static void backend_rx(hci_packet_type_t type, const uint8_t *packet, uint16_t len) {
    power_state_t state = power_supervisor_get_state();
    if (state == POWER_STATE_HOST_ON_USB_HCI && usb_bth_bridge_is_enabled()) {
        wake_policy_observe_host_packet(type, packet, len);
        usb_bth_bridge_backend_packet(type, packet, len);
        return;
    }

    if (state == POWER_STATE_STANDBY_HCI_HOST) {
        hci_host_minimal_packet(type, packet, len);

        const char *reason = NULL;
        if (wake_policy_packet_matches(type, packet, len, &reason)) {
            power_supervisor_request_wake(reason);
        }
    }
}

static void enter_host_usb_mode(void) {
    standby_configured = false;
    usb_hid_wake_set_standby(false);
    usb_bth_bridge_set_enabled(true);
    tud_connect();
    debug_log("USB Bluetooth HCI bridge active");
}

static void enter_standby_mode(void) {
    usb_hid_wake_set_standby(true);
    usb_bth_bridge_set_enabled(false);
    if (usb_hid_wake_keep_usb_connected()) {
        tud_connect();
    } else {
        tud_disconnect();
    }
    if (!standby_configured) {
        standby_configured = hci_host_configure_standby_wake();
        if (!standby_configured) {
            debug_log("standby HCI host configuration failed");
        }
    }
}

int main(void) {
    configure_system_clock();
    stdio_init_all();
    sleep_ms(300);
    board_init();

    debug_log("pico-usb-bt-wake boot");
    wake_policy_init();
    power_supervisor_init();

    transport = select_transport();
    transport->set_rx_callback(backend_rx);
    if (!transport->init()) {
        debug_log("backend init failed");
    }

    hci_host_minimal_init(transport);
    usb_bth_bridge_init(transport);
    usb_hid_wake_init();

    tusb_init();
    if (power_supervisor_get_state() != POWER_STATE_HOST_ON_USB_HCI) {
        if (usb_hid_wake_keep_usb_connected()) {
            usb_hid_wake_set_standby(true);
            tud_connect();
        } else {
            tud_disconnect();
        }
    }

    while (true) {
        power_supervisor_task();
        power_state_t state = power_supervisor_get_state();
        if (state != last_state) {
            if (state == POWER_STATE_HOST_ON_USB_HCI) {
                enter_host_usb_mode();
            } else if (state == POWER_STATE_STANDBY_HCI_HOST) {
                enter_standby_mode();
            } else if (state == POWER_STATE_HOST_SHUTTING_DOWN || state == POWER_STATE_HOST_OFF ||
                       state == POWER_STATE_WAIT_WAKE_SENSE_SETTLE) {
                usb_hid_wake_set_standby(true);
                usb_bth_bridge_set_enabled(false);
                if (usb_hid_wake_keep_usb_connected()) {
                    tud_connect();
                } else {
                    tud_disconnect();
                }
                standby_configured = false;
            } else if (state == POWER_STATE_WAKE_PULSE) {
                standby_configured = false;
            }
            last_state = state;
        }

        if (state == POWER_STATE_HOST_ON_USB_HCI) {
            tud_task();
            usb_bth_bridge_task();
            transport->task();
            usb_bth_bridge_task();
            tud_task();
        } else if (state == POWER_STATE_STANDBY_HCI_HOST) {
            transport->task();
            if (!standby_configured) enter_standby_mode();
        } else if (state == POWER_STATE_WAIT_WAKE_SENSE_SETTLE) {
            sleep_ms(1);
        } else {
            transport->task();
        }
        if (state != POWER_STATE_HOST_ON_USB_HCI && usb_hid_wake_keep_usb_connected()) {
            tud_task();
            usb_hid_wake_task();
        }
        wake_policy_task();
        bootsel_button_task();
        debug_log_task();
        tight_loop_contents();
    }
}
