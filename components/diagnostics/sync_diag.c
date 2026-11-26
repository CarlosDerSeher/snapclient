/*
 * Sync diagnostics, see sync_diag.h.
 *
 * Per interval:
 *   lag     time from the server sending a message to its arrival here, in
 *           server time (needs the time sync); per message type. Includes the
 *           server's send queue, the network and TCP, and how late we read it.
 *   gap     longest time between two arriving messages
 *   rx wait waits of the receiving task for TCP data while it was ready to
 *           take it (so not our own backpressure): count and longest of
 *           those >= RX_WAIT_US; and of those after which several
 *           segments arrived at once (a lost segment was retransmitted)
 *   time    time requests sent / replies / replies skipped as duplicates
 *           (several in one TCP segment) / timeouts; round trip of replies
 *   offset  current local -> server offset and how much it moved
 *   queue   fewest pcm chunks waiting for playback (sampled per wire chunk)
 *   free    decoder waits for a free pcm chunk: count, total time, failures
 *   resync  initial syncs, HARD 1, HARD 2, wire chunks skipped as too late
 *   wifi    RSSI now, disconnects and the last reason
 * Every server settings message is also logged right away with its lag.
 * Every wait for TCP data of RX_STALL_US or more is logged once afterwards as
 * an rx stall line: length, what arrived then, the round trips of the last
 * time replies before it and the RSSI per second around it.
 * The first time samples after the time filter starts empty (connect,
 * reconnect) are logged as one block: arrival, round trip, measured offset
 * and the filter's estimate, relative to the first sample.
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

// 3 KB had 48 B left on an ESP32-S3 after an hour (#261)
#define SYNC_DIAG_STACK_SIZE (4 * 1024)

typedef struct {
  uint32_t n;
  int64_t sum, min, max;
} stat_t;

typedef struct {
  stat_t lag[SNAPCAST_MESSAGE_LAST + 1];
  uint32_t unsynced;  // messages before the time sync had an offset
  int64_t gapMax;
  uint32_t rxWaits, rxHoles;
  int64_t rxWaitMax, rxHoleMax;
  uint32_t timeSent, timeReplies, timeDup, timeTimeouts;
  stat_t rtt;
  uint32_t queueMin;
  uint32_t freeWaits, freeFails;
  int64_t freeWaitSum, freeWaitMax;
  uint32_t resyncInit, resyncHard1, resyncHard2, lateSkips;
  uint32_t wifiDisc;
  uint8_t wifiReason;
} sync_diag_stats_t;

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static sync_diag_stats_t st;
static int64_t lastRx = 0;

// waits for TCP data counted in the summary, and logged one by one
#define RX_WAIT_US 100000
#define RX_STALL_US 200000
// a wait at least this long before a merged delivery counts as loss
#define RX_HOLE_US 20000

// rx stalls waiting to be logged by the task
#define STALLS_QUEUED 4
typedef struct {
  int64_t start_us, wait_us;
  uint32_t len;
  bool merged;
} rx_stall_t;
static rx_stall_t stallBuf[STALLS_QUEUED];
static uint32_t stallCount = 0, stallLost = 0;

// round trips of the last time replies, for the rx stall lines
#define RTT_RING_LEN 8
static struct {
  int64_t t_us;
  int32_t rtt_us;
} rttRing[RTT_RING_LEN];
static uint32_t rttHead = 0;  // total recorded

// RSSI sampled by the task once per second (0 when not on Wi-Fi)
#define RSSI_RING_LEN 16
static int8_t rssiRing[RSSI_RING_LEN];
static int64_t rssiT[RSSI_RING_LEN];
static uint32_t rssiHead = 0;

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
  rttRing[rttHead % RTT_RING_LEN].t_us = esp_timer_get_time();
  rttRing[rttHead % RTT_RING_LEN].rtt_us = (int32_t)rtt_us;
  rttHead++;
  taskEXIT_CRITICAL(&lock);
}

void sync_diag_rx(int64_t rx_local_us, int64_t wait_us, uint32_t len,
                  bool merged) {
  if (wait_us < RX_HOLE_US) {
    return;
  }
  bool hole = merged;
  taskENTER_CRITICAL(&lock);
  if (wait_us >= RX_WAIT_US) {
    st.rxWaits++;
    if (wait_us > st.rxWaitMax) st.rxWaitMax = wait_us;
  }
  if (hole) {
    st.rxHoles++;
    if (wait_us > st.rxHoleMax) st.rxHoleMax = wait_us;
  }
  if (wait_us >= RX_STALL_US) {
    if (stallCount < STALLS_QUEUED) {
      stallBuf[stallCount++] = (rx_stall_t){.start_us = rx_local_us - wait_us,
                                            .wait_us = wait_us,
                                            .len = len,
                                            .merged = merged};
    } else {
      stallLost++;
    }
  }
  taskEXIT_CRITICAL(&lock);
}

// one line per rx stall: what arrived after it, the round trips before it
// and the RSSI from 2 s before it until now
static void rx_stalls_log(void) {
  // static: only this task calls it, and its stack is small
  static rx_stall_t stalls[STALLS_QUEUED];
  static typeof(rttRing) rtts;
  static int8_t rssi[RSSI_RING_LEN];
  static int64_t rssiTs[RSSI_RING_LEN];
  static char rttStr[96], rssiStr[96];
  uint32_t n, lost, rh, sh;

  taskENTER_CRITICAL(&lock);
  n = stallCount;
  lost = stallLost;
  memcpy(stalls, stallBuf, sizeof(stalls));
  stallCount = stallLost = 0;
  memcpy(rtts, rttRing, sizeof(rtts));
  rh = rttHead;
  taskEXIT_CRITICAL(&lock);
  // the RSSI ring is only written by this task
  memcpy(rssi, rssiRing, sizeof(rssi));
  memcpy(rssiTs, rssiT, sizeof(rssiTs));
  sh = rssiHead;

  for (uint32_t i = 0; i < n; i++) {
    const rx_stall_t *x = &stalls[i];
    size_t pos = 0;
    rttStr[0] = rssiStr[0] = '\0';
    // newest first, only replies that arrived before the stall
    for (uint32_t k = 0, shown = 0; k < RTT_RING_LEN && k < rh && shown < 6;
         k++) {
      uint32_t j = (rh - 1 - k) % RTT_RING_LEN;
      if (rtts[j].t_us > x->start_us) continue;
      pos += snprintf(rttStr + pos, sizeof(rttStr) - pos, "%s%" PRId32 "@-%" PRId64 ".%" PRId64,
                      shown ? " " : "", rtts[j].rtt_us / 1000,
                      (x->start_us - rtts[j].t_us) / 1000000,
                      ((x->start_us - rtts[j].t_us) / 100000) % 10);
      shown++;
      if (pos >= sizeof(rttStr)) break;
    }
    pos = 0;
    // oldest first, from 2 s before the stall
    for (uint32_t k = (sh > RSSI_RING_LEN) ? sh - RSSI_RING_LEN : 0; k < sh;
         k++) {
      uint32_t j = k % RSSI_RING_LEN;
      if (rssiTs[j] < x->start_us - 2000000) continue;
      pos += snprintf(rssiStr + pos, sizeof(rssiStr) - pos, "%s%d",
                      pos ? " " : "", rssi[j]);
      if (pos >= sizeof(rssiStr)) break;
    }
    ESP_LOGW(TAG,
             "rx stall %" PRId64 " ms at %" PRId64 ".%03" PRId64
             " s, then %" PRIu32 " B%s; rtt ms@s before: %s; rssi/s: %s",
             x->wait_us / 1000, x->start_us / 1000000,
             (x->start_us / 1000) % 1000, x->len,
             x->merged ? " in one go (lost segment retransmitted)" : "",
             rttStr[0] ? rttStr : "-", rssiStr[0] ? rssiStr : "-");
  }
  if (lost > 0) {
    ESP_LOGW(TAG, "rx stall: %" PRIu32 " more not logged", lost);
  }
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
    case PLAYER_EVENT_LATE_SKIP:
      st.lateSkips++;
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
// time samples from an empty filter on, logged by the task when complete
#define TIME_SAMPLES_LOGGED 48

typedef struct {
  int32_t t_ms;     // arrival, since the first sample
  int32_t rtt_us;
  int32_t sample_us, est_us;  // relative to the first sample
  bool full;
} time_sample_t;

static time_sample_t tsBuf[TIME_SAMPLES_LOGGED];
static uint32_t tsCount = 0;  // recorded; TIME_SAMPLES_LOGGED + 1 = logged
static int64_t tsT0, tsRef;
static bool tsWasFull = true;

void sync_diag_time_sample(int64_t rx_local_us, int64_t rtt_us,
                           int64_t sample_us, int64_t est_us, bool full) {
  taskENTER_CRITICAL(&lock);
  if (!full && tsWasFull) {
    tsCount = 0;  // the filter was reset: record from here
    tsT0 = rx_local_us;
    tsRef = sample_us;
  }
  tsWasFull = full;
  if (tsCount < TIME_SAMPLES_LOGGED) {
    tsBuf[tsCount++] = (time_sample_t){
        .t_ms = (int32_t)((rx_local_us - tsT0) / 1000),
        .rtt_us = (int32_t)rtt_us,
        .sample_us = (int32_t)(sample_us - tsRef),
        .est_us = (int32_t)(est_us - tsRef),
        .full = full,
    };
  }
  taskEXIT_CRITICAL(&lock);
}

static void time_samples_log(void) {
  static time_sample_t buf[TIME_SAMPLES_LOGGED];
  taskENTER_CRITICAL(&lock);
  bool due = (tsCount == TIME_SAMPLES_LOGGED);
  if (due) {
    memcpy(buf, tsBuf, sizeof(buf));
    tsCount++;  // logged
  }
  taskEXIT_CRITICAL(&lock);
  if (!due) {
    return;
  }
  ESP_LOGI(TAG, "time samples since the filter was empty (us, relative to "
                "the first sample): t ms, rtt, sample, estimate");
  for (int i = 0; i < TIME_SAMPLES_LOGGED; i++) {
    ESP_LOGI(TAG, "ts %5" PRId32 " %7" PRId32 " %8" PRId32 " %8" PRId32 "%s",
             buf[i].t_ms, buf[i].rtt_us, buf[i].sample_us, buf[i].est_us,
             buf[i].full ? "" : " filling");
  }
}

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
  uint32_t tick = 0;

  while (1) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000));

    wifi_ap_record_t apNow;
    rssiRing[rssiHead % RSSI_RING_LEN] =
        wifi_get_ap_record(&apNow) ? apNow.rssi : 0;
    rssiT[rssiHead % RSSI_RING_LEN] = esp_timer_get_time();
    rssiHead++;
    rx_stalls_log();
    if (++tick < CONFIG_SNAPCLIENT_SYNC_DIAG_INTERVAL_S) {
      continue;
    }
    tick = 0;

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
             "%" PRIu32 ", gap max %" PRId64 " ms, rx waits %" PRIu32
             " (max %" PRId64 " ms) after loss %" PRIu32 " (max %" PRId64
             " ms)",
             fmt_ms(chunk, sizeof(chunk), &s.lag[SNAPCAST_MESSAGE_WIRE_CHUNK]),
             s.lag[SNAPCAST_MESSAGE_WIRE_CHUNK].n,
             fmt_ms(settings, sizeof(settings),
                    &s.lag[SNAPCAST_MESSAGE_SERVER_SETTINGS]),
             fmt_ms(time, sizeof(time), &s.lag[SNAPCAST_MESSAGE_TIME]),
             s.unsynced, s.gapMax / 1000, s.rxWaits, s.rxWaitMax / 1000,
             s.rxHoles, s.rxHoleMax / 1000);
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
             " late %" PRIu32
             ", wifi %d dBm, disconnects %" PRIu32 " (reason %u), fw %s",
             s.queueMin == UINT32_MAX ? -1L : (long)s.queueMin, s.freeWaits,
             s.freeWaitSum / 1000, s.freeWaitMax / 1000, s.freeFails,
             s.resyncInit, s.resyncHard1, s.resyncHard2, s.lateSkips, rssi,
             s.wifiDisc,
             (unsigned)s.wifiReason, esp_app_get_description()->version);
    lastDiff = diff;
    time_samples_log();
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
