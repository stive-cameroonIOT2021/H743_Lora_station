/* Compile the real driver; emulate only GPIO, clock and SPI hardware. */
#include "../main/sx1278.c"

static sx1278_t radio;
static uint8_t registers[128], fifo[256], pointer;
static unsigned int spi_calls, fifo_calls, maximum_wire_bytes, fail_call;

void host_reset(void)
{
    memset(&radio, 0, sizeof(radio));
    memset(registers, 0, sizeof(registers));
    memset(fifo, 0, sizeof(fifo));
    pointer = 0;
    spi_calls = fifo_calls = maximum_wire_bytes = fail_call = 0;
    registers[REG_VERSION] = 0x12;
    radio.spi = &radio;
    radio.config = (sx1278_config_t){.frequency_hz = 433775000, .bandwidth_hz = 125000,
                                    .sf = 7, .coding_rate = 5, .preamble = 8};
}

esp_err_t spi_device_transmit(spi_device_handle_t device, spi_transaction_t *transaction)
{
    (void)device;
    ++spi_calls;
    if (transaction->length % 8) return ESP_ERR_INVALID_ARG;
    size_t bytes = transaction->length / 8;
    if (bytes > maximum_wire_bytes) maximum_wire_bytes = bytes;
    /* Match the actual ESP32-S3 non-DMA check, regardless max_transfer_sz. */
    if (bytes > SOC_SPI_MAXIMUM_BUFFER_SIZE) return ESP_ERR_INVALID_ARG;
    if (spi_calls == fail_call) return ESP_FAIL;
    const uint8_t *outgoing = transaction->tx_buffer;
    uint8_t *incoming = transaction->rx_buffer;
    memset(incoming, 0, bytes);
    uint8_t address = outgoing[0] & 0x7f;
    bool write = outgoing[0] & 0x80;
    if (address == REG_FIFO) ++fifo_calls;
    for (size_t i = 1; i < bytes; ++i) {
        if (address == REG_FIFO) {
            if (write) fifo[pointer++] = outgoing[i];
            else incoming[i] = fifo[pointer++];
        } else {
            uint8_t current = (uint8_t)(address + i - 1) & 0x7f;
            if (write) {
                if (current == REG_IRQ_FLAGS) registers[current] &= (uint8_t)~outgoing[i];
                else registers[current] = outgoing[i];
                if (current == REG_FIFO_ADDR_PTR) pointer = outgoing[i];
            } else incoming[i] = registers[current];
        }
    }
    return ESP_OK;
}

esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *config, int dma)
{ (void)host; (void)config; (void)dma; return ESP_OK; }
esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *config, spi_device_handle_t *device)
{ (void)host; (void)config; *device = &radio; return ESP_OK; }
esp_err_t spi_bus_remove_device(spi_device_handle_t device) { (void)device; return ESP_OK; }
esp_err_t spi_bus_free(int host) { (void)host; return ESP_OK; }
esp_err_t gpio_config(const gpio_config_t *config) { (void)config; return ESP_OK; }
esp_err_t gpio_set_level(int pin, int value) { (void)pin; (void)value; return ESP_OK; }
void vTaskDelay(unsigned int ticks) { (void)ticks; }
int64_t esp_timer_get_time(void) { return 1000000; }

unsigned int host_spi_calls(void) { return spi_calls; }
unsigned int host_fifo_calls(void) { return fifo_calls; }
unsigned int host_maximum_wire_bytes(void) { return maximum_wire_bytes; }
void host_fail_call(unsigned int value) { fail_call = value; }
void host_set_pointer(uint8_t value) { pointer = value; }
uint8_t host_get_pointer(void) { return pointer; }
void host_copy_fifo(uint8_t *output) { memcpy(output, fifo, sizeof(fifo)); }
void host_copy_registers(uint8_t *output) { memcpy(output, registers, sizeof(registers)); }

int host_transfer(int write, uint8_t address, uint8_t *data, size_t length)
{ return transfer(&radio, address | (write ? 0x80 : 0), write ? data : NULL, write ? NULL : data, length); }
int host_transmit(const uint8_t *data, size_t length) { return sx1278_transmit(&radio, data, length); }
void host_prepare_packet(const uint8_t *data, size_t length, uint8_t start)
{
    for (size_t i = 0; i < length; ++i) fifo[(uint8_t)(start + i)] = data[i];
    registers[REG_FIFO_RX_CURRENT] = start;
    registers[REG_RX_NB_BYTES] = (uint8_t)length;
    registers[REG_IRQ_FLAGS] = IRQ_RX_DONE;
    registers[REG_HOP_CHANNEL] = 0x40;
    registers[REG_PKT_SNR] = 8;
    registers[REG_PKT_RSSI] = 164;
}
int host_poll(uint8_t *data, size_t *length)
{
    sx_event_t event;
    int error = sx1278_poll(&radio, &event);
    *length = event.length;
    if (error == ESP_OK && event.kind == SX_EVENT_RX) memcpy(data, event.payload, event.length);
    return error == ESP_OK ? (int)event.kind : -error;
}
