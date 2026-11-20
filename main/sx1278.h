#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_err.h"

typedef struct {
    int mosi, miso, sclk, cs, reset;
    uint32_t frequency_hz, bandwidth_hz;
    uint16_t preamble;
    uint8_t sf, coding_rate, sync_word;
    int8_t tx_power_dbm;
} sx1278_config_t;

typedef enum { SX_EVENT_NONE, SX_EVENT_RX, SX_EVENT_TX_DONE, SX_EVENT_CRC_ERROR, SX_EVENT_TX_TIMEOUT } sx_event_kind_t;
typedef struct {
    sx_event_kind_t kind;
    uint8_t payload[255];
    size_t length;
    float rssi_dbm, snr_db;
} sx_event_t;

typedef struct {
    spi_device_handle_t spi;
    sx1278_config_t config;
    bool bus_open, transmitting;
    int64_t tx_deadline_us;
} sx1278_t;

esp_err_t sx1278_open(sx1278_t *radio, const sx1278_config_t *config);
void sx1278_close(sx1278_t *radio);
esp_err_t sx1278_receive(sx1278_t *radio);
esp_err_t sx1278_transmit(sx1278_t *radio, const uint8_t *payload, size_t length);
esp_err_t sx1278_poll(sx1278_t *radio, sx_event_t *event);
uint32_t sx1278_airtime_us(const sx1278_config_t *config, size_t length);
