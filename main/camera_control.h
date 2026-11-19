#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "mt9v034_mode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    mt9v034_mode_t mode;
    bool running;
    bool crc;
    bool stream_enabled;
    uint16_t stream_div;
    uint16_t stream_fps;
    bool stream_pack4;
    bool row_noise;
} camera_state_t;

esp_err_t camera_control_init(void);

const mt9v034_mode_t *camera_find_preset(const char *name);
size_t camera_preset_count(void);
const mt9v034_mode_t *camera_preset_at(size_t index);

esp_err_t camera_set_preset(const char *name);
esp_err_t camera_apply_fields(int width, int height, int col_start, int row_start,
                              int hblank, int vblank, int row_bin,
                              int exposure, int gain);
esp_err_t camera_start(void);
esp_err_t camera_stop(void);
bool camera_is_running(void);
const mt9v034_mode_t *camera_mode(void);
void camera_get_state(camera_state_t *out);

void camera_set_crc(bool enable);
void camera_set_stream(bool enable, uint16_t div, uint16_t target_fps, bool pack4);
esp_err_t camera_set_row_noise(bool enable);

esp_err_t camera_read_reg(uint16_t reg, uint16_t *value);
esp_err_t camera_write_reg(uint16_t reg, uint16_t value);

#ifdef __cplusplus
}
#endif
