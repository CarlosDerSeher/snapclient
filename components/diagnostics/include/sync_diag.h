/*
 * Sync diagnostics (CONFIG_SNAPCLIENT_SYNC_DIAG): counts what happens on the
 * way from the server to the speaker and logs a summary line (tag SYNCDIAG)
 * every CONFIG_SNAPCLIENT_SYNC_DIAG_INTERVAL_S seconds. Without the option
 * every call compiles to nothing.
 */
#ifndef __SYNC_DIAG_H__
#define __SYNC_DIAG_H__

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#if CONFIG_SNAPCLIENT_SYNC_DIAG

// also registers the player event callback (resyncs, free-chunk waits)
void sync_diag_init(void);
// a message arrived: its type, the server's send time and the local arrival
// time (esp_timer µs)
void sync_diag_msg(uint16_t type, int64_t sent_server_us, int64_t rx_local_us);
void sync_diag_time_sent(void);
void sync_diag_time_timeout(void);
// a time reply: its round trip, and whether it was skipped as a duplicate
void sync_diag_time_reply(int64_t rtt_us, bool duplicate);
// a time sample went into the time filter: its arrival (esp_timer µs), round
// trip, the offset it measured and the filter's estimate afterwards, and
// whether the filter counts as full. Recorded from when the filter is empty
// until TIME_SAMPLES_LOGGED samples, then logged in one block.
void sync_diag_time_sample(int64_t rx_local_us, int64_t rtt_us,
                           int64_t sample_us, int64_t est_us, bool full);
// TCP data arrived after the receiving task waited wait_us for it (it was
// ready to take data the whole time). len: bytes in this delivery; merged:
// more than one segment, i.e. held back behind a lost segment.
void sync_diag_rx(int64_t rx_local_us, int64_t wait_us, uint32_t len,
                  bool merged);

#else

static inline void sync_diag_init(void) {}
static inline void sync_diag_msg(uint16_t type, int64_t sent_server_us,
                                 int64_t rx_local_us) {}
static inline void sync_diag_time_sent(void) {}
static inline void sync_diag_time_timeout(void) {}
static inline void sync_diag_time_reply(int64_t rtt_us, bool duplicate) {}
static inline void sync_diag_time_sample(int64_t rx_local_us, int64_t rtt_us,
                                         int64_t sample_us, int64_t est_us,
                                         bool full) {}
static inline void sync_diag_rx(int64_t rx_local_us, int64_t wait_us,
                                uint32_t len, bool merged) {}

#endif

#endif  // __SYNC_DIAG_H__
