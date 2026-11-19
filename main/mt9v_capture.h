#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MT9V_CAP_MAX_BUFFERS 8

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t frame_bytes;

    uint32_t vsy_edges;
    uint32_t href_edges;
    double vsy_fps;
    double vsy_interval_min_us;
    double vsy_interval_max_us;
    double vsy_interval_avg_us;

    uint32_t dma_frames;
    uint32_t app_frames;
    uint32_t drops;
    uint32_t cb_calls;
    uint32_t cb_fail;
    uint32_t free_head;
    uint32_t free_tail;
    uint32_t size_errors;
    uint32_t crc_frames;
    uint32_t crc_last;
    uint32_t crc_diff;
    uint32_t all_zero_frames;
    uint32_t all_ff_frames;
    uint32_t proc_avg_us;
    uint32_t proc_max_us;
    uint32_t proc_wait_avg_us;
    uint32_t proc_work_avg_us;

    uint8_t sample[16];
    bool running;
} mt9v_capture_stats_t;

esp_err_t mt9v_capture_init(void);

/* Start the LCD_CAM/GDMA DVP receiver. width/height must match the camera's
 * programmed resolution. */
esp_err_t mt9v_capture_start(uint32_t width, uint32_t height);
esp_err_t mt9v_capture_stop(void);
bool mt9v_capture_running(void);

/* Number of optically-black columns prepended by the sensor (0 or 36).
 * Must be set while capture is stopped.  The DVP stride becomes
 * width + dark_cols; all user-facing frames remain width x height. */
esp_err_t mt9v_capture_set_dark_cols(uint16_t dark_cols);

void mt9v_capture_reset_stats(void);
void mt9v_capture_get_stats(mt9v_capture_stats_t *out);

/* Copy the most recent frame into an internal buffer and return it. */
const uint8_t *mt9v_capture_last_frame(size_t *len);
void mt9v_capture_set_crc_enabled(bool enable);
void mt9v_capture_set_href_enabled(bool enable);

/* --- Latest-frame mailbox for the web stream ---------------------------
 * The capture task publishes a downsampled copy of a completed frame into a
 * free slot and never waits for the network.  The web task borrows the newest
 * slot, sends it, then releases it.  If all slots are busy the capture task
 * simply drops the stream frame; the sensor/DVP path is unaffected.
 */
typedef struct {
    const uint8_t *data;
    uint16_t width;
    uint16_t height;
    uint32_t seq;
    uint32_t timestamp_ms;
    uint32_t bytes;
    bool pack4;
    int slot;
} mt9v_stream_frame_t;

void mt9v_capture_stream_enable(bool enable);
void mt9v_capture_stream_config(uint16_t div, uint16_t target_fps, bool pack4);
void mt9v_capture_stream_get_config(bool *enabled, uint16_t *div, uint16_t *target_fps, bool *pack4);
bool mt9v_capture_stream_acquire(mt9v_stream_frame_t *frame);
void mt9v_capture_stream_release(const mt9v_stream_frame_t *frame);
void mt9v_capture_stream_get_stats(uint32_t *published, uint32_t *dropped, uint32_t *sent);
void mt9v_capture_stream_reset_stats(void);

/* Ask the capture task to copy the next completed frame for a host dump. */
esp_err_t mt9v_capture_request_dump(void);
bool mt9v_capture_dump_ready(void);
const uint8_t *mt9v_capture_get_dump(size_t *len);

#ifdef __cplusplus
}
#endif
