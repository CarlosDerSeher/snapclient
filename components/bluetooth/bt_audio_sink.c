#include "bt_audio_sink.h"

#ifdef CONFIG_SNAPCLIENT_BT_ENABLED

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "freertos/semphr.h"
#include "esp_coexist.h"

#include "bt_audio_task.h"
#include "dsp_processor.h"

#define TAG "BT_AUDIO_SINK"

static volatile bool bt_connected = false;
static esp_bd_addr_t bt_remote_addr = {0};

static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
static void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len);
//static void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
//static void bt_app_rc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);

static QueueHandle_t* pcm_queue;
static void (*bt_set_mute)(bool, bool);
static int sample_rate = 44100;
static int ch_count = 2;

typedef struct state_cb_s {
  void (*cb)(void);
  struct state_cb_s *next;
} state_cb_t;

static bt_state_t bt_state = BT_STOPPED;
static volatile bool bt_paused = false;
static state_cb_t *state_cb_head = NULL;
static SemaphoreHandle_t btStateMux = NULL;

#ifndef CONFIG_SNAPCLIENT_BT_PIN
#define CONFIG_SNAPCLIENT_BT_PIN "0000"
#endif

// Transaction labels for AVRC commands
#define APP_RC_CT_TL_GET_CAPS            (0)

bt_state_t bt_get_bt_state(void) {
  xSemaphoreTake(btStateMux, portMAX_DELAY);
  bt_state_t state = bt_state;
  xSemaphoreGive(btStateMux);
  return state;
}

void bt_call_state_cb(void) {
  state_cb_t *current = state_cb_head;
  while (current != NULL) {
    if (current->cb != NULL) {
      current->cb();
    }
    current = current->next;
  }
}

void bt_set_bt_state(bt_state_t new_state) {
    xSemaphoreTake(btStateMux, portMAX_DELAY);
    bt_state = new_state;
    xSemaphoreGive(btStateMux);
    bt_call_state_cb();
}

/**
 * add callback to be called when snapcast state changes, e.g. from not started to started.
 * Callbacks needs to be implemented thread safe as they will be called from http task
 */
void bt_add_state_cb(void (*cb)()) {
  state_cb_t *new_cb = malloc(sizeof(state_cb_t));
  if (new_cb == NULL) {
    ESP_LOGE(TAG, "Failed to allocate memory for state callback");
    return;
  }
  new_cb->cb = cb;
  new_cb->next = state_cb_head;
  state_cb_head = new_cb;
}

void bt_audio_sink_pause(bool pause) {
    xSemaphoreTake(btStateMux, portMAX_DELAY);
    if (bt_paused != pause) {
        bt_paused = pause;
        if (bt_state == BT_PLAYING) {
            xSemaphoreGive(btStateMux);
            if (pause) {
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
            }
        } else if (bt_state == BT_PAUSED) {
            xSemaphoreGive(btStateMux);
            if (!pause) {
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            }
        } else {
            xSemaphoreGive(btStateMux);
        }
    } else {
        // No state change, just release the mutex
        xSemaphoreGive(btStateMux);
    }
}


void bt_audio_sink_init(i2s_port_t i2sN, i2s_std_gpio_config_t pin_conf, void (*set_mute_)(bool, bool), bool (*lock)(bool, TickType_t)) {
    bt_set_mute = set_mute_;

    ESP_LOGI(TAG, "Initializing Bluetooth A2DP sink");

    esp_bt_mem_release(ESP_BT_MODE_BLE);
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);

    btStateMux = xSemaphoreCreateMutex();
    if (btStateMux == NULL) {
        ESP_LOGE(TAG, "Failed to create Bluetooth state mutex");
        return;
    }

    bt_audio_task_init(i2sN, pin_conf, set_mute_, lock);
}

void bt_audio_sink_start() {
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_LOGI(TAG, "BT Controller status before init: %d", esp_bt_controller_get_status());

    esp_err_t ret;
    if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "%s initialize controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        ESP_LOGE(TAG, "%s enable controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
#if (CONFIG_SNAPCLIENT_BT_SSP_ENABLED == false)
    bluedroid_cfg.ssp_en = false;
#endif
    if ((ret = esp_bluedroid_init_with_cfg(&bluedroid_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "%s initialize bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }
    if ((ret = esp_bluedroid_enable()) != ESP_OK) {
        ESP_LOGE(TAG, "%s enable bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

#if (CONFIG_SNAPCLIENT_BT_SSP_ENABLED == true)
    /* Set default parameters for Secure Simple Pairing */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));
#endif

    /*
     * Set default parameters for Legacy Pairing
     * Use variable pin, input pin code when pairing
     */
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
    esp_bt_pin_code_t pin_code;
    strcpy((char *)pin_code, CONFIG_SNAPCLIENT_BT_PIN);  // 4-digit PIN
    esp_bt_gap_set_pin(pin_type, 4, pin_code);

    esp_bt_gap_set_device_name(CONFIG_SNAPCLIENT_NAME);
    esp_bt_gap_register_callback(bt_app_gap_cb);

    // Initialize AVRC FIRST (critical for proper service discovery)
    //esp_avrc_ct_register_callback(bt_app_rc_ct_cb); // disabled, no local control
    // ret = esp_avrc_ct_init();
    // if (ret != ESP_OK) {
    //     ESP_LOGE(TAG, "AVRC CT init failed: %s", esp_err_to_name(ret));
    //     return;
    // }
    
    // esp_avrc_tg_register_callback(bt_app_rc_tg_cb);
    // ret = esp_avrc_tg_init();
    // if (ret != ESP_OK) {
    //     ESP_LOGE(TAG, "AVRC TG init failed: %s", esp_err_to_name(ret));
    //     return;
    // }
    
    // // Enable volume change notifications (critical for A2DP-style volume sync)
    // esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    // esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
    // ret = esp_avrc_tg_set_rn_evt_cap(&evt_set);
    // if (ret == ESP_OK) {
    //     ESP_LOGI(TAG, "AVRC volume change notifications enabled");
    // } else {
    //     ESP_LOGE(TAG, "Failed to enable AVRC volume notifications: %s", esp_err_to_name(ret));
    //     return;
    // }
    // vTaskDelay(pdMS_TO_TICKS(100));
    // Initialize A2DP after AVRC for proper service discovery
    esp_a2d_register_callback(&bt_app_a2d_cb);
    ret = esp_a2d_sink_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP sink init failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_a2d_sink_register_data_callback(bt_app_a2d_data_cb);

    /* Default delay value 120ms plus delay caused by application layer */
    // local delay dma: 2*1024/sr*1e4
    // local queue: (0-2)*1024/sr*1e4
    int delay =  (4*1024*100) / 441;
    esp_a2d_sink_set_delay_value(1200 + delay);

    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    xSemaphoreTake(btStateMux, portMAX_DELAY);
    bt_state = BT_DISCONNECTED;
    bt_paused = false;
    xSemaphoreGive(btStateMux);
    bt_call_state_cb();
}

void bt_audio_sink_stop() {
    esp_err_t ret;

    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    if (bt_connected) {
        bt_audio_sink_pause(true);
        ret = esp_a2d_sink_disconnect(bt_remote_addr);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Disconnect failed: %s", esp_err_to_name(ret));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    // Deinitialize AVRC FIRST
    // ret = esp_avrc_tg_deinit();
    // if (ret != ESP_OK) {
    //     ESP_LOGE(TAG, "AVRC TG deinit failed: %s", esp_err_to_name(ret));
    //     return;
    // }
    // ret = esp_avrc_ct_deinit();
    // if (ret != ESP_OK) {
    //     ESP_LOGE(TAG, "AVRC CT deinit failed: %s", esp_err_to_name(ret));
    //     return;
    // }

    // Deinitialize A2DP after AVRC
    ret = esp_a2d_sink_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP sink deinit failed: %s", esp_err_to_name(ret));
        return;
    }
    //vTaskDelay(pdMS_TO_TICKS(100));

    //shutdown bluetooth to save ram and power, we don't need it for now and it causes some issues with wifi
    if ((ret = esp_bluedroid_disable()) != ESP_OK) {
        ESP_LOGE(TAG, "%s disable bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    if ((ret = esp_bluedroid_deinit()) != ESP_OK) {
        ESP_LOGE(TAG, "%s deinit bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    if ((ret = esp_bt_controller_disable()) != ESP_OK) {
        ESP_LOGE(TAG, "%s disable controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    if ((ret = esp_bt_controller_deinit()) != ESP_OK) {
        ESP_LOGE(TAG, "%s deinit controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
#if CONFIG_SNAPCLIENT_BT_MODE_STOP
    esp_bt_mem_release(ESP_BT_MODE_BTDM);
    ESP_LOGI(TAG, "Released all memory");
#endif

    bt_set_bt_state(BT_STOPPED);
}

static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_BT_GAP_AUTH_CMPL_EVT: {
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Authentication success: %s", param->auth_cmpl.device_name);
            } else {
                ESP_LOGE(TAG, "Authentication failed, status: %d", param->auth_cmpl.stat);
            }
            break;
        }
#if (CONFIG_SNAPCLIENT_BT_SSP_ENABLED == true)
        case ESP_BT_GAP_CFM_REQ_EVT:
            ESP_LOGI(TAG, "ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %06"PRIu32, param->cfm_req.num_val);
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;
        case ESP_BT_GAP_KEY_NOTIF_EVT:
            ESP_LOGI(TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey:%06"PRIu32, param->key_notif.passkey);
            break;
        case ESP_BT_GAP_KEY_REQ_EVT:
            ESP_LOGI(TAG, "ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
            break;
#endif
        case ESP_BT_GAP_MODE_CHG_EVT:
            ESP_LOGI(TAG, "ESP_BT_GAP_MODE_CHG_EVT mode:%d", param->mode_chg.mode);
            break;
        default:
            ESP_LOGI(TAG, "other gap event: %u", (uint8_t) event);
            break;
    }
}

static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    esp_a2d_cb_param_t *a2d = NULL;
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                ESP_LOGI(TAG, "Bluetooth connected");
                bt_connected = true;
                memcpy(bt_remote_addr, param->conn_stat.remote_bda, 6*sizeof(uint8_t));
                esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
                // Give Bluetooth priority over WiFi
                esp_coex_preference_set(ESP_COEX_PREFER_BT);
                bt_set_bt_state(BT_CONNECTED);
            } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                ESP_LOGI(TAG, "Bluetooth disconnected");
                bt_connected = false;
                bt_audio_task_stop();
                pcm_queue = NULL;
                esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
                // Restore WiFi priority
                esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
                bt_set_bt_state(BT_DISCONNECTED);
            }
            break;
        case ESP_A2D_AUDIO_STATE_EVT:
            if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
                if (bt_paused) {
                    ESP_LOGI(TAG, "Bluetooth started playing while paused, sending suspend command");
                    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
                    bt_set_bt_state(BT_PAUSED);
                    break;
                }
                ESP_LOGI(TAG, "Bluetooth started playing");
                pcm_queue = bt_audio_task_start();
                bt_set_mute(false, true); //unmute state
                bt_set_bt_state(BT_PLAYING);
            } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND ||
                       param->audio_stat.state == ESP_A2D_AUDIO_STATE_STOPPED) {
                ESP_LOGI(TAG, "Bluetooth stopped playing");
                bt_set_mute(true, true); //mute state
                pcm_queue = NULL;
                bt_audio_task_stop();
                bt_set_bt_state(BT_CONNECTED);
            }
            break;
        /* when audio codec is configured, this event comes */
        case ESP_A2D_AUDIO_CFG_EVT: {
            a2d = (esp_a2d_cb_param_t *)(param);
            esp_a2d_mcc_t *p_mcc = &a2d->audio_cfg.mcc;
            ESP_LOGI(TAG, "A2DP audio stream configuration, codec type: %d", p_mcc->type);
            /* for now only SBC stream is supported */
            if (p_mcc->type == ESP_A2D_MCT_SBC) {
                sample_rate = 16000;
                ch_count = 2;
                if (p_mcc->cie.sbc_info.samp_freq & ESP_A2D_SBC_CIE_SF_32K) {
                    sample_rate = 32000;
                } else if (p_mcc->cie.sbc_info.samp_freq & ESP_A2D_SBC_CIE_SF_44K) {
                    sample_rate = 44100;
                } else if (p_mcc->cie.sbc_info.samp_freq & ESP_A2D_SBC_CIE_SF_48K) {
                    sample_rate = 48000;
                }

                if (p_mcc->cie.sbc_info.ch_mode & ESP_A2D_SBC_CIE_CH_MODE_MONO) {
                    ch_count = 1;
                }
                bt_audio_set_rate(sample_rate, ch_count);
                /* Default delay value 120ms plus delay caused by application layer */
                // local delay dma: 2*1024/sr*1e4
                // local queue: (0-2)*1024/sr*1e4
                int delay =  (4*1024*10000) / sample_rate;
                esp_a2d_sink_set_delay_value(1200 + delay);
            }
            break;
        }
        /* when a2dp init or deinit completed, this event comes */
        case ESP_A2D_PROF_STATE_EVT: {
            a2d = (esp_a2d_cb_param_t *)(param);
            if (ESP_A2D_INIT_SUCCESS == a2d->a2d_prof_stat.init_state) {
                ESP_LOGI(TAG, "A2DP PROF STATE: Init Complete");
                if (bt_remote_addr[0] != 0 || bt_remote_addr[1] != 0 || bt_remote_addr[2] != 0 || bt_remote_addr[3] != 0 || bt_remote_addr[4] != 0 || bt_remote_addr[5] != 0) {
                    ESP_LOGI(TAG, "Reconnecting to previously connected device");
                    esp_a2d_sink_connect(bt_remote_addr);
                }
            } else {
                ESP_LOGI(TAG, "A2DP PROF STATE: Deinit Complete");
            }
            break;
        }
        /* When protocol service capabilities configured, this event comes */
        case ESP_A2D_SNK_PSC_CFG_EVT: {
            a2d = (esp_a2d_cb_param_t *)(param);
            ESP_LOGI(TAG, "protocol service capabilities configured: 0x%x ", a2d->a2d_psc_cfg_stat.psc_mask);
            if (a2d->a2d_psc_cfg_stat.psc_mask & ESP_A2D_PSC_DELAY_RPT) {
                ESP_LOGI(TAG, "Peer device support delay reporting");
            } else {
                ESP_LOGI(TAG, "Peer device unsupported delay reporting");
            }
            break;
        }
        /* when set delay value completed, this event comes */
        case ESP_A2D_SNK_SET_DELAY_VALUE_EVT: {
            a2d = (esp_a2d_cb_param_t *)(param);
            if (ESP_A2D_SET_INVALID_PARAMS == a2d->a2d_set_delay_value_stat.set_state) {
                ESP_LOGI(TAG, "Set delay report value: fail");
            } else {
                ESP_LOGI(TAG, "Set delay report value: success, delay_value: %u * 1/10 ms", a2d->a2d_set_delay_value_stat.delay_value);
            }
            break;
        }
        /* when get delay value completed, this event comes */
        case ESP_A2D_SNK_GET_DELAY_VALUE_EVT: {
            a2d = (esp_a2d_cb_param_t *)(param);
            ESP_LOGI(TAG, "Get delay report value: delay_value: %u * 1/10 ms", a2d->a2d_get_delay_value_stat.delay_value);
            break;
        }
        case ESP_A2D_MEDIA_CTRL_ACK_EVT: {
            a2d = (esp_a2d_cb_param_t *)(param);
            ESP_LOGI(TAG, "Media control ack: %u", a2d->media_ctrl_stat.cmd);
            if (a2d->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_SUSPEND) {
                ESP_LOGI(TAG, "Media control suspend acknowledged");
                bt_audio_task_stop();
                pcm_queue = NULL;
            }
            break;
        }
        default:
            ESP_LOGI(TAG, "other a2dp event: %u", (uint8_t) event);
            break;
    }
}

static void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len) {
    //len is usually 4096 bytes -> 2 channel, 16bit: 1024 samples
    audio_chunk_t *pcmChunk = NULL;

    if (pcm_queue == NULL) {
        return;
    }

    if (allocate_audio_chunk_memory(&pcmChunk, len) < 0) {
        ESP_LOGE(TAG, "Failed to allocate PCM chunk");
        return;
    }

    memcpy(pcmChunk->payload, data, len);
    if (uxQueueMessagesWaiting(*pcm_queue)>2) {
        ESP_LOGI(TAG, "messages waiting: %d, len: %u", uxQueueMessagesWaiting(*pcm_queue), len);
    }

#if CONFIG_USE_DSP_PROCESSOR
    if (pcmChunk->payload) {
        dsp_processor_worker(pcmChunk->payload, pcmChunk->size / (2 * ch_count), sample_rate, ch_count);
    }
#endif

    if (xQueueSend(*pcm_queue, &pcmChunk, 0) != pdTRUE) {
        //ESP_LOGW(TAG, "Bluetooth PCM queue full");
        free_audio_chunk(pcmChunk);
    }
}


// static void bt_app_rc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
    
//     switch (event) {
//         case ESP_AVRC_CT_METADATA_RSP_EVT: {
//             break;
//         }
//         case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
//             if (param->conn_stat.connected) {
//                 ESP_LOGI(TAG, "AVRC CT connected");
//                 //avrc_connected = true;
                
//                 // Enable volume change notifications
//                 esp_avrc_ct_send_register_notification_cmd(1, ESP_AVRC_RN_VOLUME_CHANGE, 0);
                
//                 // Get remote capabilities
//                 esp_avrc_ct_send_get_rn_capabilities_cmd(APP_RC_CT_TL_GET_CAPS);

//                 esp_avrc_ct_send_set_absolute_volume_cmd(2, 100);
                
//             } else {
//                 ESP_LOGI(TAG, "AVRC CT disconnected");
//                 //avrc_connected = false;
//             }
//             break;
//         }
//         case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
//             ESP_LOGI(TAG, "Remote notification capabilities: count %d, bitmask 0x%x", 
//                      param->get_rn_caps_rsp.cap_count, param->get_rn_caps_rsp.evt_set.bits);
//             break;
//         }
//         case ESP_AVRC_CT_CHANGE_NOTIFY_EVT: {
//             break;
//         }
//         default:
//             break;
//     }
// }

// static void bt_app_rc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
    
//     switch (event) {
//         case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
//             uint8_t volume = param->set_abs_vol.volume;
//             int volume_percent = (volume * 100) / 127;
            
//             ESP_LOGI(TAG, "Volume set by remote device: %d%% (abs_vol=%d)", volume_percent, volume);
            
//             // Sync local state with remote device
// #if SNAPCAST_USE_SOFT_VOL
//             dsp_processor_set_volome((double)volume_percent / 100);
// #else
//             bt_set_volume(volume_percent);
// #endif
            
//             ESP_LOGI(TAG, "Synchronized local volume state to match remote: %d%%", volume_percent);
//             break;
//         }
//         case ESP_AVRC_TG_REMOTE_FEATURES_EVT: {
//             ESP_LOGI(TAG, "AVRC remote features: 0x%" PRIx32, param->rmt_feats.feat_mask);
//             break;
//         }
//         case ESP_AVRC_TG_CONNECTION_STATE_EVT: {
//             ESP_LOGI(TAG, "AVRC TG connection state: %s", 
//                      param->conn_stat.connected ? "CONNECTED" : "DISCONNECTED");
//             break;
//         }
//         case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT: {
//             ESP_LOGI(TAG, "AVRC TG reg notification: %u", param->reg_ntf.event_id);
//             break;
//         }
//         case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT: {
//             ESP_LOGI(TAG, "AVRC TG reg notification: %u", param->psth_cmd.key_code);
//             break;
//         }
//         default:
//             break;
//     }
// }
#endif