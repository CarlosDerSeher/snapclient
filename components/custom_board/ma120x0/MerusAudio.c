//
// MA120x0P ESP32 Driver
//
// Merus Audio - September 2018
// Written by Joergen Kragh Jakobsen, jkj@myrun.dk
//
// Register interface thrugh I2C for MA12070P and MA12040P
//   Support a single amplifier/i2c address
//
//

#include "MerusAudio.h"

#include <stdint.h>

#include "board.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ma120x0.h"
#include "audio_hal.h"

static const char *TAG = "MA120X0";
static int s_volume = 100; /* cached volume; re-applied at CTRL_START */

#define MA_NENABLE_IO CONFIG_MA120X0_NENABLE_PIN
#define MA_NMUTE_IO   CONFIG_MA120X0_NMUTE_PIN
#define MA_NERR_IO    CONFIG_MA120X0_NERR_PIN
#define MA_NCLIP_IO   CONFIG_MA120X0_NCLIP_PIN

#ifdef CONFIG_MA120X0_NENABLE_INVERT
#define MA_NENABLE_ON  0
#define MA_NENABLE_OFF 1
#else
#define MA_NENABLE_ON  1
#define MA_NENABLE_OFF 0
#endif


#define I2C_MASTER_NUM I2C_NUM_0	/*!< I2C port number for master dev */
#define I2C_MASTER_TX_BUF_DISABLE 0 /*!< I2C master do not need buffer */
#define I2C_MASTER_RX_BUF_DISABLE 0 /*!< I2C master do not need buffer */
#define I2C_MASTER_FREQ_HZ 100000	/*!< I2C master clock frequency */

#define MA120X0_ADDR                                                           \
	CONFIG_DAC_I2C_ADDR /*!< slave address for MA120X0 amplifier */

#define WRITE_BIT I2C_MASTER_WRITE /*!< I2C master write */
#define READ_BIT I2C_MASTER_READ   /*!< I2C master read */
#define ACK_CHECK_EN 0x1		   /*!< I2C master will check ack from slave*/
#define ACK_CHECK_DIS 0x0 /*!< I2C master will not check ack from slave */
#define ACK_VAL 0x0		  /*!< I2C ack value */
#define NACK_VAL 0x1	  /*!< I2C nack value */

static i2c_config_t i2c_cfg;

esp_err_t ma120x0_init(audio_hal_codec_config_t *codec_cfg);
esp_err_t ma120x0_deinit(void);
static void ma120x0_error_monitor_task(void *arg);
esp_err_t ma120x0_config_iface(audio_hal_codec_mode_t mode,
							   audio_hal_codec_i2s_iface_t *iface);
esp_err_t ma120x0_set_volume(int vol);
esp_err_t ma120x0_get_volume(int *vol);
esp_err_t ma120x0_set_mute(bool enable);
esp_err_t ma120x0_ctrl(audio_hal_codec_mode_t mode,
					   audio_hal_ctrl_t ctrl_state);

audio_hal_func_t AUDIO_CODEC_MA120X0_DEFAULT_HANDLE = {
	.audio_codec_initialize = ma120x0_init,
	.audio_codec_deinitialize = ma120x0_deinit,
	.audio_codec_ctrl = ma120x0_ctrl,
	.audio_codec_config_iface = ma120x0_config_iface,
	.audio_codec_set_mute = ma120x0_set_mute,
	.audio_codec_set_volume = ma120x0_set_volume,
	.audio_codec_get_volume = ma120x0_get_volume,
	.audio_hal_lock = NULL,
	.handle = NULL,
};

esp_err_t ma120x0_deinit(void) {
	if (MA_NMUTE_IO != GPIO_NUM_NC)
		gpio_set_level(MA_NMUTE_IO, 0); // mute first
	vTaskDelay(pdMS_TO_TICKS(30));
	if (MA_NENABLE_IO != GPIO_NUM_NC)
		gpio_set_level(MA_NENABLE_IO, MA_NENABLE_OFF);
	return ESP_OK;
}

esp_err_t ma120x0_ctrl(audio_hal_codec_mode_t mode,
					   audio_hal_ctrl_t ctrl_state) {
	ESP_LOGD(TAG, "ctrl: mode=%d ctrl_state=%d", mode, ctrl_state);
	if (ctrl_state == AUDIO_HAL_CTRL_START) {
		// Re-apply volume in case it was set before the I2S clock was running.
		ma120x0_set_volume(s_volume);
		return ma120x0_set_mute(false);
	} else if (ctrl_state == AUDIO_HAL_CTRL_STOP) {
		return ma120x0_set_mute(true);
	}
	return ESP_OK;
}

esp_err_t ma120x0_config_iface(audio_hal_codec_mode_t mode,
							   audio_hal_codec_i2s_iface_t *iface) {
	// TODO
	return ESP_OK;
}

esp_err_t ma120x0_set_volume(int vol) {
	esp_err_t ret = ESP_OK;
	uint8_t cmd;
	if (vol > 100) vol = 100;
	if (vol < 0) vol = 0;
	s_volume = vol;
	// reg64: 0x18 = 0 dB, each +1 step = -1 dB.
	// Map slider 0..100 linearly to [VOL_MIN_DB .. VOL_MAX_DB].
	int db = CONFIG_MA120X0_VOL_MAX_DB +
	         (100 - vol) * (CONFIG_MA120X0_VOL_MIN_DB - CONFIG_MA120X0_VOL_MAX_DB) / 100;
	cmd = (uint8_t)(0x18 - db);
	ret = ma_write_byte(MA120X0_ADDR, 1, 64, cmd);
	if (ret != ESP_OK)
		ESP_LOGW(TAG, "set_volume: I2C write failed (ret=%d)", ret);
	else
		ESP_LOGD(TAG, "set_volume: vol=%d cmd=0x%02x", vol, cmd);
	return ret;
}

esp_err_t ma120x0_get_volume(int *vol) {
	*vol = s_volume;
	return ESP_OK;
}

esp_err_t ma120x0_set_mute(bool enable) {
	ESP_LOGD(TAG, "set_mute: %s", enable ? "muted" : "unmuted");
	esp_err_t ret = ESP_OK;
	uint8_t nmute = (enable) ? 0 : 1;
	gpio_set_level(MA_NMUTE_IO, nmute);
	return ret;
}

esp_err_t ma120x0_get_mute(bool *enabled) {
	esp_err_t ret = ESP_OK;

	*enabled = false; // TODO read from register
	return ret;
}

esp_err_t ma120x0_init(audio_hal_codec_config_t *codec_cfg) {
	esp_err_t ret = ESP_OK;
	gpio_config_t io_conf;

	uint64_t output_mask = 0;
	if (MA_NENABLE_IO != GPIO_NUM_NC)
		output_mask |= (1ULL << MA_NENABLE_IO);
	if (MA_NMUTE_IO != GPIO_NUM_NC)
		output_mask |= (1ULL << MA_NMUTE_IO);
	if (output_mask) {
		io_conf.intr_type = GPIO_INTR_DISABLE;
		io_conf.mode = GPIO_MODE_OUTPUT;
		io_conf.pin_bit_mask = output_mask;
		io_conf.pull_down_en = 0;
		io_conf.pull_up_en = 0;
		ESP_LOGD(TAG, "setup output nEnable=%d nMute=%d", MA_NENABLE_IO,
				 MA_NMUTE_IO);
		gpio_config(&io_conf);
	}

	uint64_t input_mask = 0;
	if (MA_NCLIP_IO != GPIO_NUM_NC)
		input_mask |= (1ULL << MA_NCLIP_IO);
	if (MA_NERR_IO != GPIO_NUM_NC)
		input_mask |= (1ULL << MA_NERR_IO);
	if (input_mask) {
		io_conf.intr_type = GPIO_INTR_DISABLE;
		io_conf.mode = GPIO_MODE_INPUT;
		io_conf.pin_bit_mask = input_mask;
		io_conf.pull_down_en = 0;
		io_conf.pull_up_en = 0;
		ESP_LOGD(TAG, "setup input nClip=%d nErr=%d", MA_NCLIP_IO, MA_NERR_IO);
		gpio_config(&io_conf);
	}

	// Muted and disabled while I2C bus initialises
	if (MA_NMUTE_IO != GPIO_NUM_NC)
		gpio_set_level(MA_NMUTE_IO, 0);
	if (MA_NENABLE_IO != GPIO_NUM_NC)
		gpio_set_level(MA_NENABLE_IO, MA_NENABLE_OFF);

	i2c_master_init();

	// Enable the chip and wait for PVDD to stabilize before register writes
	if (MA_NENABLE_IO != GPIO_NUM_NC) {
		gpio_set_level(MA_NENABLE_IO, MA_NENABLE_ON);
		vTaskDelay(pdMS_TO_TICKS(100));
	}

	// i2s standard (bits 0:2), requires 32-bit word length
	// audio_proc_enable=1  (bit 3) - enabled volume control
	// audio_proc_release=0 (bits 4:5) - slow mode
	// audio_proc_attack=0  (bits 6:7) - slow mode
	ESP_LOGD(TAG, "apply_format_regs: writing reg53=0x08 for I2S format");
	ma_write_byte(MA120X0_ADDR, 1, 53, 0x08); 

	// Set master volume to 0 dB.
	ma_write_byte(MA120X0_ADDR, 1, 64, 0x18);

	xTaskCreate(ma120x0_error_monitor_task, "ma120x0_err", 4096, NULL, 5, NULL);

	ESP_LOGI(TAG, "init complete");
	return ret;
}

static void ma120x0_error_monitor_task(void *arg) {
	vTaskDelay(pdMS_TO_TICKS(5000)); // wait for audio clock to stabilize
	while (1) {
		ma120_read_error(MA120X0_ADDR);
		vTaskDelay(pdMS_TO_TICKS(5000));
	}
}

// ma_error__a (reg 124) bit definitions (from Linux driver):
// bit 0: flycap   bit 1: overcurr  bit 2: pll/clk  bit 3: pvdd_uv
// bit 4: otw      bit 5: ote       bit 6: low_imp   bit 7: dc_prot
static const char * const ma_err_bit_names[] = {
	"flycap", "overcurr", "pll/clk", "pvdd_uv",
	"otw", "ote", "low_imp", "dc_prot"
};

void ma120_read_error(uint8_t i2c_addr) {
	// reg 124 = ma_error__a: current error flags
	// reg 109 = ma_error_acc__a: accumulated error flags
	uint8_t err_now = ma_read_byte(i2c_addr, 1, 124);
	uint8_t err_acc = ma_read_byte(i2c_addr, 1, 109);

	if (err_now == 0 && err_acc == 0)
		return;

	if (err_now) {
		ESP_LOGE(TAG, "errors now (reg124=0x%02x):", err_now);
		for (int i = 0; i < 8; i++)
			if (err_now & (1 << i))
				ESP_LOGE(TAG, "  [bit%d] %s", i, ma_err_bit_names[i]);
	}
	if (err_acc) {
		ESP_LOGW(TAG, "errors accumulated (reg109=0x%02x):", err_acc);
		for (int i = 0; i < 8; i++)
			if (err_acc & (1 << i))
				ESP_LOGW(TAG, "  [bit%d] %s", i, ma_err_bit_names[i]);
	}

	// reg 116: i2s_data_rate (bits 1:0, 00/01/10=x1/x2/x4)
	//          audio_in_mode_mon (bits 4:2)
	uint8_t reg116 = ma_read_byte(i2c_addr, 1, 116);
	ESP_LOGI(TAG, "  i2s_data_rate=%u audio_in_mode_mon=%u",
		reg116 & 0x03, (reg116 & 0x1c) >> 2);

	if (err_acc) {
		ESP_LOGW(TAG, "clearing accumulated errors via eh_clear (reg45)");
		// Preserve thermal_compr_en (bit 5, reset=1) while toggling eh_clear (bit 2)
		ma_write_byte(i2c_addr, 1, 45, 0x24);
		ma_write_byte(i2c_addr, 1, 45, 0x20);
	}
}

void i2c_master_init() {
	int i2c_master_port = I2C_MASTER_NUM;
	i2c_cfg = (i2c_config_t){
		.mode = I2C_MODE_MASTER,
		.sda_pullup_en = GPIO_PULLUP_ENABLE,
		.scl_pullup_en = GPIO_PULLUP_ENABLE,
		.master.clk_speed = I2C_MASTER_FREQ_HZ,
	};
	get_i2c_pins(I2C_NUM_0, &i2c_cfg);

	esp_err_t res = i2c_param_config(i2c_master_port, &i2c_cfg);
	ESP_LOGD(TAG, "I2C param config: %d", res);
	res = i2c_driver_install(i2c_master_port, i2c_cfg.mode,
							 I2C_MASTER_RX_BUF_DISABLE,
							 I2C_MASTER_TX_BUF_DISABLE, 0);
	ESP_LOGD(TAG, "I2C driver installed: %d", res);
}

esp_err_t ma_write(uint8_t i2c_addr, uint8_t prot, uint16_t address,
				   uint8_t *wbuf, uint8_t n) {
	bool ack = ACK_VAL;
	i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, i2c_addr << 1 | WRITE_BIT, ACK_CHECK_EN);
	if (prot == 2) {
		i2c_master_write_byte(cmd, (uint8_t)((address & 0xff00) >> 8), ACK_VAL);
		i2c_master_write_byte(cmd, (uint8_t)(address & 0x00ff), ACK_VAL);
	} else {
		i2c_master_write_byte(cmd, (uint8_t)address, ACK_VAL);
	}

	for (int i = 0; i < n; i++) {
		if (i == n - 1)
			ack = NACK_VAL;
		i2c_master_write_byte(cmd, wbuf[i], ack);
	}
	i2c_master_stop(cmd);
	int ret =
		i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 1000 / portTICK_PERIOD_MS);
	i2c_cmd_link_delete(cmd);
	if (ret == ESP_FAIL) {
		return ret;
	}
	return ESP_OK;
}

esp_err_t ma_write_byte(uint8_t i2c_addr, uint8_t prot, uint16_t address,
						uint8_t value) {
	ESP_LOGD(TAG, "write addr=0x%04x val=0x%02x", address, value);
	esp_err_t ret = 0;
	i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (i2c_addr << 1) | WRITE_BIT, ACK_CHECK_EN);
	if (prot == 2) {
		i2c_master_write_byte(cmd, (uint8_t)((address & 0xff00) >> 8), ACK_VAL);
		i2c_master_write_byte(cmd, (uint8_t)(address & 0x00ff), ACK_VAL);
	} else {
		i2c_master_write_byte(cmd, (uint8_t)address, ACK_VAL);
	}
	i2c_master_write_byte(cmd, value, ACK_VAL);
	i2c_master_stop(cmd);
	ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 1000 / portTICK_PERIOD_MS);
	i2c_cmd_link_delete(cmd);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "I2C write failed addr=0x%02x reg=%u ret=%d", i2c_addr, address, ret);
		return ret;
	}
	return ESP_OK;
}

esp_err_t ma_read(uint8_t i2c_addr, uint8_t prot, uint16_t address,
				  uint8_t *rbuf, uint8_t n) {
	esp_err_t ret;
	i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	if (cmd == NULL) {
		ESP_LOGE(TAG, "I2C cmd handle null");
	}
	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (i2c_addr << 1) | WRITE_BIT, ACK_CHECK_EN);
	if (prot == 2) {
		i2c_master_write_byte(cmd, (uint8_t)((address & 0xff00) >> 8), ACK_VAL);
		i2c_master_write_byte(cmd, (uint8_t)(address & 0x00ff), ACK_VAL);
	} else {
		i2c_master_write_byte(cmd, (uint8_t)address, ACK_VAL);
	}
	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (i2c_addr << 1) | READ_BIT, ACK_CHECK_EN);
	// if (n == 1 )
	i2c_master_read(cmd, rbuf, n - 1, ACK_VAL);
	// for (uint8_t i = 0;i<n;i++)
	// { i2c_master_read_byte(cmd, rbuf++, ACK_VAL); }
	i2c_master_read_byte(cmd, rbuf + n - 1, NACK_VAL);
	i2c_master_stop(cmd);
	ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 100 / portTICK_PERIOD_MS);
	i2c_cmd_link_delete(cmd);
	if (ret == ESP_FAIL) {
		ESP_LOGD(TAG, "I2C read error - readback");
		return ESP_FAIL;
	}
	return ret;
}

uint8_t ma_read_byte(uint8_t i2c_addr, uint8_t prot, uint16_t address) {
	uint8_t value = 0;
	esp_err_t ret;
	i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	i2c_master_start(cmd); // Send i2c start on bus
	i2c_master_write_byte(cmd, (i2c_addr << 1) | WRITE_BIT, ACK_CHECK_EN);
	if (prot == 2) {
		i2c_master_write_byte(cmd, (uint8_t)((address & 0xff00) >> 8), ACK_VAL);
		i2c_master_write_byte(cmd, (uint8_t)(address & 0x00ff), ACK_VAL);
	} else {
		i2c_master_write_byte(cmd, (uint8_t)address, ACK_VAL);
	}
	i2c_master_start(cmd); // Repeated start
	i2c_master_write_byte(cmd, (i2c_addr << 1) | READ_BIT, ACK_CHECK_EN);

	i2c_master_read_byte(cmd, &value, NACK_VAL);

	i2c_master_stop(cmd);
	ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 1000 / portTICK_PERIOD_MS);
	i2c_cmd_link_delete(cmd);
	if (ret == ESP_FAIL) {
		ESP_LOGD(TAG, "I2C read error - readback");
		return ESP_FAIL;
	}
	return value;
}
