#include "volume_buttons.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"

static const char *TAG = "vol_btn";

/* -------------------------------------------------------------------------
 * Configuration — override via Kconfig (idf.py menuconfig → "Volume Buttons")
 * ------------------------------------------------------------------------- */
#ifndef CONFIG_VOLUME_BTN_UP_GPIO
#define CONFIG_VOLUME_BTN_UP_GPIO     27
#endif
#ifndef CONFIG_VOLUME_BTN_DOWN_GPIO
#define CONFIG_VOLUME_BTN_DOWN_GPIO   33
#endif
#ifndef CONFIG_VOLUME_BTN_DEBOUNCE_MS
#define CONFIG_VOLUME_BTN_DEBOUNCE_MS 50
#endif
#ifndef CONFIG_VOLUME_BTN_STEP
#define CONFIG_VOLUME_BTN_STEP         5
#endif

/* snapserver JSON-RPC control port */
#define SNAPRPC_PORT   1705

/* Stream name — must match the `name=` parameter in snapserver.conf */
#ifndef CONFIG_VOLUME_BTN_STREAM_ID
#define CONFIG_VOLUME_BTN_STREAM_ID   "default"
#endif

/* Task tuning */
#define VOL_TASK_STACK   8192   /* bytes — needs room for 8 KB static recv buf */
#define VOL_TASK_PRIO    5
#define VOL_POLL_MS      20     /* GPIO poll interval */

/* -------------------------------------------------------------------------
 * Internal state
 * ------------------------------------------------------------------------- */
static TaskHandle_t s_task_handle = NULL;
static volatile bool s_running    = false;


static char s_host[64]            = {0};

/* -------------------------------------------------------------------------
 * Socket helpers
 * ------------------------------------------------------------------------- */

/** Open a TCP connection to snapserver:SNAPRPC_PORT. Returns fd or -1. */
static int rpc_connect(void)
{
    struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", SNAPRPC_PORT);

    if (getaddrinfo(s_host, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGE(TAG, "getaddrinfo failed for %s", s_host);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket() failed");
        freeaddrinfo(res);
        return -1;
    }

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "connect() to %s:%d failed", s_host, SNAPRPC_PORT);
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    return fd;
}

/**
 * Send a newline-terminated JSON-RPC request and receive the response into
 * buf (max buf_len bytes). Reads until newline or buffer full.
 * Returns 0 on success, -1 on error.
 */
static int rpc_send_recv(int fd, const char *request, char *buf, size_t buf_len)
{
    size_t req_len = strlen(request);
    if (send(fd, request, req_len, 0) < 0 || send(fd, "\n", 1, 0) < 0) {
        ESP_LOGE(TAG, "send() failed");
        return -1;
    }

    size_t pos = 0;
    while (pos < buf_len - 1) {
        int n = recv(fd, buf + pos, buf_len - pos - 1, 0);
        if (n <= 0) {
            ESP_LOGE(TAG, "recv() failed (n=%d)", n);
            return -1;
        }
        /* scan for newline in newly received data */
        for (int i = (int)pos; i < (int)(pos + n); i++) {
            if (buf[i] == '\n') {
                buf[i] = '\0';
                return 0;
            }
        }
        pos += n;
    }
    buf[pos] = '\0';
    return 0;   /* buffer full but no newline — treat as complete */
}

/* -------------------------------------------------------------------------
 * Volume get / set
 * ------------------------------------------------------------------------- */

/**
 * Fetch the current stream volume from Server.GetStatus.
 * Parses the path: "streams" → "properties" → "volume"
 * to avoid matching client volumes earlier in the response.
 * Returns 0–100 on success, -1 on error.
 */
static int get_stream_volume(int fd)
{
    /* 8 KB static buffer — Server.GetStatus response is ~2–3 KB */
    static char buf[8192];

    const char *req = "{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"Server.GetStatus\"}";
    if (rpc_send_recv(fd, req, buf, sizeof(buf)) != 0)
        return -1;

    /* Walk: "streams" → "properties" → "volume" */
    char *p = strstr(buf, "\"streams\"");
    if (p) p = strstr(p, "\"properties\"");
    if (p) p = strstr(p, "\"volume\":");
    if (!p) {
        ESP_LOGW(TAG, "Could not find stream volume in Server.GetStatus response");
        return -1;
    }

    int vol = atoi(p + 9);   /* skip past "\"volume\":" */
    ESP_LOGD(TAG, "Stream volume: %d", vol);
    return vol;
}

/**
 * Send Stream.SetProperty to set stream volume.
 * Returns 0 on success.
 */
static int set_stream_volume(int fd, int volume)
{
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;

    char req[192];
    snprintf(req, sizeof(req),
             "{\"id\":2,\"jsonrpc\":\"2.0\",\"method\":\"Stream.SetProperty\","
             "\"params\":{\"id\":\"%s\",\"property\":\"volume\",\"value\":%d}}",
             CONFIG_VOLUME_BTN_STREAM_ID, volume);

    char resp[256];
    if (rpc_send_recv(fd, req, resp, sizeof(resp)) != 0)
        return -1;

    if (strstr(resp, "\"error\"")) {
        ESP_LOGW(TAG, "SetProperty error: %s", resp);
        return -1;
    }

    ESP_LOGD(TAG, "Volume set to %d", volume);
    return 0;
}

/**
 * Perform a single volume change: connect → GetStatus → SetProperty → close.
 * @param delta positive to increase, negative to decrease.
 */
static void adjust_volume(int delta)
{
    int fd = rpc_connect();
    if (fd < 0) return;

    int current = get_stream_volume(fd);
    if (current < 0) {
        close(fd);
        return;
    }

    int new_vol = current + delta;
    if (new_vol < 0)   new_vol = 0;
    if (new_vol > 100) new_vol = 100;

    if (set_stream_volume(fd, new_vol) == 0) {
        ESP_LOGI(TAG, "Volume %d → %d", current, new_vol);
    }

    close(fd);
}

/* -------------------------------------------------------------------------
 * Button polling task
 * ------------------------------------------------------------------------- */

typedef enum { BTN_IDLE = 0, BTN_PRESSED } btn_state_t;

static void volume_buttons_task(void *arg)
{
    btn_state_t up_state   = BTN_IDLE;
    btn_state_t down_state = BTN_IDLE;
    TickType_t  up_tick    = 0;
    TickType_t  down_tick  = 0;

    const TickType_t debounce = pdMS_TO_TICKS(CONFIG_VOLUME_BTN_DEBOUNCE_MS);

    while (s_running) {
        TickType_t now = xTaskGetTickCount();

        /* --- Volume Up (GPIO active-low) --- */
        bool up_active = (gpio_get_level(CONFIG_VOLUME_BTN_UP_GPIO) == 0);
        if (up_active && up_state == BTN_IDLE) {
            up_tick  = now;
            up_state = BTN_PRESSED;
        } else if (!up_active && up_state == BTN_PRESSED) {
            if ((now - up_tick) >= debounce) {
                ESP_LOGI(TAG, "Vol+");
                adjust_volume(+CONFIG_VOLUME_BTN_STEP);
            }
            up_state = BTN_IDLE;
        }

        /* --- Volume Down (GPIO active-low) --- */
        bool down_active = (gpio_get_level(CONFIG_VOLUME_BTN_DOWN_GPIO) == 0);
        if (down_active && down_state == BTN_IDLE) {
            down_tick  = now;
            down_state = BTN_PRESSED;
        } else if (!down_active && down_state == BTN_PRESSED) {
            if ((now - down_tick) >= debounce) {
                ESP_LOGI(TAG, "Vol-");
                adjust_volume(-CONFIG_VOLUME_BTN_STEP);
            }
            down_state = BTN_IDLE;
        }

        vTaskDelay(pdMS_TO_TICKS(VOL_POLL_MS));
    }

    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

esp_err_t volume_buttons_init(const char *snapserver_host)
{
    strlcpy(s_host, snapserver_host, sizeof(s_host));

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CONFIG_VOLUME_BTN_UP_GPIO) |
                        (1ULL << CONFIG_VOLUME_BTN_DOWN_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* override with external pull-up if needed */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_running = true;
    if (xTaskCreate(volume_buttons_task, "vol_btn",
                    VOL_TASK_STACK, NULL, VOL_TASK_PRIO,
                    &s_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Task create failed");
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Init OK  up=GPIO%d  down=GPIO%d  step=%d%%  stream=%s",
             CONFIG_VOLUME_BTN_UP_GPIO,
             CONFIG_VOLUME_BTN_DOWN_GPIO,
             CONFIG_VOLUME_BTN_STEP,
             CONFIG_VOLUME_BTN_STREAM_ID);
    return ESP_OK;
}

void volume_buttons_deinit(void)
{
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(VOL_POLL_MS * 2));
    s_task_handle = NULL;
}
