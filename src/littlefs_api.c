/**
 * @file littlefs_api.c
 * @brief Maps the HAL of esp_partition <-> littlefs
 * @author Brian Pugh
 */

#define ESP_LOCAL_LOG_LEVEL ESP_LOG_INFO

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs.h"
#include "littlefs/lfs.h"
#include "esp_littlefs.h"
#include "littlefs_api.h"
#include "littlefs_flash_worker.h"
#include "esp_memory_utils.h"
#include "esp_cpu.h"

static const char TAG[] = "esp_littlefs_api";

typedef enum {
    LFS_PART_READ,
    LFS_PART_PROG,
    LFS_PART_ERASE,
} lfs_part_op_kind_t;

typedef struct {
    lfs_part_op_kind_t kind;
    const esp_partition_t *partition;
    size_t offset;
    void *read_buf;        // LFS_PART_READ
    const void *write_buf; // LFS_PART_PROG
    size_t size;
} lfs_part_op_t;

static esp_err_t run_part_op(void *ctx) {
    lfs_part_op_t *op = (lfs_part_op_t *)ctx;
    switch (op->kind) {
        case LFS_PART_READ:
            return esp_partition_read(op->partition, op->offset, op->read_buf, op->size);
        case LFS_PART_PROG:
            return esp_partition_write(op->partition, op->offset, op->write_buf, op->size);
        case LFS_PART_ERASE:
            return esp_partition_erase_range(op->partition, op->offset, op->size);
    }
    return ESP_FAIL;
}

// esp_partition_* disables the SPI-flash cache, which also gates PSRAM on the
// ESP32-S3. A task whose stack is in PSRAM cannot survive that, so when the
// current stack is in PSRAM we run the op on the internal-stack flash worker.
// Otherwise (internal-stack callers, or before the worker has started during
// the early-boot mount) run it directly: no overhead, and safe.
static esp_err_t exec_part_op(lfs_part_op_t *op) {
    if (littlefs_flash_worker_started() && !esp_ptr_in_dram((void *)esp_cpu_get_sp())) {
        return littlefs_flash_worker_run(run_part_op, op);
    }
    return run_part_op(op);
}

int littlefs_api_read(const struct lfs_config *c, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size) {
    esp_littlefs_t * efs = c->context;
    size_t part_off = (block * c->block_size) + off;
    lfs_part_op_t op = {
        .kind = LFS_PART_READ, .partition = efs->partition, .offset = part_off,
        .read_buf = buffer, .write_buf = NULL, .size = size,
    };
    esp_err_t err = exec_part_op(&op);
    if (err) {
        ESP_LOGE(TAG, "failed to read addr %08x, size %08lx, err %d", part_off, size, err);
        return LFS_ERR_IO;
    }
    return 0;
}

int littlefs_api_prog(const struct lfs_config *c, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    esp_littlefs_t * efs = c->context;
    size_t part_off = (block * c->block_size) + off;
    lfs_part_op_t op = {
        .kind = LFS_PART_PROG, .partition = efs->partition, .offset = part_off,
        .read_buf = NULL, .write_buf = buffer, .size = size,
    };
    esp_err_t err = exec_part_op(&op);
    if (err) {
        ESP_LOGE(TAG, "failed to write addr %08x, size %08lx, err %d", part_off, size, err);
        return LFS_ERR_IO;
    }
    return 0;
}

int littlefs_api_erase(const struct lfs_config *c, lfs_block_t block) {
    esp_littlefs_t * efs = c->context;
    size_t part_off = block * c->block_size;
    lfs_part_op_t op = {
        .kind = LFS_PART_ERASE, .partition = efs->partition, .offset = part_off,
        .read_buf = NULL, .write_buf = NULL, .size = c->block_size,
    };
    esp_err_t err = exec_part_op(&op);
    if (err) {
        ESP_LOGE(TAG, "failed to erase addr %08x, size %08lx, err %d", part_off, c->block_size, err);
        return LFS_ERR_IO;
    }
    return 0;
}

int littlefs_api_sync(const struct lfs_config *c) {
    /* Unnecessary for esp-idf */
    return 0;
}

