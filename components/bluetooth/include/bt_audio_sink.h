#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { BT_STOPPED = 0, BT_DISCONNECTED, BT_CONNECTED, BT_PLAYING, BT_PAUSED} bt_state_t;

void bt_audio_sink_init(i2s_port_t i2sN, i2s_std_gpio_config_t pin_conf, void (*set_mute)(bool, bool), bool (*lock)(bool, TickType_t));
void bt_audio_sink_start();
void bt_audio_sink_stop();
void bt_add_state_cb(void (*cb)());
void bt_audio_sink_pause(bool pause);
bt_state_t bt_get_bt_state(void);

#ifdef __cplusplus
}
#endif
