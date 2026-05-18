#pragma once

#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_chunk {
  size_t size;
  char *payload;
} audio_chunk_t;

void bt_audio_task_init(i2s_port_t i2sN, i2s_std_gpio_config_t pin_conf, void (*set_mute)(bool, bool), bool (*lock)(bool, TickType_t));
QueueHandle_t *bt_audio_task_start();
void bt_audio_task_stop();
void bt_audio_set_rate(uint16_t samplerate, uint8_t channels);
int32_t allocate_audio_chunk_memory(audio_chunk_t **pcmChunk,
                                  size_t bytes);
void free_audio_chunk(audio_chunk_t *pcmChunk);
#ifdef __cplusplus
}
#endif
