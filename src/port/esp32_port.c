// SPDX-License-Identifier: Apache-2.0
// ESP32 runtime for the portable core (see audio_player/esp32.h).
#ifdef ESP_PLATFORM

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "audio_player/esp32.h"
#include "audio_player/events.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio_player";

// ------------------------------------------------------------------ platform ----

static void *alloc_large(size_t size) {
    // PSRAM first when the board has it; internal RAM otherwise.
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}

static void *alloc_fast(size_t size) {
    void *p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

static void *realloc_large(void *ptr, size_t size) {
    return heap_caps_realloc_prefer(ptr, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}

static void free_any(void *ptr) { heap_caps_free(ptr); }

static void log_sink(core_log_level_t level, const char *tag, const char *msg) {
    switch (level) {
        case CORE_LOG_ERROR: ESP_LOGE(tag, "%s", msg); break;
        case CORE_LOG_WARN: ESP_LOGW(tag, "%s", msg); break;
        case CORE_LOG_INFO: ESP_LOGI(tag, "%s", msg); break;
        default: ESP_LOGD(tag, "%s", msg); break;
    }
}

static void *lock_create(void) { return xSemaphoreCreateRecursiveMutex(); }
static void lock_lock(void *m) { xSemaphoreTakeRecursive((SemaphoreHandle_t)m, portMAX_DELAY); }
static void lock_unlock(void *m) { xSemaphoreGiveRecursive((SemaphoreHandle_t)m); }
static void lock_destroy(void *m) { vSemaphoreDelete((SemaphoreHandle_t)m); }

static uint32_t clock_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void audio_player_port_init(core_log_level_t max_level) {
    static const core_allocator_t k_alloc = {alloc_large, alloc_fast, realloc_large, free_any};
    static const core_lock_ops_t k_locks = {lock_create, lock_lock, lock_unlock, lock_destroy};
    core_set_allocator(&k_alloc);
    core_set_lock_ops(&k_locks);
    core_set_clock(clock_ms);
    core_set_log_sink(log_sink, max_level);
}

// ---------------------------------------------------------------- audio task ----

typedef struct {
    player_t *player;
    TaskHandle_t task;
    SemaphoreHandle_t wake;
    SemaphoreHandle_t done;
    volatile bool quit;
} runner_t;

static runner_t *s_runner;  // one player per application

static void wake_cb(void *user) {
    runner_t *r = (runner_t *)user;
    xSemaphoreGive(r->wake);
}

static void audio_task(void *arg) {
    runner_t *r = (runner_t *)arg;
    while (!r->quit) {
        int32_t n = player_run_once(r->player);
        if (n > 0) continue;  // the sink write paces the loop
        uint32_t ms = player_wait_hint_ms(r->player);
        if (ms) xSemaphoreTake(r->wake, pdMS_TO_TICKS(ms) + 1);
    }
    xSemaphoreGive(r->done);
    vTaskDelete(NULL);
}

static void runner_free(runner_t *r) {
    if (!r) return;
    if (r->wake) vSemaphoreDelete(r->wake);
    if (r->done) vSemaphoreDelete(r->done);
    heap_caps_free(r);
}

player_t *audio_player_start(const audio_player_esp32_config_t *cfg) {
    if (!cfg) return NULL;
    if (s_runner) {
        ESP_LOGE(TAG, "a player is already running");
        return NULL;
    }
    audio_player_port_init(cfg->log_level);
    if (cfg->on_event) core_events_set_sink(cfg->on_event, cfg->on_event_user);

    runner_t *r = (runner_t *)heap_caps_calloc(1, sizeof *r, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!r) return NULL;
    r->wake = xSemaphoreCreateBinary();
    r->done = xSemaphoreCreateBinary();
    if (!r->wake || !r->done) {
        runner_free(r);
        return NULL;
    }

    player_config_t pc;
    memset(&pc, 0, sizeof pc);
    pc.sink = cfg->sink;
    pc.library = cfg->library;
    pc.state_dir = cfg->state_dir;
    pc.scrobble_log = cfg->scrobble_log;
    pc.volume_default_db = cfg->volume_default_db;
    pc.volume_max_db = cfg->volume_max_db;
    pc.volume_step_db = cfg->volume_step_db > 0.0f ? cfg->volume_step_db : 1.0f;
    pc.wake = wake_cb;
    pc.wake_user = r;
    r->player = player_create(&pc);
    if (!r->player) {
        ESP_LOGE(TAG, "player_create failed");
        runner_free(r);
        return NULL;
    }

    uint32_t stack = cfg->task_stack ? cfg->task_stack : 12288;
    UBaseType_t prio = cfg->task_priority ? cfg->task_priority : 18;
    BaseType_t core = tskNO_AFFINITY;
    if (cfg->task_core >= 0 && cfg->task_core < portNUM_PROCESSORS) core = cfg->task_core;
    if (xTaskCreatePinnedToCore(audio_task, "audio_player", stack, r, prio, &r->task, core) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the audio task");
        player_destroy(r->player);
        runner_free(r);
        return NULL;
    }
    s_runner = r;
    return r->player;
}

void audio_player_stop(player_t *p) {
    runner_t *r = s_runner;
    if (!r || r->player != p) return;
    r->quit = true;
    xSemaphoreGive(r->wake);
    xSemaphoreTake(r->done, portMAX_DELAY);
    player_destroy(r->player);
    s_runner = NULL;
    runner_free(r);
}

#endif  // ESP_PLATFORM
