#include <stdio.h>
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_lcd_sh8601.h"
#include "esp_lcd_touch_ft5x06.h"

#include "esp_codec_dev_defaults.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "bsp_err_check.h"
#include "bsp/display.h"
#include "bsp/touch.h"

static const char *TAG = "ESP32-S3-Touch-AMOLED-2.06";

static i2c_master_bus_handle_t i2c_handle = NULL; // I2C Handle
static bool i2c_initialized = false;
static lv_indev_t *disp_indev = NULL;
sdmmc_card_t *bsp_sdcard = NULL; // Global uSD card handler
static esp_lcd_touch_handle_t tp = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL; // LCD panel handle
static esp_lcd_panel_io_handle_t io_handle = NULL;
uint8_t brightness;

typedef enum {
    BSP_AUDIO_MODE_NONE,
    BSP_AUDIO_MODE_STD_STD,
    BSP_AUDIO_MODE_TX_STD_RX_TDM,
} bsp_audio_mode_t;

static bsp_audio_mode_t audio_mode = BSP_AUDIO_MODE_NONE;
static i2s_chan_handle_t i2s_tx_chan = NULL;
static i2s_chan_handle_t i2s_rx_chan = NULL;
static const audio_codec_data_if_t *i2s_data_if = NULL; /* Codec data interface */

static esp_codec_dev_handle_t speaker_codec_dev = NULL;
static const audio_codec_gpio_if_t *speaker_gpio_if = NULL;
static const audio_codec_ctrl_if_t *speaker_ctrl_if = NULL;
static const audio_codec_if_t *speaker_codec_if = NULL;

static esp_codec_dev_handle_t microphone_codec_dev = NULL;
static const audio_codec_ctrl_if_t *microphone_ctrl_if = NULL;
static const audio_codec_if_t *microphone_codec_if = NULL;

#define BSP_ES7210_CODEC_ADDR ES7210_CODEC_DEFAULT_ADDR
#define BSP_I2S_GPIO_CFG       \
    {                          \
        .mclk = BSP_I2S_MCLK,  \
        .bclk = BSP_I2S_SCLK,  \
        .ws = BSP_I2S_LCLK,    \
        .dout = BSP_I2S_DOUT,  \
        .din = BSP_I2S_DSIN,   \
        .invert_flags = {      \
            .mclk_inv = false, \
            .bclk_inv = false, \
            .ws_inv = false,   \
        },                     \
    }

static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x63, (uint8_t[]){0xFF}, 1, 10},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x16, 0x01, 0xAF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xF5}, 4, 0},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    /* Keep AMOLED emission off until the application has drawn its first frame. */
};

#define BSP_I2S_DUPLEX_MONO_CFG(_sample_rate)                                                         \
    {                                                                                                 \
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(_sample_rate),                                          \
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO), \
        .gpio_cfg = BSP_I2S_GPIO_CFG,                                                                 \
    }

/**************************************************************************************************
 *
 * I2C Function
 *
 **************************************************************************************************/
esp_err_t bsp_i2c_init(void)
{
    /* I2C was initialized before */
    if (i2c_initialized)
    {
        return ESP_OK;
    }

    i2c_master_bus_config_t i2c_bus_conf = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .i2c_port = BSP_I2C_NUM,
    };
    BSP_ERROR_CHECK_RETURN_ERR(i2c_new_master_bus(&i2c_bus_conf, &i2c_handle));

    i2c_initialized = true;

    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void)
{
    BSP_ERROR_CHECK_RETURN_ERR(i2c_del_master_bus(i2c_handle));
    i2c_initialized = false;
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    bsp_i2c_init();
    return i2c_handle;
}

esp_err_t bsp_spiffs_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_BSP_SPIFFS_MOUNT_POINT,
        .partition_label = CONFIG_BSP_SPIFFS_PARTITION_LABEL,
        .max_files = CONFIG_BSP_SPIFFS_MAX_FILES,
#ifdef CONFIG_BSP_SPIFFS_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
    };

    esp_err_t ret_val = esp_vfs_spiffs_register(&conf);

    BSP_ERROR_CHECK_RETURN_ERR(ret_val);

    size_t total = 0, used = 0;
    ret_val = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret_val != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret_val));
    }
    else
    {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    return ret_val;
}

esp_err_t bsp_spiffs_unmount(void)
{
    return esp_vfs_spiffs_unregister(CONFIG_BSP_SPIFFS_PARTITION_LABEL);
}

esp_err_t bsp_sdcard_mount(void)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .max_files = 5,
        .allocation_unit_size = 16 * 1024};

    const sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    const sdmmc_slot_config_t slot_config = {
        .clk = BSP_SD_CLK,
        .cmd = BSP_SD_CMD,
        .d0 = BSP_SD_D0,
        .d1 = GPIO_NUM_NC,
        .d2 = GPIO_NUM_NC,
        .d3 = GPIO_NUM_NC,
        .d4 = GPIO_NUM_NC,
        .d5 = GPIO_NUM_NC,
        .d6 = GPIO_NUM_NC,
        .d7 = GPIO_NUM_NC,
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 1,
        .flags = 0,
    };

#if !CONFIG_FATFS_LONG_FILENAMES
    ESP_LOGW(TAG, "Warning: Long filenames on SD card are disabled in menuconfig!");
#endif

    return esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &bsp_sdcard);
}

esp_err_t bsp_sdcard_unmount(void)
{
    return esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, bsp_sdcard);
}

static void bsp_audio_update_result(esp_err_t *result, esp_err_t error)
{
    if (*result == ESP_OK && error != ESP_OK) {
        *result = error;
    }
}

static void bsp_audio_update_codec_result(esp_err_t *result, int codec_error)
{
    if (*result == ESP_OK && codec_error != ESP_CODEC_DEV_OK) {
        *result = ESP_FAIL;
    }
}

static bool bsp_audio_has_resources(void)
{
    return audio_mode != BSP_AUDIO_MODE_NONE || i2s_tx_chan != NULL ||
           i2s_rx_chan != NULL || i2s_data_if != NULL ||
           speaker_codec_dev != NULL || speaker_gpio_if != NULL ||
           speaker_ctrl_if != NULL || speaker_codec_if != NULL ||
           microphone_codec_dev != NULL || microphone_ctrl_if != NULL ||
           microphone_codec_if != NULL;
}

static esp_err_t bsp_audio_delete_speaker_codec(void)
{
    esp_err_t ret = ESP_OK;

    if (speaker_codec_dev != NULL) {
        bsp_audio_update_codec_result(&ret, esp_codec_dev_close(speaker_codec_dev));
        esp_codec_dev_delete(speaker_codec_dev);
        speaker_codec_dev = NULL;
    }
    if (speaker_codec_if != NULL) {
        bsp_audio_update_codec_result(&ret, audio_codec_delete_codec_if(speaker_codec_if));
        speaker_codec_if = NULL;
    }
    if (speaker_ctrl_if != NULL) {
        bsp_audio_update_codec_result(&ret, audio_codec_delete_ctrl_if(speaker_ctrl_if));
        speaker_ctrl_if = NULL;
    }
    if (speaker_gpio_if != NULL) {
        bsp_audio_update_codec_result(&ret, audio_codec_delete_gpio_if(speaker_gpio_if));
        speaker_gpio_if = NULL;
    }

    return ret;
}

static esp_err_t bsp_audio_delete_microphone_codec(void)
{
    esp_err_t ret = ESP_OK;

    if (microphone_codec_dev != NULL) {
        bsp_audio_update_codec_result(&ret, esp_codec_dev_close(microphone_codec_dev));
        esp_codec_dev_delete(microphone_codec_dev);
        microphone_codec_dev = NULL;
    }
    if (microphone_codec_if != NULL) {
        bsp_audio_update_codec_result(&ret, audio_codec_delete_codec_if(microphone_codec_if));
        microphone_codec_if = NULL;
    }
    if (microphone_ctrl_if != NULL) {
        bsp_audio_update_codec_result(&ret, audio_codec_delete_ctrl_if(microphone_ctrl_if));
        microphone_ctrl_if = NULL;
    }

    return ret;
}

static esp_err_t bsp_audio_delete_i2s_channel(i2s_chan_handle_t *channel)
{
    esp_err_t ret = ESP_OK;
    esp_err_t err;

    if (*channel == NULL) {
        return ESP_OK;
    }

    err = i2s_channel_disable(*channel);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ret = err;
    }
    bsp_audio_update_result(&ret, i2s_del_channel(*channel));
    *channel = NULL;

    return ret;
}

esp_err_t bsp_audio_deinit(void)
{
    esp_err_t ret = ESP_OK;

    bsp_audio_update_result(&ret, bsp_audio_delete_microphone_codec());
    bsp_audio_update_result(&ret, bsp_audio_delete_speaker_codec());
    if (i2s_data_if != NULL) {
        bsp_audio_update_codec_result(&ret, audio_codec_delete_data_if(i2s_data_if));
        i2s_data_if = NULL;
    }
    bsp_audio_update_result(&ret, bsp_audio_delete_i2s_channel(&i2s_rx_chan));
    bsp_audio_update_result(&ret, bsp_audio_delete_i2s_channel(&i2s_tx_chan));
    audio_mode = BSP_AUDIO_MODE_NONE;

    return ret;
}

static esp_err_t bsp_audio_init_channels(const i2s_std_config_t *tx_config,
                                         const i2s_std_config_t *rx_std_config,
                                         const i2s_tdm_config_t *rx_tdm_config,
                                         bsp_audio_mode_t mode)
{
    esp_err_t ret = ESP_OK;

    if (bsp_audio_has_resources()) {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_GOTO_ON_ERROR(i2s_new_channel(&chan_cfg, &i2s_tx_chan, &i2s_rx_chan),
                      err, TAG, "I2S channel creation failed");
    ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(i2s_tx_chan, tx_config),
                      err, TAG, "I2S TX STD initialization failed");
    if (rx_tdm_config != NULL) {
        ESP_GOTO_ON_ERROR(i2s_channel_init_tdm_mode(i2s_rx_chan, rx_tdm_config),
                          err, TAG, "I2S RX TDM initialization failed");
    } else {
        ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(i2s_rx_chan, rx_std_config),
                          err, TAG, "I2S RX STD initialization failed");
    }
    ESP_GOTO_ON_ERROR(i2s_channel_enable(i2s_tx_chan), err, TAG, "I2S TX enabling failed");
    ESP_GOTO_ON_ERROR(i2s_channel_enable(i2s_rx_chan), err, TAG, "I2S RX enabling failed");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = i2s_rx_chan,
        .tx_handle = i2s_tx_chan,
    };
    i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (i2s_data_if == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto err;
    }

    audio_mode = mode;
    return ESP_OK;

err:
    (void)bsp_audio_deinit();
    return ret;
}

esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config)
{
    if (audio_mode == BSP_AUDIO_MODE_STD_STD && i2s_tx_chan != NULL &&
        i2s_rx_chan != NULL && i2s_data_if != NULL) {
        return ESP_OK;
    }
    if (bsp_audio_has_resources()) {
        return ESP_ERR_INVALID_STATE;
    }

    const i2s_std_config_t default_config = BSP_I2S_DUPLEX_MONO_CFG(22050);
    const i2s_std_config_t *config = i2s_config != NULL ? i2s_config : &default_config;
    return bsp_audio_init_channels(config, config, NULL, BSP_AUDIO_MODE_STD_STD);
}

esp_err_t bsp_audio_init_tx_std_rx_tdm(const i2s_std_config_t *tx_config,
                                       const i2s_tdm_config_t *rx_config)
{
    if (bsp_audio_has_resources()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (tx_config == NULL || rx_config == NULL ||
        tx_config->clk_cfg.sample_rate_hz != rx_config->clk_cfg.sample_rate_hz) {
        return ESP_ERR_INVALID_ARG;
    }

    return bsp_audio_init_channels(tx_config, NULL, rx_config, BSP_AUDIO_MODE_TX_STD_RX_TDM);
}

esp_err_t bsp_audio_init_voice_24k(void)
{
    i2s_std_config_t tx_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = BSP_I2S_GPIO_CFG,
    };
    i2s_tdm_config_t rx_config = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(24000),
        .slot_cfg = I2S_TDM_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
            I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = BSP_I2S_GPIO_CFG,
    };

    tx_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    tx_config.gpio_cfg.din = I2S_GPIO_UNUSED;
    rx_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    rx_config.clk_cfg.bclk_div = 8;
    rx_config.slot_cfg.total_slot = BSP_AUDIO_TDM_SLOT_COUNT;
    rx_config.gpio_cfg.dout = I2S_GPIO_UNUSED;

    return bsp_audio_init_tx_std_rx_tdm(&tx_config, &rx_config);
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    if (speaker_codec_dev != NULL) {
        return speaker_codec_dev;
    }

    BSP_ERROR_CHECK_RETURN_NULL(bsp_i2c_init());
    if (i2s_data_if == NULL) {
        BSP_ERROR_CHECK_RETURN_NULL(bsp_audio_init(NULL));
    }

    speaker_gpio_if = audio_codec_new_gpio();
    if (speaker_gpio_if == NULL) {
        goto err;
    }
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_handle,
    };
    speaker_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (speaker_ctrl_if == NULL) {
        goto err;
    }

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = speaker_ctrl_if,
        .gpio_if = speaker_gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    speaker_codec_if = es8311_codec_new(&es8311_cfg);
    if (speaker_codec_if == NULL) {
        goto err;
    }

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = speaker_codec_if,
        .data_if = i2s_data_if,
    };
    speaker_codec_dev = esp_codec_dev_new(&codec_dev_cfg);
    if (speaker_codec_dev != NULL) {
        return speaker_codec_dev;
    }

err:
    (void)bsp_audio_delete_speaker_codec();
    return NULL;
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    if (microphone_codec_dev != NULL) {
        return microphone_codec_dev;
    }

    BSP_ERROR_CHECK_RETURN_NULL(bsp_i2c_init());
    if (i2s_data_if == NULL) {
        BSP_ERROR_CHECK_RETURN_NULL(bsp_audio_init(NULL));
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = BSP_ES7210_CODEC_ADDR,
        .bus_handle = i2c_handle,
    };
    microphone_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (microphone_ctrl_if == NULL) {
        goto err;
    }
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = microphone_ctrl_if,
        .mic_selected = audio_mode == BSP_AUDIO_MODE_TX_STD_RX_TDM ?
                        BSP_AUDIO_ES7210_CONNECTED_MIC_MASK : 0,
    };
    microphone_codec_if = es7210_codec_new(&es7210_cfg);
    if (microphone_codec_if == NULL) {
        goto err;
    }

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = microphone_codec_if,
        .data_if = i2s_data_if,
    };
    microphone_codec_dev = esp_codec_dev_new(&codec_dev_cfg);
    if (microphone_codec_dev != NULL) {
        return microphone_codec_dev;
    }

err:
    (void)bsp_audio_delete_microphone_codec();
    return NULL;
}

#define LCD_CMD_BITS (8)
#define LCD_PARAM_BITS (8)
#define LCD_LEDC_CH (CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH)
#define LVGL_TICK_PERIOD_MS (CONFIG_BSP_DISPLAY_LVGL_TICK)
#define LVGL_MAX_SLEEP_MS (CONFIG_BSP_DISPLAY_LVGL_MAX_SLEEP)

esp_err_t bsp_display_brightness_init(void)
{
    return bsp_display_brightness_set(0);
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    if (panel_handle == NULL)
    {
        ESP_LOGE(TAG, "Panel handle is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (brightness_percent < 0 || brightness_percent > 100)
    {
        ESP_LOGE(TAG, "Invalid brightness percentage. Should be between 0 and 100.");
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t lcd_cmd = 0x51;
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= 0x02 << 24;
    uint8_t param = (uint8_t)(brightness_percent * 255 / 100);
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(io_handle, lcd_cmd, &param, 1),
        TAG, "Set AMOLED brightness failed"
    );
    brightness = param;

    return ESP_OK;
}

int bsp_display_brightness_get(void)
{
    if (panel_handle == NULL)
    {
        ESP_LOGE(TAG, "Panel handle is not initialized");
        return -1;
    }

    return brightness * 100 / 255;
}

esp_err_t bsp_display_backlight_off(void)
{
    ESP_LOGI(TAG, "Backlight off");
    return bsp_display_brightness_set(0);
}

esp_err_t bsp_display_backlight_on(void)
{
    ESP_LOGI(TAG, "Backlight on");
    return bsp_display_brightness_set(100);
}
#if LVGL_VERSION_MAJOR >= 9
static void rounder_event_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    uint16_t x1 = area->x1;
    uint16_t x2 = area->x2;

    uint16_t y1 = area->y1;
    uint16_t y2 = area->y2;

    // round the start of coordinate down to the nearest 2M number
    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    // round the end of coordinate up to the nearest 2N+1 number
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
}
#else
static void bsp_lvgl_rounder_cb(lv_disp_drv_t *disp_drv, lv_area_t *area)
{
    uint16_t x1 = area->x1;
    uint16_t x2 = area->x2;

    uint16_t y1 = area->y1;
    uint16_t y2 = area->y2;

    // round the start of coordinate down to the nearest 2M number
    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    // round the end of coordinate up to the nearest 2N+1 number
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
}
#endif
esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel, esp_lcd_panel_io_handle_t *ret_io)
{
    esp_err_t ret = ESP_OK;
    assert(config != NULL && config->max_transfer_sz > 0);

    ESP_LOGI(TAG, "Initialize SPI bus");
    const spi_bus_config_t buscfg = SH8601_PANEL_BUS_QSPI_CONFIG(BSP_LCD_PCLK,
                                                                 BSP_LCD_DATA0,
                                                                 BSP_LCD_DATA1,
                                                                 BSP_LCD_DATA2,
                                                                 BSP_LCD_DATA3,
                                                                 config->max_transfer_sz);
    ESP_ERROR_CHECK(spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO));

    const esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, NULL, NULL);

    sh8601_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &io_handle));
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);
    esp_lcd_panel_set_gap(panel_handle, 0x16, 0);
    esp_lcd_panel_disp_on_off(panel_handle, true);

    if (ret_panel)
    {
        *ret_panel = panel_handle;
    }
    if (ret_io)
    {
        *ret_io = io_handle;
    }
    return ret;
}

esp_err_t bsp_touch_new(const bsp_touch_config_t *config, esp_lcd_touch_handle_t *ret_touch)
{
    /* Initilize I2C */
    BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());

    /* Initialize touch */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_LCD_TOUCH_RST,
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_config.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle), TAG, "");
    return esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, ret_touch);
}

static lv_display_t *bsp_display_lcd_init(const bsp_display_cfg_t *cfg)
{
    const uint32_t buffer_size = cfg->buffer_size ? cfg->buffer_size : BSP_LCD_DRAW_BUFF_SIZE;
    const bsp_display_config_t disp_config = {
        .max_transfer_sz = buffer_size * BSP_LCD_BITS_PER_PIXEL / 8,
    };

    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_new(&disp_config, &panel_handle, &io_handle));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = buffer_size,
        .trans_size = cfg->trans_size,
        .double_buffer = cfg->double_buffer,

        .monochrome = false,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif

        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .sw_rotate = true,
            .buff_dma = cfg->flags.buff_dma,
            .buff_spiram = cfg->flags.buff_spiram,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = true,
#endif
        }};

    // SH8601 uses QSPI: LVGL must wait for on_color_trans_done before
    // reusing the draw buffer. The RGB path signals completion immediately
    // and registers callbacks against an incompatible panel handle.
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp)
    {
        return NULL;
    }

#if LVGL_VERSION_MAJOR >= 9
    lv_display_add_event_cb(disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
#else
    lv_disp_t *disp_v8 = (lv_disp_t *)disp;
    if (disp_v8 && disp_v8->driver) {
        disp_v8->driver->rounder_cb = bsp_lvgl_rounder_cb;
    }
#endif

    return disp;
}

static lv_indev_t *bsp_display_indev_init(lv_display_t *disp)
{
    BSP_ERROR_CHECK_RETURN_NULL(bsp_touch_new(NULL, &tp));
    assert(tp);

    /* Add touch input (for selected screen) */
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = tp,
    };

    return lvgl_port_add_touch(&touch_cfg);
}

/**********************************************************************************************************
 *
 * Display Function
 *
 **********************************************************************************************************/
lv_display_t *bsp_display_start(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        }};
    return bsp_display_start_with_config(&cfg);
}

lv_display_t *bsp_display_start_with_config(const bsp_display_cfg_t *cfg)
{
    lv_display_t *disp;

    assert(cfg != NULL);
    BSP_ERROR_CHECK_RETURN_NULL(lvgl_port_init(&cfg->lvgl_port_cfg));

    BSP_NULL_CHECK(disp = bsp_display_lcd_init(cfg), NULL);

    BSP_NULL_CHECK(disp_indev = bsp_display_indev_init(disp), NULL);

    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_brightness_init());

    return disp;
}

lv_indev_t *bsp_display_get_input_dev(void)
{
    return disp_indev;
}

void bsp_display_rotate(lv_display_t *disp, lv_disp_rotation_t rotation)
{
    lv_disp_set_rotation(disp, rotation);
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    lvgl_port_unlock();
}
