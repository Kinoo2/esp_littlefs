#pragma once

// Mock esp_err.h for host-compile tests (no ESP-IDF required). Keeps the
// littlefs_flash_worker host test self-contained within this submodule.

typedef int esp_err_t;

#define ESP_OK            0
#define ESP_FAIL          -1
#define ESP_ERR_NOT_FOUND 0x105
