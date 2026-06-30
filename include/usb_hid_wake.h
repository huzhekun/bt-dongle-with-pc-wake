#pragma once

#include <stdbool.h>

void usb_hid_wake_init(void);
void usb_hid_wake_set_standby(bool standby);
bool usb_hid_wake_keep_usb_connected(void);
void usb_hid_wake_request_keypress(void);
void usb_hid_wake_task(void);
