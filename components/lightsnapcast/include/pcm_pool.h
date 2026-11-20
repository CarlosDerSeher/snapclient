#ifndef __PCM_POOL_H__
#define __PCM_POOL_H__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "snapcast.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Fixed pool of equal-size PCM chunk slots between the decoder (http task)
 * and the player task.
 *
 * The slots are allocated once per stream format and server buffer, so there
 * is no heap allocation from decoding to playback. Slots may live in IRAM:
 * access a payload only with aligned 32-bit reads and writes.
 *
 * Empty chunks wait in a free queue, filled ones in the play queue. The
 * decoder takes a free chunk, fills it and puts it into the play queue; the
 * player takes it from there and gives it back to the free queue.
 *
 * Build and destroy only from the decoder task, while the player is stopped.
 */

typedef struct pcmData {
  tv_t timestamp;
  uint32_t totalSize;  // bytes of PCM in payload
  char *payload;       // slot of pcm_pool_slot_bytes(), 32-bit access only
} pcm_chunk_message_t;

/**
 * Allocates slots chunks of slotBytes each. All or nothing: if not all of
 * them fit, nothing stays allocated and ESP_ERR_NO_MEM is returned.
 */
esp_err_t pcm_pool_build(uint32_t slotBytes, uint32_t slots);

/**
 * Frees all slots. Returns chunks waiting for playback to the free queue
 * first; fails if a chunk is still held by the decoder or the player.
 */
esp_err_t pcm_pool_destroy(void);

bool pcm_pool_ready(void);
uint32_t pcm_pool_slot_bytes(void);
uint32_t pcm_pool_slots(void);

// a free chunk, or NULL if there is none (buffer full) or no pool
pcm_chunk_message_t *pcm_pool_get_free(void);
void pcm_pool_put_free(pcm_chunk_message_t *chunk);

// queue a filled chunk for playback
void pcm_pool_put_filled(pcm_chunk_message_t *chunk);
// next chunk to play, or NULL after wait
pcm_chunk_message_t *pcm_pool_get_filled(TickType_t wait);

// moves all chunks waiting for playback back to the free queue
void pcm_pool_flush(void);

uint32_t pcm_pool_filled_count(void);

// diagnostics: slots in IRAM only and in DRAM (incl. D/IRAM, SPIRAM), and
// the fewest free slots seen since the pool was built
void pcm_pool_get_info(uint32_t *slots, uint32_t *slotBytes,
                       uint32_t *iramSlots, uint32_t *dramSlots,
                       uint32_t *minFreeSlots);

#ifdef __cplusplus
}
#endif

#endif  // __PCM_POOL_H__
