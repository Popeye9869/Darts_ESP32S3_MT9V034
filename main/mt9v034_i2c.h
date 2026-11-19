#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The SeekFree 总钻风 module used here is in direct-I2C mode: the onboard
 * 51 MCU is bypassed and the sensor's two-wire interface is exposed on the
 * adapter's TXD/RXD pins.  The module straps S_CTRL_ADR0/1 high, so the
 * 7-bit address is 0x5C (8-bit 0xB8/0xB9).
 *
 * Wiring: ESP32-S3 GPIO44 -> SCL, GPIO43 -> SDA (confirmed by scan).
 */
#define MT9V034_I2C_SCL_GPIO 44
#define MT9V034_I2C_SDA_GPIO 43
#define MT9V034_I2C_ADDR     0x5C

#define MT9V034_REG_CHIP_VERSION   0x00
#define MT9V034_REG_COLUMN_START   0x01
#define MT9V034_REG_ROW_START      0x02
#define MT9V034_REG_WINDOW_HEIGHT  0x03
#define MT9V034_REG_WINDOW_WIDTH   0x04
#define MT9V034_REG_HBLANK         0x05
#define MT9V034_REG_VBLANK         0x06
#define MT9V034_REG_CHIP_CONTROL   0x07
#define MT9V034_REG_SHUTTER_TOTAL  0x0B
#define MT9V034_REG_RESET          0x0C
#define MT9V034_REG_READ_MODE_A    0x0D
#define MT9V034_REG_ANALOG_GAIN    0x35
#define MT9V034_REG_AEC_AGC_ENABLE 0xAF

esp_err_t mt9v034_i2c_init(void);
esp_err_t mt9v034_read_reg(uint16_t reg, uint16_t *value);
esp_err_t mt9v034_write_reg(uint16_t reg, uint16_t value);

/* Print the chip version; returns ESP_OK if the sensor answers. */
esp_err_t mt9v034_probe(void);

#ifdef __cplusplus
}
#endif
