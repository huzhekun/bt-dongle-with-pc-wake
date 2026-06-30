#include "hci_transport.h"

#include "board_config.h"
#include "debug_log.h"
#include "h4_parser.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"

#include <string.h>

#if ESP32_HCI_UART_ID == 0
#define HCI_UART uart0
#define HCI_UART_IRQ UART0_IRQ
#else
#define HCI_UART uart1
#define HCI_UART_IRQ UART1_IRQ
#endif

#define UART_RX_RING_SIZE 4096u
#define UART_RX_RING_MASK (UART_RX_RING_SIZE - 1u)

static uint8_t rx_ring[UART_RX_RING_SIZE];
static volatile uint32_t rx_head;
static volatile uint32_t rx_tail;
static volatile uint32_t rx_overflows;
static hci_rx_cb_t rx_cb;
static uint8_t parser_packet[HCI_ACL_MAX_LEN + 8u];
static h4_parser_t parser;

static void esp32_uart_irq(void) {
    while (uart_is_readable(HCI_UART)) {
        uint32_t value = uart_get_hw(HCI_UART)->dr;
        if ((value & (UART_UARTDR_OE_BITS | UART_UARTDR_BE_BITS | UART_UARTDR_PE_BITS | UART_UARTDR_FE_BITS)) != 0) {
            uart_get_hw(HCI_UART)->rsr = 0;
            continue;
        }

        uint32_t next = (rx_head + 1u) & UART_RX_RING_MASK;
        if (next == rx_tail) {
            rx_overflows++;
            continue;
        }
        rx_ring[rx_head] = (uint8_t)value;
        rx_head = next;
    }
}

static void esp32_set_rx_callback(hci_rx_cb_t cb) {
    rx_cb = cb;
    h4_parser_set_callback(&parser, cb);
}

static bool esp32_reset_controller(void) {
    h4_parser_reset(&parser);
    rx_head = rx_tail = 0;

#if PIN_ESP32_RESET >= 0
    gpio_init(PIN_ESP32_RESET);
    gpio_set_dir(PIN_ESP32_RESET, GPIO_OUT);
    gpio_put(PIN_ESP32_RESET, 0);
    sleep_ms(100);
    gpio_put(PIN_ESP32_RESET, 1);
    sleep_ms(700);
#else
    sleep_ms(20);
#endif
    return true;
}

static bool esp32_init(void) {
    h4_parser_init(&parser, parser_packet, sizeof(parser_packet), rx_cb);

    uart_init(HCI_UART, ESP32_HCI_UART_BAUD);
    gpio_set_function(PIN_ESP32_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(PIN_ESP32_UART_RX, GPIO_FUNC_UART);

#if ENABLE_UART_FLOW_CONTROL
    gpio_set_function(PIN_ESP32_UART_CTS, GPIO_FUNC_UART);
    gpio_set_function(PIN_ESP32_UART_RTS, GPIO_FUNC_UART);
    uart_set_hw_flow(HCI_UART, true, true);
#else
    uart_set_hw_flow(HCI_UART, false, false);
#endif

    uart_set_format(HCI_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(HCI_UART, true);
    irq_set_exclusive_handler(HCI_UART_IRQ, esp32_uart_irq);
    irq_set_enabled(HCI_UART_IRQ, true);
    uart_set_irq_enables(HCI_UART, true, false);

    debug_log("ESP32 HCI UART%d on TX=%d RX=%d baud=%u", ESP32_HCI_UART_ID,
              PIN_ESP32_UART_TX, PIN_ESP32_UART_RX, (unsigned)ESP32_HCI_UART_BAUD);
    return esp32_reset_controller();
}

static bool esp32_send(hci_packet_type_t type, const uint8_t *packet, uint16_t len) {
    uint8_t h4_type = (uint8_t)type;
    uart_write_blocking(HCI_UART, &h4_type, 1);
    if (len) uart_write_blocking(HCI_UART, packet, len);
    uart_tx_wait_blocking(HCI_UART);
    return true;
}

static void esp32_task(void) {
    while (rx_tail != rx_head) {
        uint8_t byte = rx_ring[rx_tail];
        rx_tail = (rx_tail + 1u) & UART_RX_RING_MASK;
        h4_parser_feed_byte(&parser, byte);
    }
}

static const hci_transport_t transport = {
    .init = esp32_init,
    .reset_controller = esp32_reset_controller,
    .send = esp32_send,
    .set_rx_callback = esp32_set_rx_callback,
    .task = esp32_task,
};

const hci_transport_t *backend_esp32_uart_get_transport(void) {
    return &transport;
}
