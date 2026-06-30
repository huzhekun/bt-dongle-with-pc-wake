#include "tusb.h"

#define USB_VID 0xCafe
#define USB_PID 0x4017
#define USB_BCD 0x0100

enum {
    ITF_NUM_BTH = 0,
    ITF_NUM_BTH_ISO,
#if ENABLE_STANDBY_HID_KEYBOARD
    ITF_NUM_HID,
#endif
#if ENABLE_CDC_DEBUG
    ITF_NUM_CDC,
    ITF_NUM_CDC_DATA,
#endif
    ITF_NUM_TOTAL
};

enum {
    STR_LANGID = 0,
    STR_MANUFACTURER,
    STR_PRODUCT,
    STR_SERIAL,
    STR_BTH,
#if ENABLE_STANDBY_HID_KEYBOARD
    STR_HID,
#endif
#if ENABLE_CDC_DEBUG
    STR_CDC,
#endif
};

#define EPNUM_BTH_EVENT 0x81
#define EPNUM_BTH_ACL_IN 0x82
#define EPNUM_BTH_ACL_OUT 0x02

#if ENABLE_STANDBY_HID_KEYBOARD
#define EPNUM_HID 0x86
#endif

#if ENABLE_CDC_DEBUG
#define EPNUM_CDC_NOTIF 0x84
#define EPNUM_CDC_OUT 0x04
#define EPNUM_CDC_IN 0x85
#endif

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_BTH_DESC_LEN + \
                          (ENABLE_STANDBY_HID_KEYBOARD ? TUD_HID_DESC_LEN : 0) + \
                          (ENABLE_CDC_DEBUG ? TUD_CDC_DESC_LEN : 0))

#if ENABLE_STANDBY_HID_KEYBOARD
static const uint8_t desc_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};
#endif

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
#if ENABLE_CDC_DEBUG || ENABLE_STANDBY_HID_KEYBOARD
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
#else
    .bDeviceClass = TUSB_CLASS_WIRELESS_CONTROLLER,
    .bDeviceSubClass = TUD_BT_APP_SUBCLASS,
    .bDeviceProtocol = TUD_BT_PROTOCOL_PRIMARY_CONTROLLER,
#endif
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = USB_BCD,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&desc_device;
}

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          ENABLE_STANDBY_HID_KEYBOARD ? TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP : 0,
                          100),
    TUD_BTH_DESCRIPTOR(ITF_NUM_BTH, STR_BTH, EPNUM_BTH_EVENT, CFG_TUD_BTH_EVENT_EPSIZE, 1,
                       EPNUM_BTH_ACL_IN, EPNUM_BTH_ACL_OUT, CFG_TUD_BTH_DATA_EPSIZE, 9),
#if ENABLE_STANDBY_HID_KEYBOARD
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STR_HID, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(desc_hid_report), EPNUM_HID, CFG_TUD_HID_EP_BUFSIZE, 10),
#endif
#if ENABLE_CDC_DEBUG
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STR_CDC, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
#endif
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

#if ENABLE_STANDBY_HID_KEYBOARD
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}
#endif

static const char *const string_desc_arr[] = {
    [STR_MANUFACTURER] = "Codex",
    [STR_PRODUCT] = "Pico USB Bluetooth HCI Wake",
    [STR_SERIAL] = "0009",
    [STR_BTH] = "Bluetooth HCI",
#if ENABLE_STANDBY_HID_KEYBOARD
    [STR_HID] = "Standby Wake Keyboard",
#endif
#if ENABLE_CDC_DEBUG
    [STR_CDC] = "Debug",
#endif
};

static uint16_t desc_str[64];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    uint8_t chr_count;
    if (index == STR_LANGID) {
        desc_str[1] = 0x0409;
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) return NULL;
        const char *str = string_desc_arr[index];
        if (!str) return NULL;

        chr_count = 0;
        while (str[chr_count] && chr_count < 31) {
            desc_str[1 + chr_count] = str[chr_count];
            chr_count++;
        }
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8u) | (2u * chr_count + 2u));
    return desc_str;
}
