#pragma once

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Persistent worker task with an internal-SRAM stack. littlefs partition IO is
// delegated here when the calling task's stack is in PSRAM, because the
// SPI-flash cache-disable that backs esp_partition_* faults a PSRAM stack on
// the ESP32-S3 (esp_task_stack_is_sane_cache_disabled assertion). The caller
// blocks until the worker completes the op.
//
// Start() is idempotent; call it once after the littlefs mount succeeds.
void littlefs_flash_worker_start(void);

// True once the worker task is running.
bool littlefs_flash_worker_started(void);

// Run fn(ctx) on the worker task; block the caller and return fn's result.
esp_err_t littlefs_flash_worker_run(esp_err_t (*fn)(void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
