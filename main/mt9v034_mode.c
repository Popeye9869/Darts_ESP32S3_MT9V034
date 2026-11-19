#include "mt9v034_mode.h"

#include <stdio.h>
#include "esp_log.h"
#include "mt9v034_i2c.h"

static const char *TAG = "mt9v034_mode";
static bool s_row_noise;

const mt9v034_mode_t mt9v034_mode_full = {
    .name = "full", .width = 752, .height = 480,
    .col_start = 1, .row_start = 4, .hblank = 61, .vblank = 4,
    .row_bin = 0, .exposure = 400, .gain = 64,
};
const mt9v034_mode_t mt9v034_mode_bin2 = {
    .name = "bin2", .width = 752, .height = 240,
    .col_start = 1, .row_start = 4, .hblank = 61, .vblank = 4,
    .row_bin = 1, .exposure = 200, .gain = 64,
};
const mt9v034_mode_t mt9v034_mode_bin4 = {
    .name = "bin4", .width = 752, .height = 120,
    .col_start = 1, .row_start = 4, .hblank = 61, .vblank = 4,
    .row_bin = 2, .exposure = 100, .gain = 64,
};
const mt9v034_mode_t mt9v034_mode_752x60 = {
    .name = "752x60", .width = 752, .height = 60,
    .col_start = 1, .row_start = 214, .hblank = 61, .vblank = 4,
    .row_bin = 0, .exposure = 50, .gain = 64,
};
const mt9v034_mode_t mt9v034_mode_376x240 = {
    .name = "376x240", .width = 376, .height = 240,
    .col_start = 189, .row_start = 124, .hblank = 314, .vblank = 4,
    .row_bin = 0, .exposure = 200, .gain = 64,
};
const mt9v034_mode_t mt9v034_mode_188x120 = {
    .name = "188x120", .width = 188, .height = 120,
    .col_start = 283, .row_start = 184, .hblank = 502, .vblank = 4,
    .row_bin = 0, .exposure = 100, .gain = 64,
};
const mt9v034_mode_t mt9v034_mode_80x60 = {
    .name = "80x60", .width = 80, .height = 60,
    .col_start = 337, .row_start = 214, .hblank = 610, .vblank = 4,
    .row_bin = 0, .exposure = 50, .gain = 64,
};
const mt9v034_mode_t mt9v034_mode_80x30 = {
    .name = "80x30", .width = 80, .height = 30,
    .col_start = 337, .row_start = 229, .hblank = 610, .vblank = 4,
    .row_bin = 0, .exposure = 28, .gain = 64,
};
const mt9v034_mode_t mt9v034_mode_80x15 = {
    .name = "80x15", .width = 80, .height = 15,
    .col_start = 337, .row_start = 236, .hblank = 610, .vblank = 4,
    .row_bin = 0, .exposure = 15, .gain = 64,
};
const mt9v034_mode_t mt9v034_mode_80x8 = {
    .name = "80x8", .width = 80, .height = 8,
    .col_start = 337, .row_start = 240, .hblank = 610, .vblank = 4,
    .row_bin = 0, .exposure = 8, .gain = 64,
};
const mt9v034_mode_t mt9v034_mode_80x4 = {
    .name = "80x4", .width = 80, .height = 4,
    .col_start = 337, .row_start = 242, .hblank = 610, .vblank = 4,
    .row_bin = 0, .exposure = 4, .gain = 64,
};
const mt9v034_mode_t mt9v034_mode_80x2 = {
    .name = "80x2", .width = 80, .height = 2,
    .col_start = 337, .row_start = 243, .hblank = 610, .vblank = 4,
    .row_bin = 0, .exposure = 2, .gain = 64,
};
const mt9v034_mode_t mt9v034_mode_80x1 = {
    .name = "80x1", .width = 80, .height = 1,
    .col_start = 337, .row_start = 243, .hblank = 610, .vblank = 4,
    .row_bin = 0, .exposure = 2, .gain = 64,
};

esp_err_t mt9v034_apply_mode(const mt9v034_mode_t *m)
{
    if (!m) {
        return ESP_ERR_INVALID_ARG;
    }
    if (m->width == 0 || m->height == 0 || m->width > 752 || m->height > 480 ||
        m->row_bin > 2) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t read_mode = 0;
    esp_err_t err = mt9v034_read_reg(MT9V034_REG_READ_MODE_A, &read_mode);
    if (err != ESP_OK) {
        return err;
    }
    read_mode = (read_mode & ~0x0003u) | (m->row_bin & 0x3);
    if (s_row_noise) {
        read_mode |= 0x0080;   /* Show Dark Columns: required by row noise corr. */
    } else {
        read_mode &= ~0x0080;
    }

    /* Manual exposure/gain: AEC and AGC must be off for R0x0B to be used. */
    if ((err = mt9v034_write_reg(MT9V034_REG_AEC_AGC_ENABLE, 0x0000)) != ESP_OK) return err;
    if ((err = mt9v034_write_reg(MT9V034_REG_READ_MODE_A, read_mode)) != ESP_OK) return err;
    if ((err = mt9v034_write_reg(0x70, s_row_noise ? 0x0001 : 0x0000)) != ESP_OK) return err;
    if ((err = mt9v034_write_reg(0x71, 0x002A)) != ESP_OK) return err;

    uint16_t sensor_height = (uint16_t)(m->height << m->row_bin);
    if ((err = mt9v034_write_reg(MT9V034_REG_WINDOW_HEIGHT, sensor_height)) != ESP_OK) return err;
    if ((err = mt9v034_write_reg(MT9V034_REG_WINDOW_WIDTH, m->width)) != ESP_OK) return err;
    if ((err = mt9v034_write_reg(MT9V034_REG_COLUMN_START, m->col_start)) != ESP_OK) return err;
    if ((err = mt9v034_write_reg(MT9V034_REG_ROW_START, m->row_start)) != ESP_OK) return err;
    if ((err = mt9v034_write_reg(MT9V034_REG_HBLANK, m->hblank)) != ESP_OK) return err;
    if ((err = mt9v034_write_reg(MT9V034_REG_VBLANK, m->vblank)) != ESP_OK) return err;
    if ((err = mt9v034_write_reg(MT9V034_REG_SHUTTER_TOTAL, m->exposure)) != ESP_OK) return err;
    if ((err = mt9v034_write_reg(MT9V034_REG_ANALOG_GAIN, m->gain)) != ESP_OK) return err;

    uint16_t rb_rm = 0, rb_rn = 0;
    if ((err = mt9v034_read_reg(MT9V034_REG_READ_MODE_A, &rb_rm)) != ESP_OK) return err;
    if ((err = mt9v034_read_reg(0x70, &rb_rn)) != ESP_OK) return err;
    if (((rb_rm & 0x0080) != 0) != s_row_noise) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if ((rb_rn & 0x0001) != (s_row_noise ? 1 : 0)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "applied %s: %ux%u row_bin=%u hblank=%u vblank=%u exp=%u gain=%u",
             m->name ? m->name : "?", m->width, m->height, m->row_bin,
             m->hblank, m->vblank, m->exposure, m->gain);
    return ESP_OK;
}

esp_err_t mt9v034_read_mode(mt9v034_mode_t *m)
{
    if (!m) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t height, width, col, row, hb, vb, rm, exp, gain;
    esp_err_t err;
    if ((err = mt9v034_read_reg(MT9V034_REG_READ_MODE_A, &rm)) != ESP_OK) return err;
    if ((err = mt9v034_read_reg(MT9V034_REG_WINDOW_HEIGHT, &height)) != ESP_OK) return err;
    if ((err = mt9v034_read_reg(MT9V034_REG_WINDOW_WIDTH, &width)) != ESP_OK) return err;
    if ((err = mt9v034_read_reg(MT9V034_REG_COLUMN_START, &col)) != ESP_OK) return err;
    if ((err = mt9v034_read_reg(MT9V034_REG_ROW_START, &row)) != ESP_OK) return err;
    if ((err = mt9v034_read_reg(MT9V034_REG_HBLANK, &hb)) != ESP_OK) return err;
    if ((err = mt9v034_read_reg(MT9V034_REG_VBLANK, &vb)) != ESP_OK) return err;
    if ((err = mt9v034_read_reg(MT9V034_REG_SHUTTER_TOTAL, &exp)) != ESP_OK) return err;
    if ((err = mt9v034_read_reg(MT9V034_REG_ANALOG_GAIN, &gain)) != ESP_OK) return err;

    uint8_t row_bin = rm & 0x3;
    uint8_t col_bin = (rm >> 2) & 0x3;
    m->name = "read";
    m->width = width >> col_bin;
    m->height = height >> row_bin;
    m->col_start = col;
    m->row_start = row;
    m->hblank = hb;
    m->vblank = vb;
    m->row_bin = row_bin;
    m->exposure = exp;
    m->gain = gain;
    return ESP_OK;
}

void mt9v034_set_row_noise(bool enable)
{
    s_row_noise = enable;
}

void mt9v034_print_mode(const char *prefix, const mt9v034_mode_t *m)
{
    printf("%s name=%s out=%ux%u sensor_rows=%u start=(%u,%u) hblank=%u vblank=%u "
           "row_bin=%u exp=%u gain=%u\n",
           prefix, m->name ? m->name : "?", m->width, m->height,
           (unsigned)(m->height << m->row_bin),
           m->col_start, m->row_start, m->hblank, m->vblank,
           m->row_bin, m->exposure, m->gain);
}
