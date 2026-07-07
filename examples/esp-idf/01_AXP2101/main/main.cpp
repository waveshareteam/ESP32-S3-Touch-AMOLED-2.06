#include <stdio.h>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "sdkconfig.h"

#define TAG "main"

// PMU interrupt and I2C config
#define PMU_INPUT_PIN (gpio_num_t) CONFIG_PMU_INTERRUPT_PIN
#define PMU_INPUT_PIN_SEL (1ULL << PMU_INPUT_PIN)

#define I2C_MASTER_NUM (i2c_port_num_t) CONFIG_I2C_MASTER_PORT_NUM
#define I2C_MASTER_FREQ_HZ CONFIG_I2C_MASTER_FREQUENCY
#define I2C_MASTER_SDA_IO (gpio_num_t) CONFIG_PMU_I2C_SDA
#define I2C_MASTER_SCL_IO (gpio_num_t) CONFIG_PMU_I2C_SCL
#define I2C_MASTER_TIMEOUT_MS 1000

static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static i2c_master_dev_handle_t pmu_dev_handle = NULL;
static QueueHandle_t gpio_evt_queue = NULL;

// Function declarations
extern esp_err_t pmu_init();
extern void pmu_isr_handler();

// ISR for GPIO
static void IRAM_ATTR pmu_irq_handler(void *arg) {
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

// I2C init with new API
esp_err_t i2c_init() {
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
            .allow_pd = 0
        }
    };

    i2c_new_master_bus(&bus_config, &i2c_bus_handle);

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x34,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0
        }
    };

    i2c_master_bus_add_device(i2c_bus_handle, &dev_config, &pmu_dev_handle);

    return ESP_OK;
}

// PMU read function using new API
int pmu_register_read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    esp_err_t ret = i2c_master_transmit_receive(pmu_dev_handle, &regAddr, 1, data, len, I2C_MASTER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PMU READ FAILED!");
        return -1;
    }
    return 0;
}

// PMU write function using new API
int pmu_register_write_byte(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len) {
    uint8_t *buffer = (uint8_t *)malloc(len + 1);
    if (!buffer) return -1;
    buffer[0] = regAddr;
    memcpy(&buffer[1], data, len);

    esp_err_t ret = i2c_master_transmit(pmu_dev_handle, buffer, len + 1, I2C_MASTER_TIMEOUT_MS);
    free(buffer);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PMU WRITE FAILED!");
        return -1;
    }
    return 0;
}

// PMU event task
static void pmu_hander_task(void *args) {
    while (1) {
        pmu_isr_handler();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

extern "C" void app_main(void) {
    // gpio_evt_queue = xQueueCreate(5, sizeof(uint32_t));
    // irq_init();

    ESP_ERROR_CHECK(i2c_init());
    ESP_LOGI(TAG, "I2C initialized successfully");

    ESP_ERROR_CHECK(pmu_init());

    xTaskCreate(pmu_hander_task, "App/pwr", 4 * 1024, NULL, 10, NULL);
}
