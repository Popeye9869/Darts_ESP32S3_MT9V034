#include "mt9v_capture.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_dvp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"

static const char *TAG = "mt9v_cap";

#define PIN_D0 11
#define PIN_D1 9
#define PIN_D2 8
#define PIN_D3 10
#define PIN_D4 12
#define PIN_D5 18
#define PIN_D6 17
#define PIN_D7 16
#define PIN_VSYNC 6
#define PIN_HREF 7
#define PIN_PCLK 13

#define MAX_FRAME_BYTES ((752 + 36) * 480)
#define FRAME_ALIGN 64
#define VSY_RING 2048

static esp_cam_ctlr_handle_t s_cam;
static uint8_t *s_buf[MT9V_CAP_MAX_BUFFERS];
static uint8_t *s_scratch;
static uint8_t *s_dump_buf;
static size_t s_dump_len;
static volatile bool s_dump_request;
static volatile bool s_dump_ready;

#define STREAM_SLOTS 3
typedef struct {
    uint8_t *buf;
    volatile bool ready;
    volatile bool busy;
    uint16_t width;
    uint16_t height;
    uint32_t seq;
    uint32_t timestamp_ms;
    uint32_t bytes;
    bool pack4;
} stream_slot_t;

static stream_slot_t s_slots[STREAM_SLOTS];
static portMUX_TYPE s_stream_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_stream_enabled;
static volatile uint16_t s_stream_div = 1;
static volatile uint16_t s_stream_target_fps;
static volatile bool s_stream_pack4 = true;
static volatile uint32_t s_stream_pub;
static volatile uint32_t s_stream_drop;
static volatile uint32_t s_stream_sent;
static uint32_t s_stream_last_us;
static int s_nbuf;
static size_t s_buf_len;
static QueueHandle_t s_ready_q;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static uint32_t s_width;
static uint32_t s_height;
static uint32_t s_stride;
static uint16_t s_dark_cols;
static size_t s_frame_bytes;
static size_t s_dma_bytes;
static volatile bool s_running;

/* Single-producer/single-consumer ring of free frame buffer indices.  The
 * task produces (after processing) and both the task (at start) and the DMA
 * ISR consume. */
static volatile uint32_t s_free_head;
static volatile uint32_t s_free_tail;
static int s_free_ring[MT9V_CAP_MAX_BUFFERS];
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile uint32_t s_dma_frames;
static volatile uint32_t s_app_frames;
static volatile uint32_t s_drops;
static volatile uint32_t s_cb_calls;
static volatile uint32_t s_cb_fail;
static volatile uint32_t s_size_errors;
static volatile uint32_t s_crc_frames;
static volatile uint32_t s_crc_last;
static volatile uint32_t s_crc_diff;
static volatile uint32_t s_all_zero;
static volatile uint32_t s_all_ff;
static volatile uint32_t s_proc_sum_us;
static volatile uint32_t s_proc_max_us;
static volatile uint32_t s_proc_wait_sum_us;
static volatile uint32_t s_proc_work_sum_us;
static volatile uint32_t s_proc_count;
static volatile bool s_crc_enabled = false;
static volatile uint32_t s_buf_recv[MT9V_CAP_MAX_BUFFERS];
static uint8_t s_sample[16];

static volatile uint32_t s_vsy_count;
static volatile uint32_t s_href_count;
static volatile uint32_t s_vsy_head;
static volatile uint32_t s_vsy_ts[VSY_RING];
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;

static inline int buf_index(const uint8_t *p)
{
    for (int i = 0; i < s_nbuf; i++) {
        if (s_buf[i] == p) {
            return i;
        }
    }
    return -1;
}

static void IRAM_ATTR vsy_isr(void *arg)
{
    (void)arg;
    portENTER_CRITICAL_ISR(&s_stats_mux);
    uint32_t idx = s_vsy_head;
    s_vsy_ts[idx & (VSY_RING - 1)] = (uint32_t)esp_timer_get_time();
    s_vsy_head = idx + 1;
    s_vsy_count = s_vsy_count + 1;
    portEXIT_CRITICAL_ISR(&s_stats_mux);
}

static void IRAM_ATTR href_isr(void *arg)
{
    (void)arg;
    s_href_count = s_href_count + 1;
}

static inline bool free_pop(int *idx)
{
    bool ok = false;
    portENTER_CRITICAL_SAFE(&s_ring_mux);
    uint32_t tail = s_free_tail;
    uint32_t head = s_free_head;
    if (tail != head) {
        *idx = s_free_ring[tail & (MT9V_CAP_MAX_BUFFERS - 1)];
        s_free_tail = tail + 1;
        ok = true;
    }
    portEXIT_CRITICAL_SAFE(&s_ring_mux);
    return ok;
}

static inline bool free_push(int idx)
{
    bool ok = false;
    portENTER_CRITICAL_SAFE(&s_ring_mux);
    uint32_t head = s_free_head;
    uint32_t tail = s_free_tail;
    if (head - tail < MT9V_CAP_MAX_BUFFERS) {
        s_free_ring[head & (MT9V_CAP_MAX_BUFFERS - 1)] = idx;
        s_free_head = head + 1;
        ok = true;
    }
    portEXIT_CRITICAL_SAFE(&s_ring_mux);
    return ok;
}

static bool IRAM_ATTR on_get_new_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    (void)handle;
    (void)user_data;
    int idx;

    s_cb_calls = s_cb_calls + 1;
    if (!free_pop(&idx)) {
        s_cb_fail = s_cb_fail + 1;
        s_drops = s_drops + 1;
        trans->buffer = s_scratch;
        trans->buflen = s_buf_len;
        return false;
    }
    trans->buffer = s_buf[idx];
    trans->buflen = s_buf_len;
    return false;
}

static bool IRAM_ATTR on_trans_finished(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    (void)handle;
    (void)user_data;
    BaseType_t woken = pdFALSE;
    int idx = buf_index(trans->buffer);

    if (idx < 0) {
        if (trans->buffer == s_scratch) {
            s_dma_frames = s_dma_frames + 1;
        }
        return pdFALSE;
    }
    s_buf_recv[idx] = trans->received_size;
    s_dma_frames = s_dma_frames + 1;

    if (xQueueSendFromISR(s_ready_q, &idx, &woken) != pdTRUE) {
        s_drops = s_drops + 1;
        free_push(idx);
    }
    return woken == pdTRUE;
}

static void stream_publish(const uint8_t *src)
{
    if (!s_stream_enabled) {
        return;
    }

    uint16_t div = s_stream_div;
    if (div == 0) div = 1;
    if (div > 8) div = 8;
    uint16_t w = (uint16_t)(s_width / div);
    uint16_t h = (uint16_t)(s_height / div);
    if (!w || !h) {
        return;
    }

    uint32_t now = (uint32_t)esp_timer_get_time();
    uint16_t fps = s_stream_target_fps;
    if (fps && (now - s_stream_last_us) < (1000000u / fps)) {
        return;
    }

    int slot = -1;
    portENTER_CRITICAL(&s_stream_mux);
    for (int i = 0; i < STREAM_SLOTS; i++) {
        if (!s_slots[i].ready && !s_slots[i].busy) {
            slot = i;
            break;
        }
    }
    portEXIT_CRITICAL(&s_stream_mux);

    if (slot < 0) {
        s_stream_drop = s_stream_drop + 1;
        return;
    }

    uint8_t *dst = s_slots[slot].buf;
    bool pack4 = s_stream_pack4;
    uint32_t pixels = (uint32_t)w * h;
    uint32_t bytes = pack4 ? (pixels + 1) / 2 : pixels;
    if (bytes > MAX_FRAME_BYTES) {
        return;
    }

    if (pack4) {
        /* Adaptive 4-bit: stretch the current frame's min/max into 16 levels
         * before packing.  A plain >>4 loses an entire dark frame (the
         * short-exposure 80x60 image only reaches ~14 LSB). */
        uint8_t vmin = 255, vmax = 0;
        for (uint16_t y = 0; y < h; y++) {
            const uint8_t *srow = src + (size_t)(y * div) * s_stride + s_dark_cols;
            for (uint16_t x = 0; x < w; x++) {
                uint8_t v = srow[x * div];
                if (v < vmin) vmin = v;
                if (v > vmax) vmax = v;
            }
        }
        uint32_t range = (vmax > vmin) ? (uint32_t)(vmax - vmin) : 1;
        memset(dst, 0, bytes);
        for (uint16_t y = 0; y < h; y++) {
            const uint8_t *srow = src + (size_t)(y * div) * s_stride + s_dark_cols;
            for (uint16_t x = 0; x < w; x++) {
                uint8_t v = srow[x * div];
                uint8_t q = (uint8_t)(((uint32_t)(v - vmin) * 15u) / range);
                uint32_t idx = (uint32_t)y * w + x;
                if (idx & 1) {
                    dst[idx >> 1] |= q;
                } else {
                    dst[idx >> 1] |= (uint8_t)(q << 4);
                }
            }
        }
    } else if (div == 1) {
        memcpy(dst, src, (size_t)w * h);
    } else {
        for (uint16_t y = 0; y < h; y++) {
            const uint8_t *srow = src + (size_t)(y * div) * s_stride + s_dark_cols;
            uint8_t *drow = dst + (size_t)y * w;
            for (uint16_t x = 0; x < w; x++) {
                drow[x] = srow[x * div];
            }
        }
    }

    portENTER_CRITICAL(&s_stream_mux);
    s_slots[slot].width = w;
    s_slots[slot].height = h;
    s_slots[slot].seq = s_stream_pub + 1;
    s_slots[slot].timestamp_ms = now / 1000;
    s_slots[slot].bytes = bytes;
    s_slots[slot].pack4 = pack4;
    s_slots[slot].ready = true;
    s_stream_pub = s_stream_pub + 1;
    portEXIT_CRITICAL(&s_stream_mux);

    s_stream_last_us = now;
}

static void capture_task(void *arg)
{
    (void)arg;
    int idx;

    while (1) {
        if (xQueueReceive(s_ready_q, &idx, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (idx < 0 || idx >= s_nbuf) {
            continue;
        }

        uint32_t t_start = (uint32_t)esp_timer_get_time();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t t_work = (uint32_t)esp_timer_get_time();
        uint8_t *buf = s_buf[idx];
        size_t len = s_frame_bytes;
        if (s_buf_recv[idx] != s_buf_len) {
            s_size_errors++;
        }

        if (len >= 16) {
            memcpy(s_sample, buf + s_dark_cols, sizeof(s_sample));
        }

        if (s_crc_enabled && len) {
            uint32_t crc;
            if (s_dark_cols) {
                crc = 0;
                for (uint32_t y = 0; y < s_height; y++) {
                    crc = esp_rom_crc32_le(crc, buf + (size_t)y * s_stride + s_dark_cols, s_width);
                }
            } else {
                crc = esp_rom_crc32_le(0, buf, len);
            }
            if (s_crc_frames > 0 && crc != s_crc_last) {
                s_crc_diff++;
            }
            s_crc_last = crc;
            s_crc_frames++;
        }

        /* Cheap sanity check: count completely blank frames. */
        bool zero = true;
        bool ff = true;
        for (size_t i = 0; i < len; i += 97) {
            uint8_t v = buf[i];
            if (v != 0x00) zero = false;
            if (v != 0xFF) ff = false;
            if (!zero && !ff) break;
        }
        if (zero && len) s_all_zero++;
        if (ff && len) s_all_ff++;

        s_app_frames++;
        if (s_dump_request && !s_dump_ready) {
            if (s_dark_cols) {
                for (uint32_t y = 0; y < s_height; y++) {
                    memcpy(s_dump_buf + (size_t)y * s_width,
                           buf + (size_t)y * s_stride + s_dark_cols, s_width);
                }
                s_dump_len = s_frame_bytes;
            } else {
                memcpy(s_dump_buf, buf, len);
                s_dump_len = len;
            }
            s_dump_ready = true;
            s_dump_request = false;
        }
        stream_publish(buf);
        uint32_t now = (uint32_t)esp_timer_get_time();
        uint32_t dt = now - t_start;
        s_proc_sum_us += dt;
        s_proc_wait_sum_us += t_work - t_start;
        s_proc_work_sum_us += now - t_work;
        if (dt > s_proc_max_us) s_proc_max_us = dt;
        s_proc_count++;
        free_push(idx);
        xSemaphoreGive(s_lock);
    }
}

esp_err_t mt9v_capture_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    s_ready_q = xQueueCreate(MT9V_CAP_MAX_BUFFERS, sizeof(int));
    if (!s_lock || !s_ready_q) {
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < MT9V_CAP_MAX_BUFFERS; i++) {
        s_buf[i] = heap_caps_aligned_alloc(FRAME_ALIGN, MAX_FRAME_BYTES,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!s_buf[i]) {
            ESP_LOGE(TAG, "frame buffer %d allocation failed", i);
            return ESP_ERR_NO_MEM;
        }
        s_nbuf++;
    }
    s_buf_len = MAX_FRAME_BYTES;
    s_scratch = heap_caps_aligned_alloc(FRAME_ALIGN, MAX_FRAME_BYTES,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_scratch) {
        ESP_LOGE(TAG, "scratch buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }
    s_dump_buf = heap_caps_aligned_alloc(FRAME_ALIGN, MAX_FRAME_BYTES,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_dump_buf) {
        ESP_LOGE(TAG, "dump buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < STREAM_SLOTS; i++) {
        s_slots[i].buf = heap_caps_aligned_alloc(FRAME_ALIGN, MAX_FRAME_BYTES,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!s_slots[i].buf) {
            ESP_LOGE(TAG, "stream slot %d allocation failed", i);
            return ESP_ERR_NO_MEM;
        }
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_VSYNC,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "vsync gpio config");
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(PIN_VSYNC, vsy_isr, NULL), TAG, "vsync isr");

    gpio_config_t io_href = {
        .pin_bit_mask = 1ULL << PIN_HREF,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_href), TAG, "href gpio config");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(PIN_HREF, href_isr, NULL), TAG, "href isr");
    gpio_intr_disable(PIN_HREF);

    BaseType_t ok = xTaskCreatePinnedToCore(capture_task, "capture", 4096, NULL, 8, &s_task, 1);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "initialised, %d x %d-byte PSRAM frame buffers", s_nbuf, MAX_FRAME_BYTES);
    return ESP_OK;
}

esp_err_t mt9v_capture_set_dark_cols(uint16_t dark_cols)
{
    if (dark_cols != 0 && dark_cols != 36) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    s_dark_cols = dark_cols;
    return ESP_OK;
}

esp_err_t mt9v_capture_start(uint32_t width, uint32_t height)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (width == 0 || height == 0 || width > 752 || height > 480) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_running) {
        xSemaphoreGive(s_lock);
        mt9v_capture_stop();
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }

    s_width = width;
    s_height = height;
    s_stride = width + s_dark_cols;
    s_frame_bytes = (size_t)width * height;
    s_dma_bytes = (size_t)s_stride * height;
    s_buf_len = (s_dma_bytes + FRAME_ALIGN - 1) & ~(FRAME_ALIGN - 1);
    if (s_buf_len > MAX_FRAME_BYTES) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_cam_ctlr_dvp_pin_config_t pin = {
        .data_width = CAM_CTLR_DATA_WIDTH_8,
        .data_io = {PIN_D0, PIN_D1, PIN_D2, PIN_D3, PIN_D4, PIN_D5, PIN_D6, PIN_D7},
        .vsync_io = PIN_VSYNC,
        .de_io = PIN_HREF,
        .pclk_io = PIN_PCLK,
        .xclk_io = GPIO_NUM_NC,
    };
    esp_cam_ctlr_dvp_config_t cfg = {
        .ctlr_id = 0,
        .clk_src = CAM_CLK_SRC_DEFAULT,
        .h_res = s_stride,
        .v_res = height,
        .input_data_color_type = CAM_CTLR_COLOR_GRAY8,
        .output_data_color_type = CAM_CTLR_COLOR_GRAY8,
        .cam_data_width = 8,
        .dma_burst_size = 64,
        .pin = &pin,
        .bk_buffer_dis = 1,
        .external_xtal = 1,
    };

    esp_err_t err = esp_cam_new_dvp_ctlr(&cfg, &s_cam);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_cam_new_dvp_ctlr failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_lock);
        return err;
    }

    size_t fb_len = 0;
    err = esp_cam_ctlr_get_frame_buffer_len(s_cam, &fb_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get frame buffer len failed");
        esp_cam_ctlr_del(s_cam);
        s_cam = NULL;
        xSemaphoreGive(s_lock);
        return err;
    }
    ESP_LOGI(TAG, "DVP %ux%u stride=%u dark=%u frame_bytes=%u dma_len=%u", width, height,
             (unsigned)s_stride, s_dark_cols, (unsigned)s_frame_bytes, (unsigned)fb_len);

    xQueueReset(s_ready_q);
    s_free_head = 0;
    s_free_tail = 0;
    for (int i = 0; i < s_nbuf; i++) {
        free_push(i);
    }
    portENTER_CRITICAL(&s_stream_mux);
    for (int i = 0; i < STREAM_SLOTS; i++) {
        if (!s_slots[i].busy) {
            s_slots[i].ready = false;
        }
    }
    s_stream_last_us = 0;
    portEXIT_CRITICAL(&s_stream_mux);
    ESP_LOGI(TAG, "free ring after fill: head=%u tail=%u nbuf=%d", s_free_head, s_free_tail, s_nbuf);

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = on_get_new_trans,
        .on_trans_finished = on_trans_finished,
    };
    err = esp_cam_ctlr_register_event_callbacks(s_cam, &cbs, NULL);
    if (err == ESP_OK) err = esp_cam_ctlr_enable(s_cam);
    if (err == ESP_OK) err = esp_cam_ctlr_start(s_cam);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DVP start failed: %s", esp_err_to_name(err));
        esp_cam_ctlr_del(s_cam);
        s_cam = NULL;
        xSemaphoreGive(s_lock);
        return err;
    }

    s_running = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "capture started");
    return ESP_OK;
}

esp_err_t mt9v_capture_stop(void)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_cam) {
        esp_cam_ctlr_stop(s_cam);
        esp_cam_ctlr_disable(s_cam);
        esp_cam_ctlr_del(s_cam);
        s_cam = NULL;
    }
    s_running = false;
    portENTER_CRITICAL(&s_stream_mux);
    for (int i = 0; i < STREAM_SLOTS; i++) {
        if (!s_slots[i].busy) {
            s_slots[i].ready = false;
        }
    }
    portEXIT_CRITICAL(&s_stream_mux);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "capture stopped");
    return ESP_OK;
}

bool mt9v_capture_running(void)
{
    return s_running;
}

void mt9v_capture_reset_stats(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_dma_frames = 0;
    s_app_frames = 0;
    s_drops = 0;
    s_cb_calls = 0;
    s_cb_fail = 0;
    s_size_errors = 0;
    s_crc_frames = 0;
    s_crc_last = 0;
    s_crc_diff = 0;
    s_all_zero = 0;
    s_all_ff = 0;
    s_proc_sum_us = 0;
    s_proc_max_us = 0;
    s_proc_wait_sum_us = 0;
    s_proc_work_sum_us = 0;
    s_proc_count = 0;
    portENTER_CRITICAL(&s_stats_mux);
    s_vsy_count = 0;
    s_href_count = 0;
    s_vsy_head = 0;
    portEXIT_CRITICAL(&s_stats_mux);
    xSemaphoreGive(s_lock);
}

void mt9v_capture_get_stats(mt9v_capture_stats_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    out->width = s_width;
    out->height = s_height;
    out->frame_bytes = (uint32_t)s_frame_bytes;
    out->running = s_running;
    out->vsy_edges = s_vsy_count;
    out->href_edges = s_href_count;
    out->dma_frames = s_dma_frames;
    out->app_frames = s_app_frames;
    out->drops = s_drops;
    out->cb_calls = s_cb_calls;
    out->cb_fail = s_cb_fail;
    out->free_head = s_free_head;
    out->free_tail = s_free_tail;
    out->size_errors = s_size_errors;
    out->crc_frames = s_crc_frames;
    out->crc_last = s_crc_last;
    out->crc_diff = s_crc_diff;
    out->all_zero_frames = s_all_zero;
    out->all_ff_frames = s_all_ff;
    out->proc_avg_us = s_proc_count ? s_proc_sum_us / s_proc_count : 0;
    out->proc_max_us = s_proc_max_us;
    out->proc_wait_avg_us = s_proc_count ? s_proc_wait_sum_us / s_proc_count : 0;
    out->proc_work_avg_us = s_proc_count ? s_proc_work_sum_us / s_proc_count : 0;
    memcpy(out->sample, s_sample, sizeof(out->sample));

    uint32_t head = s_vsy_head;
    uint32_t n = head < 1000 ? head : 1000;
    if (n >= 2) {
        uint32_t first = s_vsy_ts[(head - n) & (VSY_RING - 1)];
        uint32_t last = s_vsy_ts[(head - 1) & (VSY_RING - 1)];
        uint32_t prev = first;
        uint32_t sum = 0;
        uint32_t mn = UINT32_MAX;
        uint32_t mx = 0;
        for (uint32_t i = 1; i < n; i++) {
            uint32_t t = s_vsy_ts[(head - n + i) & (VSY_RING - 1)];
            uint32_t d = t - prev;
            prev = t;
            sum += d;
            if (d < mn) mn = d;
            if (d > mx) mx = d;
        }
        uint32_t span = last - first;
        if (span) {
            out->vsy_fps = (double)(n - 1) * 1e6 / (double)span;
        }
        out->vsy_interval_avg_us = (double)sum / (double)(n - 1);
        out->vsy_interval_min_us = mn;
        out->vsy_interval_max_us = mx;
    }

    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

void mt9v_capture_set_crc_enabled(bool enable)
{
    s_crc_enabled = enable;
}

void mt9v_capture_set_href_enabled(bool enable)
{
    if (enable) {
        gpio_intr_enable(PIN_HREF);
    } else {
        gpio_intr_disable(PIN_HREF);
    }
}

esp_err_t mt9v_capture_request_dump(void)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    s_dump_ready = false;
    s_dump_request = true;
    return ESP_OK;
}

bool mt9v_capture_dump_ready(void)
{
    return s_dump_ready;
}

const uint8_t *mt9v_capture_get_dump(size_t *len)
{
    if (!s_dump_ready) {
        return NULL;
    }
    if (len) {
        *len = s_dump_len;
    }
    return s_dump_buf;
}

void mt9v_capture_stream_enable(bool enable)
{
    s_stream_enabled = enable;
    if (!enable) {
        portENTER_CRITICAL(&s_stream_mux);
        for (int i = 0; i < STREAM_SLOTS; i++) {
            if (!s_slots[i].busy) {
                s_slots[i].ready = false;
            }
        }
        portEXIT_CRITICAL(&s_stream_mux);
    }
}

void mt9v_capture_stream_config(uint16_t div, uint16_t target_fps, bool pack4)
{
    if (div == 0) div = 1;
    if (div > 8) div = 8;
    s_stream_div = div;
    s_stream_target_fps = target_fps;
    s_stream_pack4 = pack4;
}

void mt9v_capture_stream_get_config(bool *enabled, uint16_t *div, uint16_t *target_fps, bool *pack4)
{
    if (enabled) *enabled = s_stream_enabled;
    if (div) *div = s_stream_div;
    if (target_fps) *target_fps = s_stream_target_fps;
    if (pack4) *pack4 = s_stream_pack4;
}

bool mt9v_capture_stream_acquire(mt9v_stream_frame_t *frame)
{
    if (!frame) {
        return false;
    }

    int best = -1;
    portENTER_CRITICAL(&s_stream_mux);
    for (int i = 0; i < STREAM_SLOTS; i++) {
        if (s_slots[i].ready) {
            if (best < 0 || (int32_t)(s_slots[i].seq - s_slots[best].seq) > 0) {
                best = i;
            }
        }
    }
    if (best >= 0) {
        for (int i = 0; i < STREAM_SLOTS; i++) {
            if (i != best && s_slots[i].ready && !s_slots[i].busy) {
                s_slots[i].ready = false;
            }
        }
        s_slots[best].ready = false;
        s_slots[best].busy = true;
        frame->data = s_slots[best].buf;
        frame->width = s_slots[best].width;
        frame->height = s_slots[best].height;
        frame->seq = s_slots[best].seq;
        frame->timestamp_ms = s_slots[best].timestamp_ms;
        frame->bytes = s_slots[best].bytes;
        frame->pack4 = s_slots[best].pack4;
        frame->slot = best;
    }
    portEXIT_CRITICAL(&s_stream_mux);
    return best >= 0;
}

void mt9v_capture_stream_release(const mt9v_stream_frame_t *frame)
{
    if (!frame || frame->slot < 0 || frame->slot >= STREAM_SLOTS) {
        return;
    }
    portENTER_CRITICAL(&s_stream_mux);
    s_slots[frame->slot].busy = false;
    s_stream_sent = s_stream_sent + 1;
    portEXIT_CRITICAL(&s_stream_mux);
}

void mt9v_capture_stream_get_stats(uint32_t *published, uint32_t *dropped, uint32_t *sent)
{
    if (published) *published = s_stream_pub;
    if (dropped) *dropped = s_stream_drop;
    if (sent) *sent = s_stream_sent;
}

void mt9v_capture_stream_reset_stats(void)
{
    s_stream_pub = 0;
    s_stream_drop = 0;
    s_stream_sent = 0;
}

const uint8_t *mt9v_capture_last_frame(size_t *len)
{
    (void)len;
    return NULL;
}
