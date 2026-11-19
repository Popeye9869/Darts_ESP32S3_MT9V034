#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "netinet/tcp.h"
#include "camera_control.h"
#include "mt9v_capture.h"

static const char *TAG = "web";

#define STREAM_MAX_BYTES (752 * 480)
#define STREAM_HDR_LEN 16

typedef struct __attribute__((packed)) {
    uint8_t magic[2];      /* 'M', 'T' */
    uint8_t version;
    uint8_t flags;
    uint16_t width;
    uint16_t height;
    uint32_t seq;
    uint32_t timestamp_ms;
} stream_hdr_t;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_server;
static SemaphoreHandle_t s_ws_lock;
static volatile int s_ws_fd = -1;
static uint8_t *s_tx_buf;
static char s_status_json[640] =
    "{\"type\":\"status\",\"mode\":\"--\",\"w\":0,\"h\":0,\"col\":0,\"row\":0,"
    "\"hb\":0,\"vb\":0,\"rb\":0,"
    "\"exp\":0,\"gain\":0,\"running\":0,\"crc\":0,\"rn\":0,\"vsy\":0,\"dma\":0,\"app\":0,"
    "\"drops\":0,\"proc\":0,\"stream\":0,\"div\":1,\"tfps\":0,\"pack\":4,"
    "\"sfps\":0,\"tx\":0,\"txb\":0,\"pub\":0,\"sdropped\":0,\"rssi\":0,\"heap\":0}";
static char s_ip_str[20] = "0.0.0.0";
static EventGroupHandle_t s_wifi_events;
#define WIFI_GOT_IP_BIT BIT0
static volatile bool s_allow_connect;

static uint32_t s_last_status_us;
static uint32_t s_last_vsy;
static uint32_t s_last_dma;
static uint32_t s_last_app;
static uint32_t s_last_tx;
static uint32_t s_tx_frames;
static uint32_t s_tx_bytes;

static int ws_send_text(int fd, const char *text)
{
    if (fd < 0 || !text) {
        return -1;
    }
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)text,
        .len = strlen(text),
    };
    if (xSemaphoreTake(s_ws_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return -1;
    }
    esp_err_t err = httpd_ws_send_frame_async(s_server, fd, &frame);
    xSemaphoreGive(s_ws_lock);
    return err == ESP_OK ? 0 : -1;
}

static void ws_reply(httpd_req_t *req, const char *text)
{
    if (!req || !text) return;
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)text,
        .len = strlen(text),
    };
    if (xSemaphoreTake(s_ws_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        httpd_ws_send_frame(req, &frame);
        xSemaphoreGive(s_ws_lock);
    }
}

static int ap_rssi(void)
{
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

static void update_status(void)
{
    uint32_t now = (uint32_t)esp_timer_get_time();
    if (s_last_status_us && (now - s_last_status_us) < 500000) {
        return;
    }
    uint32_t dt = s_last_status_us ? (now - s_last_status_us) : 500000;
    s_last_status_us = now;

    camera_state_t cs;
    camera_get_state(&cs);
    mt9v_capture_stats_t st;
    mt9v_capture_get_stats(&st);

    uint32_t dv = (st.vsy_edges >= s_last_vsy) ? st.vsy_edges - s_last_vsy : 0;
    uint32_t dd = (st.dma_frames >= s_last_dma) ? st.dma_frames - s_last_dma : 0;
    uint32_t da = (st.app_frames >= s_last_app) ? st.app_frames - s_last_app : 0;
    uint32_t dt_frames = (s_tx_frames >= s_last_tx) ? s_tx_frames - s_last_tx : 0;
    s_last_vsy = st.vsy_edges;
    s_last_dma = st.dma_frames;
    s_last_app = st.app_frames;
    s_last_tx = s_tx_frames;

    double sec = (double)dt / 1e6;
    double vsy = dv / sec;
    double dma = dd / sec;
    double app = da / sec;
    double sfps = dt_frames / sec;

    uint32_t pub = 0, sdrop = 0, sent = 0;
    mt9v_capture_stream_get_stats(&pub, &sdrop, &sent);

    snprintf(s_status_json, sizeof(s_status_json),
             "{\"type\":\"status\",\"mode\":\"%s\",\"w\":%u,\"h\":%u,"
             "\"col\":%u,\"row\":%u,\"hb\":%u,\"vb\":%u,\"rb\":%u,\"exp\":%u,\"gain\":%u,"
             "\"running\":%d,\"crc\":%d,\"rn\":%d,\"vsy\":%.2f,\"dma\":%.2f,\"app\":%.2f,"
             "\"drops\":%lu,\"proc\":%lu,\"stream\":%d,\"div\":%u,\"tfps\":%u,\"pack\":%d,"
             "\"sfps\":%.2f,\"tx\":%lu,\"txb\":%lu,\"pub\":%lu,\"sdropped\":%lu,"
             "\"rssi\":%d,\"heap\":%u}",
             cs.mode.name ? cs.mode.name : "?",
             (unsigned)cs.mode.width, (unsigned)cs.mode.height,
             cs.mode.col_start, cs.mode.row_start,
             cs.mode.hblank, cs.mode.vblank, cs.mode.row_bin,
             cs.mode.exposure, cs.mode.gain,
             cs.running, cs.crc, cs.row_noise, vsy, dma, app,
             (unsigned long)st.drops, (unsigned long)st.proc_avg_us,
             cs.stream_enabled, cs.stream_div, cs.stream_fps,
             cs.stream_pack4 ? 4 : 8,
             sfps, (unsigned long)s_tx_frames, (unsigned long)s_tx_bytes,
             (unsigned long)pub, (unsigned long)sdrop, ap_rssi(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (s_ws_fd >= 0) {
        if (ws_send_text(s_ws_fd, s_status_json) != 0) {
            s_ws_fd = -1;
        }
    }
}

static void stream_task(void *arg)
{
    (void)arg;
    while (1) {
        update_status();

        int fd = s_ws_fd;
        bool enabled = false;
        uint16_t div = 1, fps = 0;
        bool pack4 = true;
        mt9v_capture_stream_get_config(&enabled, &div, &fps, &pack4);

        if (fd < 0 || !enabled) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        mt9v_stream_frame_t frame;
        if (!mt9v_capture_stream_acquire(&frame)) {
            vTaskDelay(1);
            continue;
        }

        size_t pixels = frame.bytes;
        if (16 + pixels > STREAM_MAX_BYTES + STREAM_HDR_LEN) {
            mt9v_capture_stream_release(&frame);
            continue;
        }

        stream_hdr_t hdr = {
            .magic = {'M', 'T'},
            .version = 1,
            .flags = frame.pack4 ? 1 : 0,
            .width = frame.width,
            .height = frame.height,
            .seq = frame.seq,
            .timestamp_ms = frame.timestamp_ms,
        };
        memcpy(s_tx_buf, &hdr, sizeof(hdr));
        memcpy(s_tx_buf + sizeof(hdr), frame.data, pixels);

        httpd_ws_frame_t ws = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_BINARY,
            .payload = s_tx_buf,
            .len = sizeof(hdr) + pixels,
        };
        esp_err_t err = ESP_FAIL;
        if (xSemaphoreTake(s_ws_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
            err = httpd_ws_send_frame_async(s_server, fd, &ws);
            xSemaphoreGive(s_ws_lock);
        }
        mt9v_capture_stream_release(&frame);

        if (err != ESP_OK) {
            s_ws_fd = -1;
            vTaskDelay(pdMS_TO_TICKS(20));
        } else {
            s_tx_frames++;
            s_tx_bytes += sizeof(hdr) + pixels;
        }
    }
}

static int parse_hex_arg(const char *s)
{
    if (!s) return -1;
    return (int)strtoul(s, NULL, 16);
}

static void handle_command(httpd_req_t *req, char *line)
{
    char *save = NULL;
    char *cmd = strtok_r(line, " \t\r\n", &save);
    if (!cmd) return;

    char reply[256];
    if (strcmp(cmd, "hello") == 0 || strcmp(cmd, "status") == 0) {
        ws_reply(req, s_status_json[0] ? s_status_json : "{\"type\":\"status\"}");
        return;
    }

    if (strcmp(cmd, "mode") == 0) {
        char *name = strtok_r(NULL, " \t\r\n", &save);
        if (!name) {
            ws_reply(req, "{\"type\":\"ack\",\"ok\":false,\"msg\":\"mode name missing\"}");
            return;
        }
        esp_err_t err = camera_set_preset(name);
        snprintf(reply, sizeof(reply), "{\"type\":\"ack\",\"ok\":%s,\"cmd\":\"mode\",\"msg\":\"%s\"}",
                 err == ESP_OK ? "true" : "false", esp_err_to_name(err));
        ws_reply(req, reply);
        return;
    }

    if (strcmp(cmd, "apply") == 0) {
        int v[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
        for (int i = 0; i < 9; i++) {
            char *tok = strtok_r(NULL, " \t\r\n", &save);
            if (!tok) break;
            v[i] = atoi(tok);
        }
        esp_err_t err = camera_apply_fields(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
        snprintf(reply, sizeof(reply), "{\"type\":\"ack\",\"ok\":%s,\"cmd\":\"apply\",\"msg\":\"%s\"}",
                 err == ESP_OK ? "true" : "false", esp_err_to_name(err));
        ws_reply(req, reply);
        return;
    }

    if (strcmp(cmd, "start") == 0 || strcmp(cmd, "stop") == 0) {
        esp_err_t err = (cmd[0] == 's' && cmd[1] == 't' && cmd[2] == 'a')
                            ? camera_start() : camera_stop();
        snprintf(reply, sizeof(reply), "{\"type\":\"ack\",\"ok\":%s,\"cmd\":\"%s\"}",
                 err == ESP_OK ? "true" : "false", cmd);
        ws_reply(req, reply);
        return;
    }

    if (strcmp(cmd, "rownoise") == 0) {
        char *v = strtok_r(NULL, " \t\r\n", &save);
        bool en = v && strcmp(v, "on") == 0;
        esp_err_t err = camera_set_row_noise(en);
        snprintf(reply, sizeof(reply),
                 "{\"type\":\"ack\",\"ok\":%s,\"cmd\":\"rownoise\",\"on\":%d,\"msg\":\"%s\"}",
                 err == ESP_OK ? "true" : "false", en, esp_err_to_name(err));
        ws_reply(req, reply);
        return;
    }

    if (strcmp(cmd, "crc") == 0) {
        char *v = strtok_r(NULL, " \t\r\n", &save);
        bool en = v && strcmp(v, "on") == 0;
        camera_set_crc(en);
        snprintf(reply, sizeof(reply), "{\"type\":\"ack\",\"ok\":true,\"cmd\":\"crc\",\"on\":%d}", en);
        ws_reply(req, reply);
        return;
    }

    if (strcmp(cmd, "stream") == 0) {
        char *a1 = strtok_r(NULL, " \t\r\n", &save);
        bool en = false;
        uint16_t div = 1, fps = 0;
        bool pack4 = true;
        mt9v_capture_stream_get_config(&en, &div, &fps, &pack4);
        if (!a1) {
            ws_reply(req, "{\"type\":\"ack\",\"ok\":false,\"msg\":\"stream arg missing\"}");
            return;
        }
        if (strcmp(a1, "on") == 0) {
            en = true;
        } else if (strcmp(a1, "off") == 0) {
            en = false;
        } else if (strcmp(a1, "div") == 0) {
            char *v = strtok_r(NULL, " \t\r\n", &save);
            div = (uint16_t)atoi(v ? v : "1");
        } else if (strcmp(a1, "fps") == 0) {
            char *v = strtok_r(NULL, " \t\r\n", &save);
            fps = (uint16_t)atoi(v ? v : "0");
        } else if (strcmp(a1, "pack") == 0) {
            char *v = strtok_r(NULL, " \t\r\n", &save);
            pack4 = !v || atoi(v) == 4;
        } else {
            ws_reply(req, "{\"type\":\"ack\",\"ok\":false,\"msg\":\"stream on|off|div|fps|pack\"}");
            return;
        }
        camera_set_stream(en, div, fps, pack4);
        snprintf(reply, sizeof(reply),
                 "{\"type\":\"ack\",\"ok\":true,\"cmd\":\"stream\",\"on\":%d,\"div\":%u,\"fps\":%u,\"pack\":%d}",
                 en, div, fps, pack4 ? 4 : 8);
        ws_reply(req, reply);
        return;
    }

    if (strcmp(cmd, "reg") == 0) {
        int reg = parse_hex_arg(strtok_r(NULL, " \t\r\n", &save));
        uint16_t val = 0;
        esp_err_t err = (reg < 0) ? ESP_ERR_INVALID_ARG : camera_read_reg((uint16_t)reg, &val);
        snprintf(reply, sizeof(reply),
                 "{\"type\":\"ack\",\"ok\":%s,\"cmd\":\"reg\",\"reg\":%d,\"val\":%u}",
                 err == ESP_OK ? "true" : "false", reg, val);
        ws_reply(req, reply);
        return;
    }

    if (strcmp(cmd, "setreg") == 0) {
        int reg = parse_hex_arg(strtok_r(NULL, " \t\r\n", &save));
        int val = parse_hex_arg(strtok_r(NULL, " \t\r\n", &save));
        esp_err_t err = (reg < 0 || val < 0) ? ESP_ERR_INVALID_ARG
                                             : camera_write_reg((uint16_t)reg, (uint16_t)val);
        snprintf(reply, sizeof(reply), "{\"type\":\"ack\",\"ok\":%s,\"cmd\":\"setreg\"}",
                 err == ESP_OK ? "true" : "false");
        ws_reply(req, reply);
        return;
    }

    if (strcmp(cmd, "href") == 0) {
        char *v = strtok_r(NULL, " \t\r\n", &save);
        int ms = v ? atoi(v) : 1000;
        if (ms < 100) ms = 100;
        mt9v_capture_set_href_enabled(true);
        mt9v_capture_reset_stats();
        vTaskDelay(pdMS_TO_TICKS(ms));
        mt9v_capture_stats_t st;
        mt9v_capture_get_stats(&st);
        mt9v_capture_set_href_enabled(false);
        snprintf(reply, sizeof(reply),
                 "{\"type\":\"href\",\"edges\":%lu,\"vsy\":%lu,\"fps\":%.2f,\"lines\":%.2f}",
                 (unsigned long)st.href_edges, (unsigned long)st.vsy_edges,
                 st.vsy_edges * 1000.0 / ms,
                 st.vsy_edges ? (double)st.href_edges / (double)st.vsy_edges : 0.0);
        ws_reply(req, reply);
        return;
    }

    if (strcmp(cmd, "presets") == 0) {
        char out[768];
        size_t n = snprintf(out, sizeof(out), "{\"type\":\"presets\",\"list\":[");
        for (size_t i = 0; i < camera_preset_count() && n < sizeof(out) - 32; i++) {
            const mt9v034_mode_t *m = camera_preset_at(i);
            n += snprintf(out + n, sizeof(out) - n, "%s\"%s\"", i ? "," : "", m->name);
        }
        snprintf(out + n, sizeof(out) - n, "]}");
        ws_reply(req, out);
        return;
    }

    ws_reply(req, "{\"type\":\"ack\",\"ok\":false,\"msg\":\"unknown command\"}");
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        s_ws_fd = fd;
        s_tx_frames = 0;
        s_tx_bytes = 0;
        s_last_tx = 0;
        int one = 1;
        int sndbuf = 32768;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        ESP_LOGI(TAG, "WebSocket client fd=%d", fd);
        return ESP_OK;
    }

    httpd_ws_frame_t ws = {0};
    esp_err_t ret = httpd_ws_recv_frame(req, &ws, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    if (ws.type == HTTPD_WS_TYPE_CLOSE) {
        if (s_ws_fd == fd) s_ws_fd = -1;
        return ESP_OK;
    }
    if (ws.len == 0) {
        return ESP_OK;
    }

    char buf[320];
    size_t n = ws.len < sizeof(buf) - 1 ? ws.len : sizeof(buf) - 1;
    ws.payload = (uint8_t *)buf;
    ret = httpd_ws_recv_frame(req, &ws, n);
    if (ret != ESP_OK) {
        return ret;
    }
    buf[n] = '\0';
    if (ws.type == HTTPD_WS_TYPE_TEXT) {
        s_ws_fd = fd;
        handle_command(req, buf);
    }
    return ESP_OK;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t speed_handler(httpd_req_t *req)
{
    static uint8_t chunk[4096];
    memset(chunk, 0x5A, sizeof(chunk));
    httpd_resp_set_type(req, "application/octet-stream");
    for (int i = 0; i < 256; i++) {
        if (httpd_resp_send_chunk(req, (const char *)chunk, sizeof(chunk)) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_allow_connect) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "STA disconnected, reconnecting...");
        if (s_allow_connect) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "STA got IP %s", s_ip_str);
        xEventGroupSetBits(s_wifi_events, WIFI_GOT_IP_BIT);
    }
}

static void wifi_sta_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    wifi_country_t country = {
        .cc = "CN",
        .schan = 1,
        .nchan = 13,
        .max_tx_power = 20,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    esp_wifi_set_country(&country);

    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, WEB_STA_SSID, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, WEB_STA_PASS, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    /* At -30 dBm the link should run at a high MCS.  Some 2.4 GHz APs leave
     * rate control at a legacy 1 Mbps rate; pin MCS7/HT20 for the test. */
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11G);
    esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_54M);
    s_allow_connect = false;
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);

    /* Scan for all BNGU BSSIDs and pin the best 2.4 GHz one.  A router often
     * exposes several mesh nodes under the same SSID; the ESP32 may otherwise
     * associate with a congested/slow one. */
    wifi_scan_config_t scan_cfg = {
        .ssid = (uint8_t *)WEB_STA_SSID,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
    };
    if (esp_wifi_scan_start(&scan_cfg, true) == ESP_OK) {
        uint16_t num = 16;
        wifi_ap_record_t records[16];
        if (esp_wifi_scan_get_ap_records(&num, records) == ESP_OK) {
            int best = -1;
            for (int i = 0; i < num; i++) {
                ESP_LOGI(TAG, "AP %02x:%02x:%02x:%02x:%02x:%02x ch=%u rssi=%d auth=%d",
                         records[i].bssid[0], records[i].bssid[1], records[i].bssid[2],
                         records[i].bssid[3], records[i].bssid[4], records[i].bssid[5],
                         records[i].primary, records[i].rssi, records[i].authmode);
                if (best < 0 || records[i].rssi > records[best].rssi) {
                    best = i;
                }
            }
            if (best >= 0) {
                cfg.sta.bssid_set = true;
                memcpy(cfg.sta.bssid, records[best].bssid, 6);
                esp_wifi_set_config(WIFI_IF_STA, &cfg);
                ESP_LOGI(TAG, "pinned BSSID %02x:%02x:%02x:%02x:%02x:%02x rssi=%d",
                         records[best].bssid[0], records[best].bssid[1],
                         records[best].bssid[2], records[best].bssid[3],
                         records[best].bssid[4], records[best].bssid[5],
                         records[best].rssi);
            }
        }
        esp_wifi_clear_ap_list();
    }

    s_allow_connect = true;
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_GOT_IP_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (bits & WIFI_GOT_IP_BIT) {
        wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
        wifi_bandwidth_t bw = WIFI_BW20;
        wifi_phy_mode_t phy = WIFI_PHY_MODE_11B;
        esp_wifi_get_ps(&ps);
        esp_wifi_get_bandwidth(WIFI_IF_STA, &bw);
        esp_wifi_sta_get_negotiated_phymode(&phy);
        ESP_LOGI(TAG, "STA connected to %s, web UI at http://%s/ (ps=%d bw=%d phy=%d)",
                 WEB_STA_SSID, s_ip_str, ps, bw, phy);
    } else {
        ESP_LOGE(TAG, "STA connect to %s timed out (check 2.4 GHz SSID/password)", WEB_STA_SSID);
    }
}

const char *web_server_ip(void)
{
    return s_ip_str;
}

esp_err_t web_server_start(void)
{
    s_ws_lock = xSemaphoreCreateMutex();
    if (!s_ws_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_tx_buf = heap_caps_malloc(STREAM_MAX_BYTES + STREAM_HDR_LEN,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tx_buf) {
        ESP_LOGE(TAG, "stream tx buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    wifi_sta_start();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = 0;
    config.task_priority = 5;
    config.stack_size = 8192;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return ESP_FAIL;
    }

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t favicon = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = favicon_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = true,
    };
    httpd_uri_t speed = {
        .uri = "/speed",
        .method = HTTP_GET,
        .handler = speed_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &favicon);
    httpd_register_uri_handler(s_server, &speed);
    httpd_register_uri_handler(s_server, &ws);

    BaseType_t ok = xTaskCreatePinnedToCore(stream_task, "web_stream", 8192, NULL, 6, NULL, 0);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "web server ready at http://%s/", web_server_ip());
    return ESP_OK;
}
