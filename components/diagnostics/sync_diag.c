/*
 * Sync diagnostics, see sync_diag.h.
 *
 * Per interval:
 *   lag     time from the server sending a message to its arrival here, in
 *           server time (needs the time sync); per message type. Includes the
 *           server's send queue, the network and TCP, and how late we read it.
 *   gap     longest time between two arriving messages
 *   time    time requests sent / replies / replies skipped as duplicates
 *           (several in one TCP segment) / timeouts; round trip of replies
 *   offset  current local -> server offset and how much it moved
 *   queue   fewest pcm chunks waiting for playback (sampled per wire chunk)
 *   free    decoder waits for a free pcm chunk: count, total time, failures
 *   resync  initial syncs, HARD 1, HARD 2
 *   wifi    RSSI now, disconnects and the last reason
 * Every server settings message is also logged right away with its lag.
 */
#include "sync_diag.h"

#if CONFIG_SNAPCLIENT_SYNC_DIAG

#include <inttypes.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "wifi_interface.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pcm_pool.h"
#include "player.h"
#include "snapcast.h"

static const char *TAG = "SYNCDIAG";

#define SYNC_DIAG_STACK_SIZE (3 * 1024)

typedef struct {
  uint32_t n;
  int64_t sum, min, max;
} stat_t;

typedef struct {
  stat_t lag[SNAPCAST_MESSAGE_LAST + 1];
  uint32_t unsynced;  // messages before the time sync had an offset
  int64_t gapMax;
  uint32_t timeSent, timeReplies, timeDup, timeTimeouts;
  stat_t rtt;
  uint32_t queueMin;
  uint32_t freeWaits, freeFails;
  int64_t freeWaitSum, freeWaitMax;
  uint32_t resyncInit, resyncHard1, resyncHard2;
  uint32_t wifiDisc;
  uint8_t wifiReason;
} sync_diag_stats_t;

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static sync_diag_stats_t st;
static int64_t lastRx = 0;

static void stat_reset(stat_t *s) {
  s->n = 0;
  s->sum = 0;
  s->min = INT64_MAX;
  s->max = INT64_MIN;
}

static void stat_add(stat_t *s, int64_t v) {
  s->n++;
  s->sum += v;
  if (v < s->min) s->min = v;
  if (v > s->max) s->max = v;
}

static void stats_reset(sync_diag_stats_t *s) {
  memset(s, 0, sizeof(*s));
  for (int i = 0; i <= SNAPCAST_MESSAGE_LAST; i++) stat_reset(&s->lag[i]);
  stat_reset(&s->rtt);
  s->queueMin = UINT32_MAX;
}

void sync_diag_msg(uint16_t type, int64_t sent_server_us, int64_t rx_local_us) {
  int64_t diff = 0;
  get_diff_to_server(&diff, rx_local_us);
  int64_t lag = rx_local_us + diff - sent_server_us;
  uint32_t queued = 0;
  if (type == SNAPCAST_MESSAGE_WIRE_CHUNK) {
    queued = pcm_pool_filled_count();
  }

  taskENTER_CRITICAL(&lock);
  if (lastRx > 0 && rx_local_us - lastRx > st.gapMax) {
    st.gapMax = rx_local_us - lastRx;
  }
  if (rx_local_us > lastRx) lastRx = rx_local_us;
  if (diff == 0) {
    st.unsynced++;
  } else if (type <= SNAPCAST_MESSAGE_LAST) {
    stat_add(&st.lag[type], lag);
  }
  if (type == SNAPCAST_MESSAGE_WIRE_CHUNK && queued < st.queueMin) {
    st.queueMin = queued;
  }
  taskEXIT_CRITICAL(&lock);

  if (type == SNAPCAST_MESSAGE_SERVER_SETTINGS) {
    if (diff == 0) {
      ESP_LOGI(TAG, "server settings arrived (no time sync yet)");
    } else {
      ESP_LOGI(TAG, "server settings arrived, lag %" PRId64 " ms",
               lag / 1000);
    }
  }
}

void sync_diag_time_sent(void) {
  taskENTER_CRITICAL(&lock);
  st.timeSent++;
  taskEXIT_CRITICAL(&lock);
}

void sync_diag_time_timeout(void) {
  taskENTER_CRITICAL(&lock);
  st.timeTimeouts++;
  taskEXIT_CRITICAL(&lock);
}

void sync_diag_time_reply(int64_t rtt_us, bool duplicate) {
  taskENTER_CRITICAL(&lock);
  st.timeReplies++;
  if (duplicate) {
    st.timeDup++;
  }
  stat_add(&st.rtt, rtt_us);
  taskEXIT_CRITICAL(&lock);
}

// player events (player_set_event_cb): resyncs, and the decoder's waits for a
// free pcm chunk
static void player_event_handler(player_event_t event, int64_t value) {
  if (event == PLAYER_EVENT_FREE_CHUNK_WAIT && value < 1000) {
    return;  // took a chunk without waiting
  }
  taskENTER_CRITICAL(&lock);
  switch (event) {
    case PLAYER_EVENT_RESYNC_INITIAL:
      st.resyncInit++;
      break;
    case PLAYER_EVENT_RESYNC_HARD1:
      st.resyncHard1++;
      break;
    case PLAYER_EVENT_RESYNC_HARD2:
      st.resyncHard2++;
      break;
    case PLAYER_EVENT_FREE_CHUNK_WAIT:
    case PLAYER_EVENT_FREE_CHUNK_FAIL:
      if (event == PLAYER_EVENT_FREE_CHUNK_FAIL) st.freeFails++;
      st.freeWaits++;
      st.freeWaitSum += value;
      if (value > st.freeWaitMax) st.freeWaitMax = value;
      break;
  }
  taskEXIT_CRITICAL(&lock);
}

static void wifi_disc_handler(void *arg, esp_event_base_t base, int32_t id,
                              void *data) {
  wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
  taskENTER_CRITICAL(&lock);
  st.wifiDisc++;
  st.wifiReason = ev->reason;
  taskEXIT_CRITICAL(&lock);
}

// "min/avg/max" in ms, or "-" without samples
static const char *fmt_ms(char *buf, size_t len, const stat_t *s) {
  if (s->n == 0) {
    snprintf(buf, len, "-");
  } else {
    snprintf(buf, len, "%" PRId64 "/%" PRId64 "/%" PRId64, s->min / 1000,
             s->sum / s->n / 1000, s->max / 1000);
  }
  return buf;
}

static void sync_diag_task(void *arg) {
  int64_t lastDiff = 0;
  TickType_t wake = xTaskGetTickCount();

  while (1) {
    vTaskDelayUntil(&wake,
                    pdMS_TO_TICKS(CONFIG_SNAPCLIENT_SYNC_DIAG_INTERVAL_S * 1000));

    sync_diag_stats_t s;
    taskENTER_CRITICAL(&lock);
    s = st;
    stats_reset(&st);
    taskEXIT_CRITICAL(&lock);

    int64_t diff = 0;
    get_diff_to_server(&diff, esp_timer_get_time());
    int8_t rssi = 0;
    wifi_ap_record_t ap;
    if (wifi_get_ap_record(&ap)) {
      rssi = ap.rssi;
    }

    char chunk[64], settings[64], time[64], rtt[64];
    ESP_LOGI(TAG,
             "lag ms chunk %s (n %" PRIu32 ") settings %s time %s, unsynced "
             "%" PRIu32 ", gap max %" PRId64 " ms",
             fmt_ms(chunk, sizeof(chunk), &s.lag[SNAPCAST_MESSAGE_WIRE_CHUNK]),
             s.lag[SNAPCAST_MESSAGE_WIRE_CHUNK].n,
             fmt_ms(settings, sizeof(settings),
                    &s.lag[SNAPCAST_MESSAGE_SERVER_SETTINGS]),
             fmt_ms(time, sizeof(time), &s.lag[SNAPCAST_MESSAGE_TIME]),
             s.unsynced, s.gapMax / 1000);
    ESP_LOGI(TAG,
             "time sent %" PRIu32 " replies %" PRIu32 " dup %" PRIu32
             " timeouts %" PRIu32 ", rtt ms %s, offset %" PRId64
             " us (moved %" PRId64 ")",
             s.timeSent, s.timeReplies, s.timeDup, s.timeTimeouts,
             fmt_ms(rtt, sizeof(rtt), &s.rtt), diff,
             lastDiff ? diff - lastDiff : 0);
    ESP_LOGI(TAG,
             "queue min %ld, free waits %" PRIu32 " (%" PRId64
             " ms, max %" PRId64 " ms) failed %" PRIu32
             ", resync init %" PRIu32 " hard1 %" PRIu32 " hard2 %" PRIu32
             ", wifi %d dBm, disconnects %" PRIu32 " (reason %u), fw %s",
             s.queueMin == UINT32_MAX ? -1L : (long)s.queueMin, s.freeWaits,
             s.freeWaitSum / 1000, s.freeWaitMax / 1000, s.freeFails,
             s.resyncInit, s.resyncHard1, s.resyncHard2, rssi, s.wifiDisc,
             (unsigned)s.wifiReason, esp_app_get_description()->version);
    lastDiff = diff;
  }
}

void sync_diag_init(void) {
  static StaticTask_t tcb;
  static StackType_t stack[SYNC_DIAG_STACK_SIZE / sizeof(StackType_t)];

  stats_reset(&st);
  esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                             wifi_disc_handler, NULL);
  player_set_event_cb(player_event_handler);
  xTaskCreateStaticPinnedToCore(sync_diag_task, "sync_diag",
                                SYNC_DIAG_STACK_SIZE / sizeof(StackType_t),
                                NULL, 1, stack, &tcb, tskNO_AFFINITY);
}

#endif  // CONFIG_SNAPCLIENT_SYNC_DIAG
