#include "camera_control.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "mt9v034_i2c.h"
#include "mt9v_capture.h"

static const char *TAG = "camera";

static const mt9v034_mode_t *const s_presets[] = {
    &mt9v034_mode_full,
    &mt9v034_mode_bin2,
    &mt9v034_mode_bin4,
    &mt9v034_mode_752x60,
    &mt9v034_mode_376x240,
    &mt9v034_mode_188x120,
    &mt9v034_mode_80x60,
    &mt9v034_mode_80x30,
    &mt9v034_mode_80x15,
    &mt9v034_mode_80x8,
    &mt9v034_mode_80x4,
    &mt9v034_mode_80x2,
    &mt9v034_mode_80x1,
};

static mt9v034_mode_t s_mode = {
    .name = "full", .width = 752, .height = 480,
    .col_start = 1, .row_start = 4, .hblank = 61, .vblank = 4,
    .row_bin = 0, .exposure = 100, .gain = 32,
};
static char s_mode_name[24] = "full";
static bool s_running;
static bool s_crc;
static bool s_stream_enabled;
static uint16_t s_stream_div = 1;
static uint16_t s_stream_fps;
static bool s_stream_pack4 = true;
static bool s_row_noise;

const mt9v034_mode_t *camera_find_preset(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(s_presets) / sizeof(s_presets[0]); i++) {
        if (strcmp(name, s_presets[i]->name) == 0) {
            return s_presets[i];
        }
    }
    return NULL;
}

size_t camera_preset_count(void)
{
    return sizeof(s_presets) / sizeof(s_presets[0]);
}

const mt9v034_mode_t *camera_preset_at(size_t index)
{
    if (index >= camera_preset_count()) return NULL;
    return s_presets[index];
}

static const mt9v034_mode_t *match_preset(const mt9v034_mode_t *m)
{
    for (size_t i = 0; i < camera_preset_count(); i++) {
        const mt9v034_mode_t *p = s_presets[i];
        if (p->width == m->width && p->height == m->height &&
            p->hblank == m->hblank && p->vblank == m->vblank &&
            p->row_bin == m->row_bin && p->exposure == m->exposure &&
            p->gain == m->gain) {
            return p;
        }
    }
    return NULL;
}

static esp_err_t apply_mode_checked(const mt9v034_mode_t *want)
{
    uint16_t chip_ctrl = 0;
    bool have_ctrl = (mt9v034_read_reg(MT9V034_REG_CHIP_CONTROL, &chip_ctrl) == ESP_OK);
    if (have_ctrl) {
        mt9v034_write_reg(MT9V034_REG_CHIP_CONTROL, chip_ctrl & ~0x0080);
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    esp_err_t err = ESP_FAIL;
    mt9v034_mode_t rb = {0};
    for (int attempt = 0; attempt < 3; attempt++) {
        err = mt9v034_apply_mode(want);
        vTaskDelay(pdMS_TO_TICKS(30));
        if (mt9v034_read_mode(&rb) == ESP_OK &&
            rb.width == want->width && rb.height == want->height &&
            rb.hblank == want->hblank && rb.vblank == want->vblank &&
            rb.row_bin == want->row_bin && rb.exposure == want->exposure &&
            rb.gain == want->gain) {
            err = ESP_OK;
            break;
        }
        ESP_LOGW(TAG, "mode verify attempt %d failed (%ux%u)", attempt + 1, rb.width, rb.height);
        err = ESP_ERR_INVALID_RESPONSE;
    }

    if (have_ctrl) {
        mt9v034_write_reg(MT9V034_REG_CHIP_CONTROL, chip_ctrl | 0x0080);
    }
    vTaskDelay(pdMS_TO_TICKS(30));

    if (err == ESP_OK) {
        s_mode = rb;
        s_mode.name = s_mode_name;
        strncpy(s_mode_name, want->name ? want->name : "custom", sizeof(s_mode_name) - 1);
        s_mode_name[sizeof(s_mode_name) - 1] = '\0';
    }
    return err;
}

esp_err_t camera_control_init(void)
{
    bool probe_ok = (mt9v034_probe() == ESP_OK);
    if (probe_ok) {
        mt9v034_mode_t m = {0};
        if (mt9v034_read_mode(&m) == ESP_OK) {
            s_mode = m;
            s_mode.name = s_mode_name;
            const mt9v034_mode_t *p = match_preset(&m);
            strncpy(s_mode_name, p ? p->name : "custom", sizeof(s_mode_name) - 1);
            mt9v034_print_mode("CAMBOOT", &s_mode);
        }
    } else {
        ESP_LOGW(TAG, "MT9V034 not responding on I2C");
    }

    s_stream_enabled = false;
    s_stream_div = 1;
    s_stream_fps = 0;
    s_stream_pack4 = true;
    /* Row-wise noise cancellation is the default: the MT9V034's dark columns
     * remove the horizontal banding seen with short exposures / USB power. */
    s_row_noise = true;
    mt9v034_set_row_noise(true);
    mt9v_capture_stream_enable(false);
    mt9v_capture_stream_config(s_stream_div, s_stream_fps, s_stream_pack4);

    if (!probe_ok) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Re-apply the current mode so R0x0D[7] and R0x70 are actually programmed
     * before the first DVP start (the sensor may have been left in any state). */
    if (apply_mode_checked(&s_mode) != ESP_OK) {
        ESP_LOGW(TAG, "row-noise apply failed, falling back to normal readout");
        s_row_noise = false;
        mt9v034_set_row_noise(false);
        apply_mode_checked(&s_mode);
    }

    esp_err_t err = camera_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "auto-start failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t camera_set_preset(const char *name)
{
    const mt9v034_mode_t *m = camera_find_preset(name);
    if (!m) {
        return ESP_ERR_NOT_FOUND;
    }

    bool was_running = s_running;
    if (was_running) {
        mt9v_capture_stop();
        s_running = false;
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    mt9v034_mode_t want = *m;
    esp_err_t err = apply_mode_checked(&want);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "preset %s failed: %s", name, esp_err_to_name(err));
    }
    if (was_running) {
        esp_err_t serr = camera_start();
        if (serr != ESP_OK) {
            ESP_LOGE(TAG, "restart failed: %s", esp_err_to_name(serr));
            return serr;
        }
    }
    return err;
}

esp_err_t camera_apply_fields(int width, int height, int col_start, int row_start,
                              int hblank, int vblank, int row_bin,
                              int exposure, int gain)
{
    mt9v034_mode_t want = s_mode;
    if (width > 0) want.width = (uint16_t)width;
    if (height > 0) want.height = (uint16_t)height;
    if (col_start > 0) want.col_start = (uint16_t)col_start;
    if (row_start > 0) want.row_start = (uint16_t)row_start;
    if (hblank > 0) want.hblank = (uint16_t)hblank;
    if (vblank > 0) want.vblank = (uint16_t)vblank;
    if (row_bin >= 0) want.row_bin = (uint8_t)row_bin;
    if (exposure > 0) want.exposure = (uint16_t)exposure;
    if (gain > 0) want.gain = (uint16_t)gain;
    want.name = "custom";

    /* height is the output height; row bin multiplies the sensor rows that
     * are read.  Clamp h so h<<row_bin never exceeds the 480-row array. */
    if (((uint32_t)want.height << want.row_bin) > 480) {
        want.height = (uint16_t)(480 >> want.row_bin);
    }

    if (want.width < 1 || want.width > 752 || want.height < 1 || want.height > 480 ||
        want.row_bin > 2) {
        return ESP_ERR_INVALID_ARG;
    }

    bool was_running = s_running;
    if (was_running) {
        mt9v_capture_stop();
        s_running = false;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    esp_err_t err = apply_mode_checked(&want);
    if (was_running) {
        esp_err_t serr = camera_start();
        if (serr != ESP_OK) {
            return serr;
        }
    }
    return err;
}

esp_err_t camera_start(void)
{
    if (s_running) {
        return ESP_OK;
    }
    if (s_mode.width == 0 || s_mode.height == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t derr = mt9v_capture_set_dark_cols(s_row_noise ? 36 : 0);
    if (derr != ESP_OK) {
        return derr;
    }
    esp_err_t err = mt9v_capture_start(s_mode.width, s_mode.height);
    if (err == ESP_OK) {
        s_running = true;
        mt9v_capture_set_crc_enabled(s_crc);
    }
    return err;
}

esp_err_t camera_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }
    esp_err_t err = mt9v_capture_stop();
    s_running = false;
    return err;
}

bool camera_is_running(void)
{
    return s_running;
}

const mt9v034_mode_t *camera_mode(void)
{
    return &s_mode;
}

void camera_get_state(camera_state_t *out)
{
    if (!out) return;
    out->mode = s_mode;
    out->running = s_running;
    out->crc = s_crc;
    out->stream_enabled = s_stream_enabled;
    out->stream_div = s_stream_div;
    out->stream_fps = s_stream_fps;
    out->stream_pack4 = s_stream_pack4;
    out->row_noise = s_row_noise;
}

void camera_set_crc(bool enable)
{
    s_crc = enable;
    mt9v_capture_set_crc_enabled(enable);
}

void camera_set_stream(bool enable, uint16_t div, uint16_t target_fps, bool pack4)
{
    if (div == 0) div = 1;
    if (div > 8) div = 8;
    s_stream_enabled = enable;
    s_stream_div = div;
    s_stream_fps = target_fps;
    s_stream_pack4 = pack4;
    mt9v_capture_stream_config(div, target_fps, pack4);
    mt9v_capture_stream_enable(enable);
}

esp_err_t camera_set_row_noise(bool enable)
{
    if (s_row_noise == enable) {
        return ESP_OK;
    }
    bool was_running = s_running;
    if (was_running) {
        mt9v_capture_stop();
        s_running = false;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    s_row_noise = enable;
    mt9v034_set_row_noise(enable);
    esp_err_t err = apply_mode_checked(&s_mode);
    if (was_running) {
        esp_err_t serr = camera_start();
        if (serr != ESP_OK) {
            return serr;
        }
    }
    return err;
}

esp_err_t camera_read_reg(uint16_t reg, uint16_t *value)
{
    return mt9v034_read_reg(reg, value);
}

esp_err_t camera_write_reg(uint16_t reg, uint16_t value)
{
    return mt9v034_write_reg(reg, value);
}
