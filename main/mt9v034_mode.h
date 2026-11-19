#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    uint16_t width;       /* output width */
    uint16_t height;      /* output height */
    uint16_t col_start;   /* sensor column start, 1-based */
    uint16_t row_start;   /* sensor row start */
    uint16_t hblank;
    uint16_t vblank;
    uint8_t row_bin;      /* 0 = none, 1 = bin2, 2 = bin4 */
    uint16_t exposure;    /* coarse shutter width in rows */
    uint16_t gain;        /* analog gain 16..64 */
} mt9v034_mode_t;

esp_err_t mt9v034_apply_mode(const mt9v034_mode_t *mode);
esp_err_t mt9v034_read_mode(mt9v034_mode_t *mode);
void mt9v034_print_mode(const char *prefix, const mt9v034_mode_t *mode);

/* Sensor row-wise noise cancellation.  Enabling this also enables the 36
 * optically-black columns at the start of each line, so the DVP stride grows
 * by 36 and mt9v_capture_set_dark_cols(36) must be used. */
void mt9v034_set_row_noise(bool enable);

extern const mt9v034_mode_t mt9v034_mode_full;
extern const mt9v034_mode_t mt9v034_mode_bin2;
extern const mt9v034_mode_t mt9v034_mode_bin4;
extern const mt9v034_mode_t mt9v034_mode_752x60;
extern const mt9v034_mode_t mt9v034_mode_376x240;
extern const mt9v034_mode_t mt9v034_mode_188x120;
extern const mt9v034_mode_t mt9v034_mode_80x60;
extern const mt9v034_mode_t mt9v034_mode_80x30;
extern const mt9v034_mode_t mt9v034_mode_80x15;
extern const mt9v034_mode_t mt9v034_mode_80x8;
extern const mt9v034_mode_t mt9v034_mode_80x4;
extern const mt9v034_mode_t mt9v034_mode_80x2;
extern const mt9v034_mode_t mt9v034_mode_80x1;

#ifdef __cplusplus
}
#endif
