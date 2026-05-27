#include "littlefs_flash_worker.h"

#ifdef KC_LFS_FLASH_WORKER_TEST_HOOKS

// Host build: std::thread/mutex/condvar replace FreeRTOS so the worker can be
// unit-tested without ESP-IDF.
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

namespace {

struct HostJob {
  esp_err_t (*fn)(void *ctx);
  void *ctx;
  esp_err_t result = ESP_OK;
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
};

std::mutex g_queue_mu;
std::condition_variable g_queue_cv;
std::deque<HostJob *> g_queue;
std::thread g_worker;
bool g_started  = false;
bool g_shutdown = false;

void workerLoop() {
  while (true) {
    HostJob *job = nullptr;
    {
      std::unique_lock<std::mutex> lk(g_queue_mu);
      g_queue_cv.wait(lk, [] { return !g_queue.empty() || g_shutdown; });
      if (g_shutdown && g_queue.empty()) {
        return;
      }
      job = g_queue.front();
      g_queue.pop_front();
    }
    job->result = job->fn(job->ctx);
    {
      std::lock_guard<std::mutex> lk(job->mu);
      job->done = true;
    }
    job->cv.notify_one();
  }
}

} // namespace

extern "C" void littlefs_flash_worker_start(void) {
  if (g_started) {
    return;
  }
  g_started = true;
  g_worker  = std::thread(workerLoop);
}

extern "C" bool littlefs_flash_worker_started(void) { return g_started; }

extern "C" esp_err_t littlefs_flash_worker_run(esp_err_t (*fn)(void *ctx), void *ctx) {
  if (!g_started) {
    std::fprintf(stderr, "littlefs_flash_worker_run called before start\n");
    std::abort();
  }
  // Self-dispatch would deadlock: the worker would block waiting for itself.
  if (std::this_thread::get_id() == g_worker.get_id()) {
    std::fprintf(stderr, "littlefs_flash_worker_run called from the worker thread — would deadlock\n");
    std::abort();
  }
  HostJob job;
  job.fn  = fn;
  job.ctx = ctx;
  {
    std::lock_guard<std::mutex> lk(g_queue_mu);
    g_queue.push_back(&job);
  }
  g_queue_cv.notify_one();
  std::unique_lock<std::mutex> lk(job.mu);
  job.cv.wait(lk, [&] { return job.done; });
  return job.result;
}

extern "C" void kc_lfs_flash_worker_test_shutdown() {
  {
    std::lock_guard<std::mutex> lk(g_queue_mu);
    g_shutdown = true;
  }
  g_queue_cv.notify_all();
  if (g_worker.joinable()) {
    g_worker.join();
  }
  g_started  = false;
  g_shutdown = false;
}

#else // IDF on-target build

#include "sdkconfig.h"

#include <atomic>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {

const char *TAG = "lfs_flash_worker";

struct IdfJob {
  esp_err_t (*fn)(void *ctx);
  void *ctx;
  esp_err_t result;
  SemaphoreHandle_t done;
  StaticSemaphore_t done_storage;
};

constexpr UBaseType_t kQueueDepth = 4;
// littlefs read/prog/erase callbacks only call esp_partition_*; a few hundred
// bytes of stack. 3 KiB leaves comfortable headroom on an internal-SRAM stack.
constexpr uint32_t kStackBytes = 3072;
// Must be >= the priority of the highest-priority task that performs littlefs
// IO (e.g. an audio task). The worker does brief, blocking work, so a high
// priority can't starve the system; but a lower priority would let a
// mid-priority task preempt the worker while a higher-priority caller is
// blocked waiting on it (priority inversion).
constexpr UBaseType_t kWorkerPrio = CONFIG_LITTLEFS_FLASH_WORKER_PRIORITY;

QueueHandle_t g_queue       = nullptr;
TaskHandle_t g_task         = nullptr;
std::atomic<bool> g_started{false};

void workerTask(void *) {
  IdfJob *job = nullptr;
  while (xQueueReceive(g_queue, &job, portMAX_DELAY) == pdTRUE) {
    job->result = job->fn(job->ctx);
    xSemaphoreGive(job->done);
  }
}

} // namespace

extern "C" void littlefs_flash_worker_start(void) {
  if (g_started) {
    return;
  }
  g_queue = xQueueCreate(kQueueDepth, sizeof(IdfJob *));
  configASSERT(g_queue != nullptr);
  BaseType_t ok = xTaskCreate(workerTask, "lfs_flash", kStackBytes, nullptr, kWorkerPrio, &g_task);
  configASSERT(ok == pdPASS);
  g_started = true;
  ESP_LOGI(TAG, "started (stack=%lu, prio=%u, queue=%u)", (unsigned long)kStackBytes,
           (unsigned)kWorkerPrio, (unsigned)kQueueDepth);
}

extern "C" bool littlefs_flash_worker_started(void) { return g_started; }

extern "C" esp_err_t littlefs_flash_worker_run(esp_err_t (*fn)(void *ctx), void *ctx) {
  configASSERT(g_started);
  // The caller decides to delegate only when its stack is in PSRAM; the worker
  // task's own stack is internal SRAM, so it never reaches this path for
  // itself. Guard against accidental self-dispatch (would deadlock) anyway.
  configASSERT(xTaskGetCurrentTaskHandle() != g_task);
  // The completion semaphore is backed by storage inside the job, which lives
  // on the (blocked) caller's stack — no per-op heap allocation, so no OOM path.
  IdfJob job{fn, ctx, ESP_OK, nullptr, {}};
  job.done = xSemaphoreCreateBinaryStatic(&job.done_storage);
  IdfJob *p     = &job;
  BaseType_t ok = xQueueSend(g_queue, &p, portMAX_DELAY);
  configASSERT(ok == pdPASS);
  xSemaphoreTake(job.done, portMAX_DELAY);
  vSemaphoreDelete(job.done);
  return job.result;
}

#endif // KC_LFS_FLASH_WORKER_TEST_HOOKS
