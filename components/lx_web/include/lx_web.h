/*
 * Wi-Fi file manager: a web page for the SD card, on port 80. It stays off until
 * started, and every request except the page needs a PIN made at each start.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t lx_web_start(void);
void lx_web_stop(void);
bool lx_web_running(void);
const char *lx_web_pin(void);
uint32_t lx_web_changes(void);             /* counts uploads, new folders and deletes */
