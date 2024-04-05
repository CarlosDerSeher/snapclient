//
// MA120 ESP32 Driver
//
// Merus Audio - September 2018
// Written by Joergen Kragh Jakobsen, jkj@myrun.dk
//
// Register interface thrugh I2C for MA120
//   Support a single amplifier/i2c address
//
//

#include "ma120.h"

#include <stdint.h>
#include <stdio.h>

#include "board.h"
#include "driver/i2c.h"
#include "esp_log.h"

static const char *TAG = "MA120";

#define MA_ENABLE_IO CONFIG_MA120_ENABLE_PIN
#define MA_NMUTE_IO CONFIG_MA120_NMUTE_PIN
#define MA_NERR_IO CONFIG_MERUS_NERR_PIN
#define MA_NCLIP_IO CONFIG_MERUS_NCLIP_PIN

static const char *I2C_TAG = "i2c";
#define I2C_CHECK(a, str, ret)                                                 \
  if (!(a)) {                                                                  \
    ESP_LOGE(I2C_TAG, "%s:%d (%s):%s", __FILE__, __LINE__, __FUNCTION__, str); \
    return (ret);                                                              \
  }

#define I2C_MASTER_NUM I2C_NUM_0    /*!< I2C port number for master dev */
#define I2C_MASTER_TX_BUF_DISABLE 0 /*!< I2C master do not need buffer */
#define I2C_MASTER_RX_BUF_DISABLE 0 /*!< I2C master do not need buffer */
#define I2C_MASTER_FREQ_HZ 100000   /*!< I2C master clock frequency */

#define MA120_ADDR                                           \
  CONFIG_DAC_I2C_ADDR /*!< slave address for MA120 amplifier \
                       */

#define WRITE_BIT I2C_MASTER_WRITE /*!< I2C master write */
#define READ_BIT I2C_MASTER_READ   /*!< I2C master read */
#define ACK_CHECK_EN 0x1           /*!< I2C master will check ack from slave*/
#define ACK_CHECK_DIS 0x0 /*!< I2C master will not check ack from slave */
#define ACK_VAL 0x0       /*!< I2C ack value */
#define NACK_VAL 0x1      /*!< I2C nack value */

audio_hal_func_t AUDIO_CODEC_MA120_DEFAULT_HANDLE = {
    .audio_codec_initialize = ma120_init,
    .audio_codec_deinitialize = ma120_deinit,
    .audio_codec_ctrl = ma120_ctrl,
    .audio_codec_config_iface = ma120_config_iface,
    .audio_codec_set_mute = ma120_set_mute,
    .audio_codec_set_volume = ma120_set_volume,
    .audio_codec_get_volume = ma120_get_volume,
    .audio_hal_lock = NULL,
    .handle = NULL,
};

esp_err_t ma120_deinit(void) {
  // TODO
  return ESP_OK;
}

esp_err_t ma120_ctrl(audio_hal_codec_mode_t mode, audio_hal_ctrl_t ctrl_state) {
  ESP_LOGI("MA120 Driver", "ctrl w. mode and ctrl_state");
  return ESP_OK;
}

esp_err_t ma120_config_iface(audio_hal_codec_mode_t mode,
                             audio_hal_codec_i2s_iface_t *iface) {
  ESP_LOGI("MA120 Driver", "config_iface w. mode and interface");
  return ESP_OK;
}

esp_err_t ma120_set_volume(int vol) {
  esp_err_t ret = ESP_OK;
  uint8_t cmd[2];
  cmd[0] = 128 - vol;
  cmd[1] = cmd[0];
  ma_write(MA120_ADDR, 2, 0x0003, cmd, 2);
  return ret;
}

esp_err_t ma120_get_volume(int *vol) {
  esp_err_t ret = ESP_OK;
  uint8_t rxbuf;
  rxbuf = ma_read_byte(MA120_ADDR, 2, 3);
  *vol = 128 - rxbuf;
  return ret;
}

esp_err_t ma120_set_mute(bool enable) {
  esp_err_t ret = ESP_OK;
  uint8_t nmute;
  nmute = (enable) ? 0 : 1;
  gpio_set_level(MA_NMUTE_IO, nmute);
  return ret;
}

esp_err_t ma120_get_mute(bool *enabled) {
  esp_err_t ret = ESP_OK;

  *enabled = false;  // TODO read from register
  return ret;
}

esp_err_t ma120_init(audio_hal_codec_config_t *codec_cfg) {
  esp_err_t ret = ESP_OK;
  setup_ma120();
  return ret;
}

void setup_ma120(void) {
  gpio_config_t io_conf;

  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pin_bit_mask = (1ULL << MA_ENABLE_IO | 1ULL << MA_NMUTE_IO);
  io_conf.pull_down_en = 0;
  io_conf.pull_up_en = 0;

  printf("setup output %d %d \n", MA_ENABLE_IO, MA_NMUTE_IO);
  gpio_config(&io_conf);

  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pin_bit_mask = (1ULL << MA_NCLIP_IO | 1ULL << MA_NERR_IO);
  io_conf.pull_down_en = 0;
  io_conf.pull_up_en = 0;
  printf("Setup input NCLIP:%d  NERR:%d \n", MA_NCLIP_IO, MA_NERR_IO);
  gpio_config(&io_conf);

  gpio_set_level(MA_NMUTE_IO, 0);
  gpio_set_level(MA_ENABLE_IO, 0);

  i2c_master_init();

  gpio_set_level(MA_ENABLE_IO, 1);

  uint8_t res = ma_write_byte(MA120_ADDR, 2, 0x060c, 0);
  res = ma_read_byte(MA120_ADDR, 2, 0x060c);
  printf("Hardware version: 0x%02x\n", res);

  printf("Scan I2C bus: ");
  for (uint8_t addr = 0x20; addr <= 0x23; addr++) {
    res = ma_read_byte(addr, 2, 0);
    printf(" 0x%02x => GEN2 ,", addr);
    // printf("Scan i2c address 0x%02x read address 0 : 0x%02x \n", addr ,res);
  }
  printf("\n");

  uint8_t rxbuf[32];
  uint8_t otp[1024];
  for (uint8_t i = 0; i < 16; i++) {
    ma_read(MA120_ADDR, 2, 0x8000 + i * 32, rxbuf, 32);
    // printf("%04x : ",0x8000+i*32 );
    for (uint8_t j = 0; j < 32; j++) {
      otp[i * 32 + j] = rxbuf[j];
    }
  }
  for (uint16_t i = 0; i < 16 * 32; i++) {
    if (i % 32 == 0) {
      printf("\n0x%04x : ", 0x8000 + i);
    }
    printf("%02x ", otp[i]);
  }

  res = ma_read(MA120_ADDR, 2, 0x0000, rxbuf, 2);
  printf("\nAddress 0 : 0x%02x\n", rxbuf[0]);
  ma_write_byte(MA120_ADDR, 2, 0x0003, 0x50);
  ma_write_byte(MA120_ADDR, 2, 0x0004, 0x50);
  ma_write_byte(MA120_ADDR, 2, 0x0005, 0x02);
  // ma_write_byte(MA120_ADDR,2,0x0246,0x00)  ;   //
  ESP_LOGI(TAG, "ma120_setup done [ok]");
}

#define CRED "\x1b[31m"
#define CGRE "\x1b[32m"
#define CYEL "\x1b[33m"
#define CBLU "\x1b[34m"
#define CMAG "\x1b[35m"
#define CYAN "\x1b[36m"
#define CWHI "\x1b[0m"
const char *cherr_str[] = {"Clip_stuck", "DC", "VCF", "OCP_SEV", "OCP"};
const char *syserr1_str[] = {" X ",    " X ",   "DSP3 ", " DSP2",
                             " DSP1 ", "DSP0 ", "ERR",   "PVT_low"};
const char *syserr0_str[] = {"OTW",   "OTE",   "PV_uv", "PV_low",
                             "OV_ov", " CLK ", "AUD",   " TW    "};

void ma120_read_error(uint8_t i2c_addr) {  // 0x0118 error now ch0 [clip_stuck
                                           // dc  vcf_err  ocp_severe  ocp]
  // 0x0119 error now ch1 [clip_stuck  dc  vcf_err  ocp_severe  ocp]
  // 0x011a error now system [ AE CE ... ]
  // 0x011b error now system [DSP3 DSP2 DSP1 DSP0 OC OE]
  // 0x011c error acc ch0 [clip_stuck  dc  vcf_err  ocp_severe  ocp]
  // 0x011d error acc ch1 [clip_stuck  dc  vcf_err  ocp_severe  ocp]
  // 0x011e error acc system [7..0]
  // 0x011f error acc system [13..8]
  uint8_t errbuf[10] = {0};

  ma_read(i2c_addr, 2, 0x0118, errbuf, 8);

  // Error flag now : RED
  // Error flag acc : WHITE
  // No flag set    : GREEN
  for (int i = 0; i <= 7; i++) {  // Error now
    printf(" %s%s ",
           ((errbuf[3] & (1 << i)) == (1 << i))   ? CRED
           : ((errbuf[7] & (1 << i)) == (1 << i)) ? CWHI
                                                  : CGRE,
           syserr1_str[i]);
  }
  printf(" [0x%02x 0x%02x]\n", errbuf[3], errbuf[7]);

  for (int i = 0; i <= 7; i++) {
    printf(" %s%s ",
           ((errbuf[2] & (1 << i)) == (1 << i))   ? CRED
           : ((errbuf[6] & (1 << i)) == (1 << i)) ? CWHI
                                                  : CGRE,
           syserr0_str[i]);
  }
  printf(" [0x%02x 0x%02x]\n", errbuf[2], errbuf[6]);

  // printf("0x011b : 0x%02x %s", rxbuf[2], l1 );
  // printf("\nError vectors :");
  // for (int i = 0; i<8; i++)
  //{ printf("%02x ", errbuf[i]);
  // }
  // printf("\n");
}

static i2c_config_t i2c_cfg = {
    .mode = I2C_MODE_MASTER,
    .sda_pullup_en = GPIO_PULLUP_ENABLE,
    .scl_pullup_en = GPIO_PULLUP_ENABLE,
    .master.clk_speed = I2C_MASTER_FREQ_HZ,
};

void i2c_master_init() {
  int i2c_master_port = I2C_MASTER_NUM;

  get_i2c_pins(I2C_NUM_0, &i2c_cfg);

  esp_err_t res = i2c_param_config(i2c_master_port, &i2c_cfg);
  printf("Driver param setup : %d\n", res);
  res = i2c_driver_install(i2c_master_port, i2c_cfg.mode,
                           I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE,
                           0);
  printf("Driver installed   : %d\n", res);
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
    if (i == n - 1) ack = NACK_VAL;
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
  printf("%04x %02x\n", address, value);
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
  if (ret == ESP_FAIL) {
    printf("ESP_I2C_WRITE ERROR : %d\n", ret);
    return ret;
  }
  return ESP_OK;
}

esp_err_t ma_read(uint8_t i2c_addr, uint8_t prot, uint16_t address,
                  uint8_t *rbuf, uint8_t n) {
  esp_err_t ret;
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  if (cmd == NULL) {
    printf("ERROR handle null\n");
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
    printf("i2c Error read - readback\n");
    return ESP_FAIL;
  }
  return ret;
}

uint8_t ma_read_byte(uint8_t i2c_addr, uint8_t prot, uint16_t address) {
  uint8_t value = 0;
  esp_err_t ret;
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);  // Send i2c start on bus
  i2c_master_write_byte(cmd, (i2c_addr << 1) | WRITE_BIT, ACK_CHECK_EN);
  if (prot == 2) {
    i2c_master_write_byte(cmd, (uint8_t)((address & 0xff00) >> 8), ACK_VAL);
    i2c_master_write_byte(cmd, (uint8_t)(address & 0x00ff), ACK_VAL);
  } else {
    i2c_master_write_byte(cmd, (uint8_t)address, ACK_VAL);
  }
  i2c_master_start(cmd);  // Repeated start
  i2c_master_write_byte(cmd, (i2c_addr << 1) | READ_BIT, ACK_CHECK_EN);

  i2c_master_read_byte(cmd, &value, NACK_VAL);

  i2c_master_stop(cmd);
  ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 1000 / portTICK_PERIOD_MS);
  i2c_cmd_link_delete(cmd);
  if (ret == ESP_FAIL) {
    printf("i2c Error read - readback\n");
    return ESP_FAIL;
  }
  return value;
}
