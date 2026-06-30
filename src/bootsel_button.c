#include "bootsel_button.h"

#include "debug_log.h"
#include "wake_policy.h"

#include "hardware/gpio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "pico/flash.h"
#include "pico/time.h"

#if PICO_RP2350
#include "hardware/regs/sio.h"
#endif

#ifndef BOOTSEL_CLEAR_HOLD_MS
#define BOOTSEL_CLEAR_HOLD_MS 5000u
#endif

#define BOOTSEL_POLL_MS 100u

static uint32_t last_poll_ms;
static uint32_t pressed_ms;
static bool hold_fired;

static void __no_inline_not_in_flash_func(read_bootsel_cb)(void *param) {
    bool *out = (bool *)param;
    const uint cs_pin_index = 1u;

    hw_write_masked(&ioqspi_hw->io[cs_pin_index].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    for (volatile int i = 0; i < 1000; ++i) {
        __asm volatile("nop");
    }

#if PICO_RP2350
    *out = !(sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS);
#else
    *out = !(sio_hw->gpio_hi_in & (1u << cs_pin_index));
#endif

    hw_write_masked(&ioqspi_hw->io[cs_pin_index].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
}

static bool read_bootsel(void) {
    bool pressed = false;
    int rc = flash_safe_execute(read_bootsel_cb, &pressed, 100);
    return rc == PICO_OK && pressed;
}

void bootsel_button_task(void) {
#if ENABLE_BOOTSEL_CLEAR_WAKE_PEERS
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_poll_ms < BOOTSEL_POLL_MS) return;
    last_poll_ms = now;

    bool pressed = read_bootsel();
    if (!pressed) {
        pressed_ms = 0;
        hold_fired = false;
        return;
    }

    if (pressed_ms < BOOTSEL_CLEAR_HOLD_MS) {
        pressed_ms += BOOTSEL_POLL_MS;
    }

    if (!hold_fired && pressed_ms >= BOOTSEL_CLEAR_HOLD_MS) {
        hold_fired = true;
        debug_log("BOOTSEL held: clearing BLE wake peers");
        wake_policy_clear_known_peers();
    }
#endif
}
