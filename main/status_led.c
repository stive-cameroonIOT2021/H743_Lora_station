#include "status_led.h"

#include <stdatomic.h>
#include <stddef.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"

#ifndef CONFIG_STATION_LED_ENABLED
#define CONFIG_STATION_LED_ENABLED 0
#endif
#ifndef CONFIG_STATION_LED_GPIO
#define CONFIG_STATION_LED_GPIO 21
#endif
#ifndef CONFIG_STATION_LED_BRIGHTNESS
#define CONFIG_STATION_LED_BRIGHTNESS 8
#endif

_Static_assert(CONFIG_STATION_LED_BRIGHTNESS >= 0 && CONFIG_STATION_LED_BRIGHTNESS <= 255,
               "RGB brightness must be between 0 and 255");

static atomic_bool ready = ATOMIC_VAR_INIT(false);

const char *status_led_state_name(status_led_state_t state)
{
    switch (state) {
    case STATUS_LED_ERROR: return "error";
    case STATUS_LED_KEY_MISSING: return "key_missing";
    case STATUS_LED_WAITING: return "waiting";
    case STATUS_LED_CONNECTED: return "connected";
    case STATUS_LED_DEGRADED: return "degraded";
    case STATUS_LED_LOST: return "lost";
    default: return "unknown";
    }
}

bool status_led_ready(void)
{
    return atomic_load_explicit(&ready, memory_order_acquire);
}

#if CONFIG_STATION_LED_ENABLED

static const char *TAG = "status_led";
static rmt_channel_handle_t channel;
static rmt_encoder_handle_t encoder;
static atomic_bool transmitting = ATOMIC_VAR_INIT(false);
static rmt_symbol_word_t symbols[25];
static uint32_t previous_rgb = UINT32_MAX;
static bool initialization_attempted;
static esp_err_t initialization_result = ESP_ERR_INVALID_STATE;

/* One WS2812 LED: MSB-first GRB, 10MHz ticks, then 80us held low. */
static void encode_rgb(uint32_t rgb)
{
    const uint8_t grb[] = {
        (uint8_t)(rgb >> 8), (uint8_t)(rgb >> 16), (uint8_t)rgb,
    };
    size_t index = 0;
    for (size_t byte = 0; byte < sizeof(grb); ++byte) {
        for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
            bool one = (grb[byte] & mask) != 0;
            symbols[index++] = (rmt_symbol_word_t){
                .level0 = 1,
                .duration0 = one ? 9 : 3,
                .level1 = 0,
                .duration1 = one ? 3 : 9,
            };
        }
    }
    symbols[index] = (rmt_symbol_word_t){
        .level0 = 0, .duration0 = 400,
        .level1 = 0, .duration1 = 400,
    };
}

static bool IRAM_ATTR transfer_done(rmt_channel_handle_t tx_channel,
                                  const rmt_tx_done_event_data_t *event,
                                  void *context)
{
    (void)tx_channel;
    (void)event;
    (void)context;
    atomic_store_explicit(&transmitting, false, memory_order_release);
    return false;
}

static uint32_t status_rgb(status_led_state_t state, int64_t now_us)
{
    const uint32_t brightness = CONFIG_STATION_LED_BRIGHTNESS;
    const uint32_t red = brightness << 16;
    const uint32_t green = brightness << 8;
    const uint32_t blue = brightness;
    const uint32_t amber = red | ((brightness / 2) << 8);
    uint64_t time_us = now_us > 0 ? (uint64_t)now_us : 0;
    switch (state) {
    case STATUS_LED_CONNECTED:
        return green;
    case STATUS_LED_DEGRADED:
        return amber;
    case STATUS_LED_KEY_MISSING:
        return (time_us % 1600000 < 800000) ? amber : 0;
    case STATUS_LED_WAITING:
        return (time_us % 2000000 < 250000) ? blue : 0;
    case STATUS_LED_LOST:
        return (time_us % 1000000 < 500000) ? red : 0;
    case STATUS_LED_ERROR:
    default:
        return (time_us % 250000 < 125000) ? red : 0;
    }
}

esp_err_t status_led_init(void)
{
    if (initialization_attempted) {
        return initialization_result;
    }
    initialization_attempted = true;
    if (!GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_STATION_LED_GPIO)) {
        initialization_result = ESP_ERR_INVALID_ARG;
        ESP_LOGW(TAG, "RGB LED disabled: GPIO%d is not an output", CONFIG_STATION_LED_GPIO);
        return initialization_result;
    }

    const rmt_tx_channel_config_t config = {
        .gpio_num = CONFIG_STATION_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    esp_err_t result = rmt_new_tx_channel(&config, &channel);
    if (result == ESP_OK) {
        const rmt_copy_encoder_config_t copy_config = {};
        result = rmt_new_copy_encoder(&copy_config, &encoder);
    }
    if (result == ESP_OK) {
        const rmt_tx_event_callbacks_t callbacks = {.on_trans_done = transfer_done};
        result = rmt_tx_register_event_callbacks(channel, &callbacks, NULL);
    }
    if (result == ESP_OK) {
        result = rmt_enable(channel);
    }
    initialization_result = result;
    if (result != ESP_OK) {
        if (encoder != NULL) {
            rmt_del_encoder(encoder);
            encoder = NULL;
        }
        if (channel != NULL) {
            rmt_del_channel(channel);
            channel = NULL;
        }
        ESP_LOGW(TAG, "RGB LED unavailable: %s", esp_err_to_name(result));
        return result;
    }
    atomic_store_explicit(&ready, true, memory_order_release);
    return ESP_OK;
}

void status_led_update(status_led_state_t state, int64_t now_us)
{
    if (!status_led_ready() || atomic_load_explicit(&transmitting, memory_order_acquire)) {
        return;
    }
    uint32_t rgb = status_rgb(state, now_us);
    if (rgb == previous_rgb) {
        return;
    }
    /* Static payload remains unchanged until transfer_done releases it. */
    encode_rgb(rgb);
    const rmt_transmit_config_t config = {
        .loop_count = 0,
        .flags = {.eot_level = 0, .queue_nonblocking = 1},
    };
    atomic_store_explicit(&transmitting, true, memory_order_release);
    esp_err_t result = rmt_transmit(channel, encoder, symbols, sizeof(symbols), &config);
    if (result == ESP_OK) {
        previous_rgb = rgb;
        return;
    }
    atomic_store_explicit(&transmitting, false, memory_order_release);
    /* Report once and stop submitting LED work. Radio and web keep running. */
    atomic_store_explicit(&ready, false, memory_order_release);
    ESP_LOGW(TAG, "RGB LED updates disabled: %s", esp_err_to_name(result));
}

#else

esp_err_t status_led_init(void)
{
    return ESP_OK;
}

void status_led_update(status_led_state_t state, int64_t now_us)
{
    (void)state;
    (void)now_us;
}

#endif
