idf_component_register(
    SRCS "main.c" "station.c" "sx1278.c" "usb_console.c" "link_crypto.c" "link_protocol.c"
         "app_config.c" "status_led.c" "local_web.c" "web_origin.c" "telemetry_cache.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_spi esp_driver_gpio esp_driver_usb_serial_jtag esp_driver_rmt
             esp_timer esp_system bootloader_support nvs_flash json mbedtls esp_wifi esp_netif esp_event esp_http_server mdns
    EMBED_TXTFILES "web/index.html"
)
