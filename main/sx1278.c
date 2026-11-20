#include "sx1278.h"

#include <math.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REG_FIFO 0x00
#define REG_OP_MODE 0x01
#define REG_FRF_MSB 0x06
#define REG_PA_CONFIG 0x09
#define REG_OCP 0x0b
#define REG_LNA 0x0c
#define REG_FIFO_ADDR_PTR 0x0d
#define REG_FIFO_TX_BASE 0x0e
#define REG_FIFO_RX_BASE 0x0f
#define REG_FIFO_RX_CURRENT 0x10
#define REG_IRQ_FLAGS 0x12
#define REG_RX_NB_BYTES 0x13
#define REG_PKT_SNR 0x19
#define REG_PKT_RSSI 0x1a
#define REG_HOP_CHANNEL 0x1c
#define REG_MODEM_CONFIG1 0x1d
#define REG_MODEM_CONFIG2 0x1e
#define REG_PREAMBLE_MSB 0x20
#define REG_PAYLOAD_LENGTH 0x22
#define REG_MODEM_CONFIG3 0x26
#define REG_DETECT_OPTIMIZE 0x31
#define REG_DETECTION_THRESHOLD 0x37
#define REG_SYNC_WORD 0x39
#define REG_VERSION 0x42
#define REG_PA_DAC 0x4d
#define IRQ_RX_DONE 0x40
#define IRQ_CRC_ERROR 0x20
#define IRQ_TX_DONE 0x08
#define MODE_SLEEP 0x88
#define MODE_STANDBY 0x89
#define MODE_TX 0x8b
#define MODE_RX 0x8d

static esp_err_t transfer(sx1278_t *radio, uint8_t address, const uint8_t *tx, uint8_t *rx, size_t length)
{
    uint8_t outgoing[SOC_SPI_MAXIMUM_BUFFER_SIZE] = {0};
    uint8_t incoming[SOC_SPI_MAXIMUM_BUFFER_SIZE] = {0};
    const size_t chunk_capacity = sizeof(outgoing) - 1;
    if (length > 255) return ESP_ERR_INVALID_SIZE;
    /* DMA is disabled, so the ESP32-S3 host accepts at most 64 bytes,
     * including the register address. FIFO bursts can be split because the
     * SX1278 retains and advances its FIFO pointer between CS assertions. */
    if ((address & 0x7f) != REG_FIFO && length > chunk_capacity) return ESP_ERR_INVALID_SIZE;
    size_t offset = 0;
    do {
        size_t count = length - offset;
        if (count > chunk_capacity) count = chunk_capacity;
        outgoing[0] = address;
        if (tx) memcpy(outgoing + 1, tx + offset, count);
        spi_transaction_t transaction = {.length = (count + 1) * 8, .tx_buffer = outgoing, .rx_buffer = incoming};
        esp_err_t error = spi_device_transmit(radio->spi, &transaction);
        if (error != ESP_OK) return error;
        if (rx) memcpy(rx + offset, incoming + 1, count);
        offset += count;
    } while (offset < length);
    return ESP_OK;
}

static esp_err_t write_reg(sx1278_t *radio, uint8_t address, uint8_t value)
{
    return transfer(radio, address | 0x80, &value, NULL, 1);
}

static esp_err_t read_reg(sx1278_t *radio, uint8_t address, uint8_t *value)
{
    return transfer(radio, address & 0x7f, NULL, value, 1);
}

uint32_t sx1278_airtime_us(const sx1278_config_t *config, size_t length)
{
    double symbol_s = (double)(1U << config->sf) / config->bandwidth_hz;
    int ldro = symbol_s >= .016;
    double quotient = (8.0 * length - 4.0 * config->sf + 28 + 16) / (4.0 * (config->sf - 2 * ldro));
    double payload_symbols = 8 + fmax(0, ceil(quotient) * config->coding_rate);
    return (uint32_t)ceil((config->preamble + 4.25 + payload_symbols) * symbol_s * 1000000);
}

esp_err_t sx1278_receive(sx1278_t *radio)
{
    if (!radio->spi) return ESP_ERR_INVALID_STATE;
    esp_err_t error = write_reg(radio, REG_OP_MODE, MODE_STANDBY);
    if (error != ESP_OK) return error;
    error = write_reg(radio, REG_IRQ_FLAGS, 0xff);
    if (error != ESP_OK) return error;
    radio->transmitting = false;
    return write_reg(radio, REG_OP_MODE, MODE_RX);
}

esp_err_t sx1278_open(sx1278_t *radio, const sx1278_config_t *config)
{
    if (!radio || !config || config->frequency_hz < 410000000 || config->frequency_hz > 525000000 ||
        config->sf < 7 || config->sf > 12 || config->coding_rate < 5 || config->coding_rate > 8 ||
        config->preamble < 6 || config->tx_power_dbm < 2 || config->tx_power_dbm > 17) return ESP_ERR_INVALID_ARG;
    int bandwidth_code = config->bandwidth_hz == 125000 ? 7 : config->bandwidth_hz == 250000 ? 8 : config->bandwidth_hz == 500000 ? 9 : -1;
    if (bandwidth_code < 0) return ESP_ERR_INVALID_ARG;
    memset(radio, 0, sizeof(*radio));
    radio->config = *config;
    int pins[] = {config->mosi, config->miso, config->sclk, config->cs, config->reset};
    for (size_t i = 0; i < 5; ++i) {
        if (!GPIO_IS_VALID_GPIO(pins[i]) || (i != 1 && !GPIO_IS_VALID_OUTPUT_GPIO(pins[i]))) return ESP_ERR_INVALID_ARG;
        for (size_t j = 0; j < i; ++j) if (pins[i] == pins[j]) return ESP_ERR_INVALID_ARG;
        if (pins[i] == 19 || pins[i] == 20 || pins[i] == 0 || pins[i] == 3 ||
            pins[i] == 45 || pins[i] == 46 || (pins[i] >= 26 && pins[i] <= 37)) return ESP_ERR_INVALID_ARG;
#if CONFIG_STATION_LED_ENABLED
        if (pins[i] == CONFIG_STATION_LED_GPIO) return ESP_ERR_INVALID_ARG;
#endif
    }
    gpio_config_t reset = {.pin_bit_mask = 1ULL << config->reset, .mode = GPIO_MODE_OUTPUT};
    esp_err_t error = gpio_config(&reset);
    if (error != ESP_OK) return error;
    gpio_set_level(config->reset, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(config->reset, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    spi_bus_config_t bus = {.mosi_io_num = config->mosi, .miso_io_num = config->miso, .sclk_io_num = config->sclk,
                           .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = SOC_SPI_MAXIMUM_BUFFER_SIZE};
    error = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED);
    if (error != ESP_OK) return error;
    radio->bus_open = true;
    spi_device_interface_config_t device = {.clock_speed_hz = 1000000, .mode = 0, .spics_io_num = config->cs, .queue_size = 1};
    error = spi_bus_add_device(SPI2_HOST, &device, &radio->spi);
    if (error != ESP_OK) goto failed;
    uint8_t version;
    error = read_reg(radio, REG_VERSION, &version);
    if (error != ESP_OK) goto failed;
    if (version != 0x12) { error = ESP_ERR_NOT_FOUND; goto failed; }
    error = write_reg(radio, REG_OP_MODE, MODE_SLEEP);
    if (error != ESP_OK) goto failed;
    uint64_t frequency = (((uint64_t)config->frequency_hz << 19) + 16000000) / 32000000;
    uint8_t frequency_bytes[] = {frequency >> 16, frequency >> 8, frequency};
    error = transfer(radio, REG_FRF_MSB | 0x80, frequency_bytes, NULL, 3);
    if (error != ESP_OK) goto failed;
    const uint8_t settings[][2] = {
        {REG_FIFO_TX_BASE, 0}, {REG_FIFO_RX_BASE, 0}, {REG_LNA, 0x23},
        {REG_PA_CONFIG, (uint8_t)(0x80 | (config->tx_power_dbm - 2))}, {REG_PA_DAC, 0x84}, {REG_OCP, 0x2b},
        {REG_MODEM_CONFIG1, (uint8_t)((bandwidth_code << 4) | ((config->coding_rate - 4) << 1))},
        {REG_MODEM_CONFIG2, (uint8_t)((config->sf << 4) | 0x04)},
        {REG_MODEM_CONFIG3, (uint8_t)(0x04 | (((double)(1U << config->sf) / config->bandwidth_hz >= .016) ? 0x08 : 0))},
        {REG_DETECT_OPTIMIZE, 0x03}, {REG_DETECTION_THRESHOLD, 0x0a}, {0x33, 0x27}, {0x3b, 0x1d},
        {REG_SYNC_WORD, config->sync_word}, {REG_PREAMBLE_MSB, config->preamble >> 8}, {REG_PREAMBLE_MSB + 1, config->preamble & 0xff},
    };
    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        error = write_reg(radio, settings[i][0], settings[i][1]);
        if (error != ESP_OK) goto failed;
    }
    return sx1278_receive(radio);
failed:
    sx1278_close(radio);
    return error;
}

esp_err_t sx1278_transmit(sx1278_t *radio, const uint8_t *payload, size_t length)
{
    if (!radio || !radio->spi || radio->transmitting) return ESP_ERR_INVALID_STATE;
    if (!payload || !length || length > 255) return ESP_ERR_INVALID_SIZE;
    esp_err_t error = write_reg(radio, REG_OP_MODE, MODE_STANDBY);
    if (error == ESP_OK) error = write_reg(radio, REG_IRQ_FLAGS, 0xff);
    if (error == ESP_OK) error = write_reg(radio, REG_FIFO_ADDR_PTR, 0);
    if (error == ESP_OK) error = transfer(radio, REG_FIFO | 0x80, payload, NULL, length);
    if (error == ESP_OK) error = write_reg(radio, REG_PAYLOAD_LENGTH, length);
    if (error == ESP_OK) error = write_reg(radio, REG_OP_MODE, MODE_TX);
    if (error != ESP_OK) { sx1278_receive(radio); return error; }
    radio->transmitting = true;
    radio->tx_deadline_us = esp_timer_get_time() + sx1278_airtime_us(&radio->config, length) + 500000;
    return ESP_OK;
}

esp_err_t sx1278_poll(sx1278_t *radio, sx_event_t *event)
{
    memset(event, 0, sizeof(*event));
    uint8_t flags;
    esp_err_t error = read_reg(radio, REG_IRQ_FLAGS, &flags);
    if (error != ESP_OK) return error;
    if (radio->transmitting) {
        if (flags & IRQ_TX_DONE) event->kind = SX_EVENT_TX_DONE;
        else if (esp_timer_get_time() >= radio->tx_deadline_us) event->kind = SX_EVENT_TX_TIMEOUT;
        else return ESP_OK;
        return sx1278_receive(radio);
    }
    if (!(flags & IRQ_RX_DONE)) return ESP_OK;
    uint8_t hop_channel;
    if ((error = read_reg(radio, REG_HOP_CHANNEL, &hop_channel)) != ESP_OK) return error;
    if ((flags & IRQ_CRC_ERROR) || !(hop_channel & 0x40)) {
        event->kind = SX_EVENT_CRC_ERROR;
        return write_reg(radio, REG_IRQ_FLAGS, flags);
    }
    uint8_t current, count, snr, rssi;
    if ((error = read_reg(radio, REG_FIFO_RX_CURRENT, &current)) != ESP_OK ||
        (error = read_reg(radio, REG_RX_NB_BYTES, &count)) != ESP_OK ||
        (error = read_reg(radio, REG_PKT_SNR, &snr)) != ESP_OK ||
        (error = read_reg(radio, REG_PKT_RSSI, &rssi)) != ESP_OK ||
        (error = write_reg(radio, REG_FIFO_ADDR_PTR, current)) != ESP_OK ||
        (error = transfer(radio, REG_FIFO, NULL, event->payload, count)) != ESP_OK) return error;
    event->kind = SX_EVENT_RX;
    event->length = count;
    event->snr_db = (int8_t)snr / 4.0f;
    event->rssi_dbm = rssi - 164.0f + fminf(0, event->snr_db);
    return write_reg(radio, REG_IRQ_FLAGS, flags);
}

void sx1278_close(sx1278_t *radio)
{
    if (!radio) return;
    if (radio->spi) { write_reg(radio, REG_OP_MODE, MODE_SLEEP); spi_bus_remove_device(radio->spi); radio->spi = NULL; }
    if (radio->bus_open) { spi_bus_free(SPI2_HOST); radio->bus_open = false; }
    radio->transmitting = false;
}
