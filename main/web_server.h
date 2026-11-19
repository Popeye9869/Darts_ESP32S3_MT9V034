#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_STA_SSID "BNGU"
#define WEB_STA_PASS "12345678"

esp_err_t web_server_start(void);
const char *web_server_ip(void);

#ifdef __cplusplus
}
#endif
