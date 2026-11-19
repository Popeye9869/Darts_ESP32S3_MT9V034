#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_console.h"
#include "driver/gpio.h"
#include "camera_control.h"
#include "mt9v_capture.h"
#include "web_server.h"

static const char *TAG = "mt9v034";

static int parse_int(const char *s, int def)
{
    if (!s) return def;
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (end == s) return def;
    return (int)v;
}

static int parse_hex(const char *s, int def)
{
    if (!s) return def;
    char *end = NULL;
    long v = strtoul(s, &end, 16);
    if (end == s) return def;
    return (int)v;
}

static int cmd_probe(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint16_t version = 0;
    esp_err_t err = camera_read_reg(0, &version);
    printf("PROBE chip_version=0x%04X (%s)\n", version, esp_err_to_name(err));
    mt9v034_print_mode("MODE", camera_mode());
    return err == ESP_OK ? 0 : 1;
}

static int cmd_reg(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: reg <reg_hex>\n");
        return 1;
    }
    uint16_t reg = (uint16_t)parse_hex(argv[1], -1);
    uint16_t val = 0;
    esp_err_t err = camera_read_reg(reg, &val);
    printf("REG 0x%04X = 0x%04X (%s)\n", reg, val, esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_set(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: set <reg_hex> <value_hex>\n");
        return 1;
    }
    uint16_t reg = (uint16_t)parse_hex(argv[1], -1);
    uint16_t val = (uint16_t)parse_hex(argv[2], -1);
    esp_err_t err = camera_write_reg(reg, val);
    uint16_t rb = 0;
    if (err == ESP_OK) err = camera_read_reg(reg, &rb);
    printf("SET 0x%04X <- 0x%04X readback=0x%04X (%s)\n",
           reg, val, rb, esp_err_to_name(err));
    return err == ESP_OK && rb == val ? 0 : 1;
}

static int cmd_mode(int argc, char **argv)
{
    if (argc < 2) {
        printf("modes:");
        for (size_t i = 0; i < camera_preset_count(); i++) {
            printf(" %s", camera_preset_at(i)->name);
        }
        printf(" read\n");
        return 0;
    }
    if (strcmp(argv[1], "read") == 0) {
        mt9v034_print_mode("MODE", camera_mode());
        return 0;
    }
    esp_err_t err = camera_set_preset(argv[1]);
    if (err != ESP_OK) {
        printf("MODE apply failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    mt9v034_print_mode("MODE", camera_mode());
    return 0;
}

static int cmd_capstart(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t err = camera_start();
    if (err != ESP_OK) {
        printf("CAPSTART failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("CAPSTART %ux%u\n", camera_mode()->width, camera_mode()->height);
    return 0;
}

static int cmd_capstop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    camera_stop();
    printf("CAPSTOP\n");
    return 0;
}

static int cmd_stream(int argc, char **argv)
{
    bool en = false;
    uint16_t div = 1, fps = 0;
    bool pack4 = true;
    mt9v_capture_stream_get_config(&en, &div, &fps, &pack4);
    if (argc < 2) {
        printf("STREAM enabled=%d div=%u fps=%u pack=%d\n", en, div, fps, pack4 ? 4 : 8);
        return 0;
    }
    if (strcmp(argv[1], "on") == 0) {
        en = true;
    } else if (strcmp(argv[1], "off") == 0) {
        en = false;
    } else if (strcmp(argv[1], "div") == 0 && argc >= 3) {
        div = (uint16_t)parse_int(argv[2], div);
    } else if (strcmp(argv[1], "fps") == 0 && argc >= 3) {
        fps = (uint16_t)parse_int(argv[2], fps);
    } else if (strcmp(argv[1], "pack") == 0 && argc >= 3) {
        pack4 = parse_int(argv[2], 4) == 4;
    } else {
        printf("usage: stream [on|off] | stream div N | stream fps N | stream pack 4|8\n");
        return 1;
    }
    camera_set_stream(en, div, fps, pack4);
    printf("STREAM enabled=%d div=%u fps=%u pack=%d\n", en, div, fps, pack4 ? 4 : 8);
    return 0;
}

static int cmd_bench(int argc, char **argv)
{
    int seconds = argc > 1 ? parse_int(argv[1], 5) : 5;
    if (seconds < 1) seconds = 1;
    if (!camera_is_running()) {
        printf("CAP not running\n");
        return 1;
    }

    mt9v_capture_reset_stats();
    vTaskDelay(pdMS_TO_TICKS(100));
    mt9v_capture_reset_stats();
    int64_t t0 = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS((uint32_t)seconds * 1000));
    int64_t t1 = esp_timer_get_time();

    mt9v_capture_stats_t st;
    mt9v_capture_get_stats(&st);
    const mt9v034_mode_t *m = camera_mode();
    double sec = (double)(t1 - t0) / 1e6;
    double vsy_fps = st.vsy_edges / sec;
    double dma_fps = st.dma_frames / sec;
    double app_fps = st.app_frames / sec;
    double calc_fps = 27000000.0 / (((double)(st.height + m->vblank) *
                                     (double)(st.width + m->hblank)) + 4.0);
    double vsy_ring = st.vsy_interval_avg_us > 0.0 ? 1e6 / st.vsy_interval_avg_us : 0.0;

    printf("RESULT mode=%s w=%lu h=%lu hblank=%u vblank=%u row_bin=%u exp=%u "
           "vsy_fps=%.3f vsy_ring=%.3f dma_fps=%.3f app_fps=%.3f calc_fps=%.3f "
           "vsy_edges=%lu dma_frames=%lu app_frames=%lu drops=%lu crc_frames=%lu crc_diff=%lu "
           "jitter_us=%.1f/%.1f ms=%.1f\n",
           m->name ? m->name : "?", (unsigned long)st.width, (unsigned long)st.height,
           m->hblank, m->vblank, m->row_bin, m->exposure,
           vsy_fps, vsy_ring, dma_fps, app_fps, calc_fps,
           (unsigned long)st.vsy_edges, (unsigned long)st.dma_frames,
           (unsigned long)st.app_frames, (unsigned long)st.drops,
           (unsigned long)st.crc_frames, (unsigned long)st.crc_diff,
           st.vsy_interval_min_us, st.vsy_interval_max_us, sec * 1000.0);
    return 0;
}

static int cmd_capstat(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    mt9v_capture_stats_t st;
    mt9v_capture_get_stats(&st);
    uint32_t pub = 0, drop = 0, sent = 0;
    mt9v_capture_stream_get_stats(&pub, &drop, &sent);
    printf("CAPSTAT run=%d w=%lu h=%lu vsy=%lu href=%lu dma=%lu app=%lu drops=%lu "
           "crc=%lu diff=%lu zero=%lu ff=%lu stream_pub=%lu stream_drop=%lu stream_sent=%lu\n",
           st.running, (unsigned long)st.width, (unsigned long)st.height,
           (unsigned long)st.vsy_edges, (unsigned long)st.href_edges,
           (unsigned long)st.dma_frames, (unsigned long)st.app_frames,
           (unsigned long)st.drops, (unsigned long)st.crc_frames, (unsigned long)st.crc_diff,
           (unsigned long)st.all_zero_frames, (unsigned long)st.all_ff_frames,
           (unsigned long)pub, (unsigned long)drop, (unsigned long)sent);
    return 0;
}

static int cmd_signals(int argc, char **argv)
{
    int ms = argc > 1 ? parse_int(argv[1], 1000) : 1000;
    if (ms < 100) ms = 100;
    mt9v_capture_stats_t st;
    mt9v_capture_set_href_enabled(true);
    mt9v_capture_reset_stats();
    int64_t t0 = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(ms));
    int64_t t1 = esp_timer_get_time();
    mt9v_capture_get_stats(&st);
    mt9v_capture_set_href_enabled(false);
    double sec = (double)(t1 - t0) / 1e6;
    printf("SIGNALS ms=%d vsy=%lu href=%lu vsy_fps=%.3f href_hz=%.1f lines_per_frame=%.2f\n",
           ms, (unsigned long)st.vsy_edges, (unsigned long)st.href_edges,
           st.vsy_edges / sec, st.href_edges / sec,
           st.vsy_edges ? (double)st.href_edges / (double)st.vsy_edges : 0.0);
    return 0;
}

static int cmd_vsy(int argc, char **argv)
{
    int ms = argc > 1 ? parse_int(argv[1], 1000) : 1000;
    if (ms < 100) ms = 100;
    mt9v_capture_stats_t st;
    mt9v_capture_reset_stats();
    int64_t t0 = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(ms));
    int64_t t1 = esp_timer_get_time();
    mt9v_capture_get_stats(&st);
    double sec = (double)(t1 - t0) / 1e6;
    printf("VSY ms=%d edges=%lu fps=%.3f interval_avg=%.1f us min=%.1f max=%.1f\n",
           ms, (unsigned long)st.vsy_edges, st.vsy_edges / sec,
           st.vsy_interval_avg_us, st.vsy_interval_min_us, st.vsy_interval_max_us);
    return 0;
}

static int cmd_dump(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!camera_is_running()) {
        printf("CAP not running\n");
        return 1;
    }
    esp_err_t err = mt9v_capture_request_dump();
    if (err != ESP_OK) {
        printf("DUMP request failed\n");
        return 1;
    }
    for (int i = 0; i < 300 && !mt9v_capture_dump_ready(); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    size_t len = 0;
    const uint8_t *buf = mt9v_capture_get_dump(&len);
    if (!buf || !len) {
        printf("DUMP timeout\n");
        return 1;
    }
    printf("DUMP_BEGIN %lu %lu %lu\n",
           (unsigned long)camera_mode()->width,
           (unsigned long)camera_mode()->height, (unsigned long)len);
    fflush(stdout);
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
    printf("\nDUMP_END\n");
    fflush(stdout);
    return 0;
}

static int cmd_rownoise(int argc, char **argv)
{
    bool en = true;
    if (argc > 1 && strcmp(argv[1], "off") == 0) en = false;
    esp_err_t err = camera_set_row_noise(en);
    printf("ROWNOISE %s (%s)\n", en ? "on" : "off", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_crc(int argc, char **argv)
{
    bool en = true;
    if (argc > 1 && strcmp(argv[1], "off") == 0) en = false;
    camera_set_crc(en);
    printf("CRC %s\n", en ? "on" : "off");
    return 0;
}

static int cmd_pins(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint64_t mask = (1ULL << 43) | (1ULL << 44) | (1ULL << 6) | (1ULL << 7) | (1ULL << 13);
    gpio_dump_io_configuration(stdout, mask);
    printf("LEVEL 43=%d 44=%d 6=%d 7=%d 13=%d\n",
           gpio_get_level(43), gpio_get_level(44), gpio_get_level(6),
           gpio_get_level(7), gpio_get_level(13));
    return 0;
}

static int cmd_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    printf("IDF=%s chip_rev=%d cores=%d flash=%luMB psram=%dMB free_int=%u free_psram=%u\n",
           IDF_VER, chip.revision, chip.cores, (unsigned long)(flash_size / (1024 * 1024)),
           esp_psram_is_initialized() ? (int)(esp_psram_get_size() / (1024 * 1024)) : 0,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("WEB http://%s/  STA=%s / %s\n", web_server_ip(), WEB_STA_SSID, WEB_STA_PASS);
    mt9v034_print_mode("MODE", camera_mode());
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("rebooting\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
}

static void register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        {.command = "probe", .help = "show camera state", .func = cmd_probe},
        {.command = "reg", .help = "reg <reg_hex>", .func = cmd_reg},
        {.command = "set", .help = "set <reg_hex> <value_hex>", .func = cmd_set},
        {.command = "mode", .help = "mode <preset|read>", .func = cmd_mode},
        {.command = "capstart", .help = "start capture [w h]", .func = cmd_capstart},
        {.command = "capstop", .help = "stop capture", .func = cmd_capstop},
        {.command = "stream", .help = "stream [on|off] / div N / fps N", .func = cmd_stream},
        {.command = "bench", .help = "bench [seconds]", .func = cmd_bench},
        {.command = "capstat", .help = "capture statistics", .func = cmd_capstat},
        {.command = "signals", .help = "measure VSY/HREF", .func = cmd_signals},
        {.command = "vsy", .help = "vsy [ms]", .func = cmd_vsy},
        {.command = "crc", .help = "crc on|off", .func = cmd_crc},
        {.command = "rownoise", .help = "rownoise on|off", .func = cmd_rownoise},
        {.command = "dump", .help = "dump one raw frame", .func = cmd_dump},
        {.command = "pins", .help = "dump GPIO configuration", .func = cmd_pins},
        {.command = "info", .help = "show chip/network/mode", .func = cmd_info},
        {.command = "reboot", .help = "restart", .func = cmd_reboot},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "MT9V034 ESP32-S3 web debug console, IDF %s", IDF_VER);

    esp_err_t err = mt9v_capture_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "capture init failed: %s", esp_err_to_name(err));
    }
    err = camera_control_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "camera init: %s", esp_err_to_name(err));
    }

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "mt9v> ";
    repl_config.max_cmdline_length = 256;

    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
    register_commands();
    ESP_ERROR_CHECK(esp_console_register_help_command());

    err = web_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web server failed: %s", esp_err_to_name(err));
    }

    printf("\nMT9V034 web debug ready. STA %s / %s, http://%s/\n",
           WEB_STA_SSID, WEB_STA_PASS, web_server_ip());
    printf("USB console ready ('help' for commands).\n");
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
