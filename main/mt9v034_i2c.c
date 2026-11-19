#include "mt9v034_i2c.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "mt9v034_i2c";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

esp_err_t mt9v034_i2c_init(void)
{
    if (s_dev) {
        return ESP_OK;
    }

    /* UART0 is the default camera-control path on this board; make sure it
     * releases GPIO43/44 before the I2C master claims them. */
    if (uart_is_driver_installed(UART_NUM_0)) {
        uart_driver_delete(UART_NUM_0);
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = MT9V034_I2C_SDA_GPIO,
        .scl_io_num = MT9V034_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MT9V034_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c device add failed: %s", esp_err_to_name(err));
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return err;
    }

    ESP_LOGI(TAG, "I2C ready: SCL=%d SDA=%d addr=0x%02X",
             MT9V034_I2C_SCL_GPIO, MT9V034_I2C_SDA_GPIO, MT9V034_I2C_ADDR);
    return ESP_OK;
}

esp_err_t mt9v034_read_reg(uint16_t reg, uint16_t *value)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t addr = (uint8_t)reg;
    uint8_t data[2] = {0};
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        err = i2c_master_transmit_receive(s_dev, &addr, 1, data, sizeof(data), 100);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (err == ESP_OK && value) {
        *value = ((uint16_t)data[0] << 8) | data[1];
    }
    return err;
}

esp_err_t mt9v034_write_reg(uint16_t reg, uint16_t value)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[3] = {(uint8_t)reg, (uint8_t)(value >> 8), (uint8_t)value};
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        err = i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return err;
}

esp_err_t mt9v034_probe(void)
{
    esp_err_t err = mt9v034_i2c_init();
    if (err != ESP_OK) {
        return err;
    }
    uint16_t version = 0;
    err = mt9v034_read_reg(MT9V034_REG_CHIP_VERSION, &version);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "chip version read failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "chip version 0x%04X", version);
    return version == 0x1324 ? ESP_OK : ESP_ERR_INVALID_VERSION;
}
