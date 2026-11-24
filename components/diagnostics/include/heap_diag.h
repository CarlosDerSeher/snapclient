#ifndef __HEAP_DIAG_H__
#define __HEAP_DIAG_H__

#include <stdbool.h>

/**
 * Runtime memory diagnostics (CONFIG_SNAPCLIENT_HEAP_DIAG).
 *
 * Logs free / largest free block / minimum free for the 8-bit capable heap
 * and the IRAM heap, plus the stack high-water mark of every task, at:
 * boot, got IP, 10 s and 10 min after playback started, and periodically.
 * All functions are no-ops when the option is disabled.
 */

void heap_diag_init(void);
void heap_diag_mark(const char *label);
void heap_diag_set_playing(bool playing);

#endif  // __HEAP_DIAG_H__
