#include "station.h"
#include "usb_console.h"
#include "app_config.h"
#include "local_web.h"
#include "status_led.h"
#include "link_crypto.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void station_task(void *argument)
{
    (void)argument;
    while (true) {
        station_poll();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void app_main(void)
{
    link_crypto_init();
    app_config_init();
    esp_err_t led_error = status_led_init();
    if (led_error != ESP_OK) ESP_LOGW("main", "RGB unavailable: %s", esp_err_to_name(led_error));
    esp_err_t error = usb_console_open();
    if (error != ESP_OK) ESP_LOGW("main", "USB unavailable: %s", esp_err_to_name(error));
    error = station_start();
    if (error != ESP_OK) {
        // Continue servicing USB so status/debugging works without a wired radio.
        // station_start records which initialisation stage failed.
    }
    // Radio deadlines have priority over HTTP rendering. On dual-core S3s the
    // SPI/USB/LED loop runs on core1, separately from the Wi-Fi driver.
#if CONFIG_FREERTOS_UNICORE
    const BaseType_t station_core = 0;
#else
    const BaseType_t station_core = 1;
#endif
    bool task_started = xTaskCreatePinnedToCore(station_task, "lora_station", 8192,
        NULL, 8, NULL, station_core) == pdPASS;
    if (!task_started) ESP_LOGE("main", "Radio task allocation failed; using main-loop fallback");
    error = local_web_start(station_web_snapshot, station_web_live_snapshot);
    if (error != ESP_OK) ESP_LOGE("main", "Local webpage unavailable: %s", esp_err_to_name(error));
    else if (local_web_mdns_is_ready()) {
        ESP_LOGI("main", "Local page: %s (fallback http://%s) / RGB GPIO%d",
                 local_web_url(), local_web_ip(), CONFIG_STATION_LED_GPIO);
    } else {
        ESP_LOGI("main", "Local page: http://%s / mDNS unavailable: %s / RGB GPIO%d",
                 local_web_ip(), esp_err_to_name(local_web_mdns_error()), CONFIG_STATION_LED_GPIO);
    }
    if (!task_started) station_task(NULL);
}
