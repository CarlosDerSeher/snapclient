#include "pcm_pool.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

static const char *TAG = "PCMPOOL";

// Internal DRAM left free: DRAM slots are only taken while at least this much
// stays free. System headroom, not a buffer budget. It covers what is
// allocated after the pool: the player task and the I2S DMA buffers when
// playback starts (~12 KB), and Wi-Fi/lwIP buffers under load and httpd.
// Measured on an ESP32 with FLAC 44.1 kHz: free DRAM falls by ~27 KB from
// building the pool to the lowest point during playback.
#define PCM_POOL_DRAM_RESERVE (32 * 1024)

#define CAPS_IRAM (MALLOC_CAP_32BIT | MALLOC_CAP_EXEC)
#define CAPS_DRAM (MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL)

static pcm_chunk_message_t *chunks = NULL;  // descriptors, DRAM
static uint32_t chunkCnt = 0;
static uint32_t slotSize = 0;
static uint32_t iramCnt = 0;
static uint32_t dramCnt = 0;
static uint32_t minFree = 0;  // fewest free chunks seen, for diagnostics
static uint32_t dropped = 0;  // pcm_pool_get_free() calls that got no chunk
static QueueHandle_t freeQHdl = NULL;
static QueueHandle_t filledQHdl = NULL;

/**
 * One slot: SPIRAM if there is some, then IRAM, then internal DRAM down to
 * the reserve. *dram is set if the slot took byte-addressable internal memory.
 */
static char *slot_alloc(uint32_t bytes, bool *dram) {
  char *p = NULL;

  *dram = true;

#if CONFIG_SPIRAM
  p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p != NULL) {
    return p;
  }
#endif

  // On ESP32 IRAM capable memory includes D/IRAM, which is DRAM seen through
  // the instruction bus: compare the free DRAM before and after
  size_t dramBefore = heap_caps_get_free_size(CAPS_DRAM);
  p = heap_caps_malloc(bytes, CAPS_IRAM);
  if (p == NULL) {
    p = heap_caps_malloc(bytes, CAPS_DRAM);
  }
  if (p == NULL) {
    return NULL;
  }

  size_t dramAfter = heap_caps_get_free_size(CAPS_DRAM);
  *dram = dramAfter < dramBefore;
  if (*dram && (dramAfter < PCM_POOL_DRAM_RESERVE)) {
    heap_caps_free(p);
    p = NULL;
  }

  return p;
}

static void pool_free_all(void) {
  if (chunks != NULL) {
    for (uint32_t i = 0; i < chunkCnt; i++) {
      heap_caps_free(chunks[i].payload);
    }
    heap_caps_free(chunks);
    chunks = NULL;
  }
  if (freeQHdl != NULL) {
    vQueueDelete(freeQHdl);
    freeQHdl = NULL;
  }
  if (filledQHdl != NULL) {
    vQueueDelete(filledQHdl);
    filledQHdl = NULL;
  }
  chunkCnt = 0;
  slotSize = 0;
  iramCnt = 0;
  dramCnt = 0;
  minFree = 0;
  dropped = 0;
}

esp_err_t pcm_pool_build(uint32_t slotBytes, uint32_t slots) {
  if (chunks != NULL) {
    ESP_LOGE(TAG, "pool exists, destroy it first");
    return ESP_ERR_INVALID_STATE;
  }
  if ((slots == 0) || (slotBytes == 0) || (slotBytes % 4)) {
    ESP_LOGE(TAG, "invalid pool: %lu slots of %lu B", slots, slotBytes);
    return ESP_ERR_INVALID_ARG;
  }

  chunks = heap_caps_calloc(slots, sizeof(pcm_chunk_message_t), CAPS_DRAM);
  freeQHdl = xQueueCreate(slots, sizeof(pcm_chunk_message_t *));
  filledQHdl = xQueueCreate(slots, sizeof(pcm_chunk_message_t *));
  if ((chunks == NULL) || (freeQHdl == NULL) || (filledQHdl == NULL)) {
    ESP_LOGE(TAG, "no memory for %lu chunk descriptors", slots);
    pool_free_all();
    return ESP_ERR_NO_MEM;
  }

  // count first, so pool_free_all() frees what was allocated on failure
  chunkCnt = slots;
  slotSize = slotBytes;

  uint32_t got = 0;
  for (; got < slots; got++) {
    bool dram;
    char *p = slot_alloc(slotBytes, &dram);
    if (p == NULL) {
      break;
    }
    chunks[got].payload = p;
    if (dram) {
      dramCnt++;
    } else {
      iramCnt++;
    }
  }

  if (got < slots) {
    ESP_LOGE(TAG,
             "%lu chunks of %lu B (%lu KB) needed, only %lu fit (%lu in IRAM, "
             "%lu in DRAM, %u B DRAM kept free)",
             slots, slotBytes, slots * slotBytes / 1024, got, iramCnt, dramCnt,
             PCM_POOL_DRAM_RESERVE);
    pool_free_all();
    return ESP_ERR_NO_MEM;
  }

  for (uint32_t i = 0; i < slots; i++) {
    pcm_chunk_message_t *c = &chunks[i];
    xQueueSend(freeQHdl, &c, 0);
  }
  minFree = slots;

  ESP_LOGI(TAG, "%lu chunks of %lu B (%lu KB): %lu in IRAM, %lu in DRAM/SPIRAM",
           slots, slotBytes, slots * slotBytes / 1024, iramCnt, dramCnt);

  return ESP_OK;
}

esp_err_t pcm_pool_destroy(void) {
  if (chunks == NULL) {
    return ESP_OK;
  }

  pcm_pool_flush();

  UBaseType_t free = uxQueueMessagesWaiting(freeQHdl);
  if (free != chunkCnt) {
    // freeing now would pull memory from under the holder
    ESP_LOGE(TAG, "%lu of %lu chunks still in use, pool kept",
             chunkCnt - (uint32_t)free, chunkCnt);
    return ESP_ERR_INVALID_STATE;
  }

  pool_free_all();

  return ESP_OK;
}

bool pcm_pool_ready(void) { return chunks != NULL; }

uint32_t pcm_pool_slot_bytes(void) { return slotSize; }

uint32_t pcm_pool_slots(void) { return chunkCnt; }

pcm_chunk_message_t *pcm_pool_get_free(TickType_t wait) {
  pcm_chunk_message_t *c = NULL;

  if (freeQHdl == NULL) {
    return NULL;
  }
  if (xQueueReceive(freeQHdl, &c, 0) != pdTRUE) {
    minFree = 0;
    if ((wait == 0) || (xQueueReceive(freeQHdl, &c, wait) != pdTRUE)) {
      dropped++;
      return NULL;
    }
  }
  c->totalSize = 0;

  uint32_t left = uxQueueMessagesWaiting(freeQHdl);
  if (left < minFree) {
    minFree = left;
  }

  return c;
}

void pcm_pool_put_free(pcm_chunk_message_t *chunk) {
  if ((chunk == NULL) || (freeQHdl == NULL)) {
    return;
  }
  // never blocks: the queue holds all chunks
  xQueueSend(freeQHdl, &chunk, 0);
}

void pcm_pool_put_filled(pcm_chunk_message_t *chunk) {
  if ((chunk == NULL) || (filledQHdl == NULL)) {
    return;
  }
  xQueueSend(filledQHdl, &chunk, 0);
}

pcm_chunk_message_t *pcm_pool_get_filled(TickType_t wait) {
  pcm_chunk_message_t *c = NULL;

  if (filledQHdl == NULL) {
    vTaskDelay(wait);
    return NULL;
  }
  if (xQueueReceive(filledQHdl, &c, wait) != pdTRUE) {
    return NULL;
  }

  return c;
}

void pcm_pool_flush(void) {
  pcm_chunk_message_t *c;

  if (filledQHdl == NULL) {
    return;
  }
  while (xQueueReceive(filledQHdl, &c, 0) == pdTRUE) {
    pcm_pool_put_free(c);
  }
}

uint32_t pcm_pool_filled_count(void) {
  QueueHandle_t q = filledQHdl;

  return (q != NULL) ? uxQueueMessagesWaiting(q) : 0;
}

uint32_t pcm_pool_dropped(void) { return dropped; }

void pcm_pool_get_info(uint32_t *slots, uint32_t *slotBytes,
                       uint32_t *iramSlots, uint32_t *dramSlots,
                       uint32_t *minFreeSlots, uint32_t *droppedChunks) {
  *minFreeSlots = minFree;
  *droppedChunks = dropped;
  *slots = chunkCnt;
  *slotBytes = slotSize;
  *iramSlots = iramCnt;
  *dramSlots = dramCnt;
}
