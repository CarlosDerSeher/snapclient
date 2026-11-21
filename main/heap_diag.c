#include "heap_diag.h"

#include "sdkconfig.h"

#if CONFIG_SNAPCLIENT_HEAP_DIAG

#include <stdint.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "HEAPDIAG";

#define HEAP_DIAG_STACK_SIZE (3 * 1024)
#define HEAP_DIAG_MAX_TASKS 32
#define HEAP_DIAG_PLAY_SHORT_MS (10 * 1000)
#define HEAP_DIAG_PLAY_LONG_MS (CONFIG_SNAPCLIENT_HEAP_DIAG_PLAY_LONG_S * 1000)

#define CAPS_IRAM (MALLOC_CAP_32BIT | MALLOC_CAP_EXEC)

typedef enum {
  DIAG_MSG_MARK = 0,
  DIAG_MSG_PLAY_START,
  DIAG_MSG_PLAY_STOP,
} diag_msg_type_t;

typedef struct {
  diag_msg_type_t type;
  const char *label;
} diag_msg_t;

// everything static so the diagnostics do not disturb the heap they measure
static StaticTask_t diagTaskBuffer;
static StackType_t diagTaskStack[HEAP_DIAG_STACK_SIZE];
static StaticQueue_t diagQueueBuffer;
static uint8_t diagQueueStorage[8 * sizeof(diag_msg_t)];
static QueueHandle_t diagQHdl = NULL;
static TaskStatus_t taskStatus[HEAP_DIAG_MAX_TASKS];

static void heap_diag_post(diag_msg_type_t type, const char *label) {
  if (diagQHdl == NULL) {
    return;
  }

  diag_msg_t msg = {.type = type, .label = label};
  xQueueSend(diagQHdl, &msg, 0);
}

static void heap_diag_dump(const char *label) {
  int64_t uptime_s = esp_timer_get_time() / 1000000;

  ESP_LOGI(TAG, "==== %s (uptime %lld s) ====", label, uptime_s);
  ESP_LOGI(TAG, "8BIT free %u largest %u min %u",
           heap_caps_get_free_size(MALLOC_CAP_8BIT),
           heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
  // on ESP32 32BIT|EXEC also matches the D/IRAM regions (which are 8BIT too),
  // so additionally report the 32-bit-only part, which is pure IRAM
  ESP_LOGI(TAG, "32BIT|EXEC free %u largest %u min %u",
           heap_caps_get_free_size(CAPS_IRAM),
           heap_caps_get_largest_free_block(CAPS_IRAM),
           heap_caps_get_minimum_free_size(CAPS_IRAM));
  ESP_LOGI(TAG, "IRAM-only (32BIT minus 8BIT) free %u",
           heap_caps_get_free_size(MALLOC_CAP_32BIT) -
               heap_caps_get_free_size(MALLOC_CAP_8BIT));

  UBaseType_t n =
      uxTaskGetSystemState(taskStatus, HEAP_DIAG_MAX_TASKS, NULL);
  for (UBaseType_t i = 0; i < n; i++) {
    // ESP-IDF stack sizes and high-water marks are in bytes
    ESP_LOGI(TAG, "stack %-16s hwm %5lu prio %2u",
             taskStatus[i].pcTaskName,
             (unsigned long)taskStatus[i].usStackHighWaterMark,
             (unsigned)taskStatus[i].uxCurrentPriority);
  }
  ESP_LOGI(TAG, "==== end %s ====", label);
}

static void heap_diag_task(void *pvParameters) {
  const TickType_t interval =
      pdMS_TO_TICKS(CONFIG_SNAPCLIENT_HEAP_DIAG_INTERVAL_S * 1000);
  TickType_t nextPeriodic = xTaskGetTickCount() + interval;
  TickType_t playStart = 0;
  bool playing = false;
  bool shortDone = true;
  bool longDone = true;
  diag_msg_t msg;

  heap_diag_dump("boot");

  while (1) {
    TickType_t now = xTaskGetTickCount();
    TickType_t deadline = nextPeriodic;

    if (playing && !shortDone) {
      TickType_t t = playStart + pdMS_TO_TICKS(HEAP_DIAG_PLAY_SHORT_MS);
      if ((int32_t)(t - deadline) < 0) {
        deadline = t;
      }
    } else if (playing && !longDone) {
      TickType_t t = playStart + pdMS_TO_TICKS(HEAP_DIAG_PLAY_LONG_MS);
      if ((int32_t)(t - deadline) < 0) {
        deadline = t;
      }
    }

    TickType_t wait = 0;
    if ((int32_t)(deadline - now) > 0) {
      wait = deadline - now;
    }

    if (xQueueReceive(diagQHdl, &msg, wait) == pdTRUE) {
      switch (msg.type) {
        case DIAG_MSG_MARK:
          heap_diag_dump(msg.label);
          break;

        case DIAG_MSG_PLAY_START:
          playing = true;
          playStart = xTaskGetTickCount();
          shortDone = false;
          longDone = false;
          heap_diag_dump("playback started");
          break;

        case DIAG_MSG_PLAY_STOP:
          playing = false;
          heap_diag_dump("playback stopped");
          break;
      }
      continue;
    }

    now = xTaskGetTickCount();
    if (playing && !shortDone &&
        (int32_t)(now - (playStart + pdMS_TO_TICKS(HEAP_DIAG_PLAY_SHORT_MS))) >= 0) {
      shortDone = true;
      heap_diag_dump("playback +10 s");
    } else if (playing && !longDone &&
               (int32_t)(now - (playStart + pdMS_TO_TICKS(HEAP_DIAG_PLAY_LONG_MS))) >= 0) {
      longDone = true;
      heap_diag_dump("playback long");
    }

    if ((int32_t)(now - nextPeriodic) >= 0) {
      nextPeriodic = now + interval;
      heap_diag_dump("periodic");
    }
  }
}

static void heap_diag_got_ip_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data) {
  heap_diag_post(DIAG_MSG_MARK, (const char *)arg);
}

void heap_diag_init(void) {
  if (diagQHdl != NULL) {
    return;
  }

  diagQHdl = xQueueCreateStatic(8, sizeof(diag_msg_t), diagQueueStorage,
                                &diagQueueBuffer);

  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                             heap_diag_got_ip_handler, (void *)"wifi got ip");
  esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                             heap_diag_got_ip_handler, (void *)"eth got ip");

  xTaskCreateStaticPinnedToCore(heap_diag_task, "heap_diag",
                                HEAP_DIAG_STACK_SIZE, NULL, 1, diagTaskStack,
                                &diagTaskBuffer, tskNO_AFFINITY);
}

void heap_diag_mark(const char *label) {
  heap_diag_post(DIAG_MSG_MARK, label);
}

void heap_diag_set_playing(bool playing) {
  heap_diag_post(playing ? DIAG_MSG_PLAY_START : DIAG_MSG_PLAY_STOP, NULL);
}

#else  // CONFIG_SNAPCLIENT_HEAP_DIAG

void heap_diag_init(void) {}
void heap_diag_mark(const char *label) {}
void heap_diag_set_playing(bool playing) {}

#endif  // CONFIG_SNAPCLIENT_HEAP_DIAG
