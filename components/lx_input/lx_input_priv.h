#pragma once

#include "esp_err.h"

esp_err_t lx_input_usb_start(void);
esp_err_t lx_input_xinput_start(void);     /* after usb_start: the USB host is installed */
