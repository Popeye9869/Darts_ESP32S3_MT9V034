idf_component_register(SRCS "main.c" "mt9v034_i2c.c" "mt9v034_mode.c" "mt9v_capture.c"
                            "camera_control.c" "web_server.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "web/index.html"
                       REQUIRES esp_driver_uart esp_driver_gpio esp_driver_cam esp_driver_i2c esp_timer
                                spi_flash esp_psram esp_hw_support console
                                esp_wifi esp_netif esp_http_server nvs_flash esp_event)
