#include "littlefs_flash_worker.h"

#include <atomic>
#include <cstdio>

extern "C" void kc_lfs_flash_worker_test_shutdown();

namespace {
int g_failures = 0;
}

#define EXPECT(cond)                                              \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

static std::atomic<int> g_ran{0};

static esp_err_t incrJob(void *) {
  g_ran.fetch_add(1);
  return ESP_OK;
}

static esp_err_t errJob(void *) { return ESP_ERR_NOT_FOUND; }

static void testSingleJobRoundTrip() {
  g_ran.store(0);
  littlefs_flash_worker_start();
  EXPECT(littlefs_flash_worker_started());
  esp_err_t result = littlefs_flash_worker_run(incrJob, nullptr);
  EXPECT(g_ran.load() == 1);
  EXPECT(result == ESP_OK);
  kc_lfs_flash_worker_test_shutdown();
}

static void testErrorPropagates() {
  littlefs_flash_worker_start();
  esp_err_t result = littlefs_flash_worker_run(errJob, nullptr);
  EXPECT(result == ESP_ERR_NOT_FOUND);
  kc_lfs_flash_worker_test_shutdown();
}

static void testManySerialized() {
  g_ran.store(0);
  littlefs_flash_worker_start();
  for (int i = 0; i < 100; ++i) {
    EXPECT(littlefs_flash_worker_run(incrJob, nullptr) == ESP_OK);
  }
  EXPECT(g_ran.load() == 100);
  kc_lfs_flash_worker_test_shutdown();
}

int main() {
  testSingleJobRoundTrip();
  testErrorPropagates();
  testManySerialized();
  if (g_failures == 0) {
    std::printf("OK: all littlefs_flash_worker tests passed\n");
    return 0;
  }
  std::printf("FAILED: %d checks\n", g_failures);
  return 1;
}
