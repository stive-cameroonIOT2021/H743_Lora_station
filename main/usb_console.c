#include "usb_console.h"

#include <string.h>
#include "driver/usb_serial_jtag.h"

#define LINE_LIMIT 512
#define OUTPUT_LIMIT 4096
static bool driver_ready;
static char input[LINE_LIMIT];
static size_t input_length;
static bool discarding;
static char output[OUTPUT_LIMIT];
static size_t output_length, output_offset;
static uint32_t dropped;

esp_err_t usb_console_open(void)
{
    usb_serial_jtag_driver_config_t config = {.tx_buffer_size = 2048, .rx_buffer_size = 2048};
    esp_err_t error = usb_serial_jtag_driver_install(&config);
    driver_ready = error == ESP_OK;
    return error;
}

static void pump(void)
{
    if (!driver_ready) return;
    if (!usb_serial_jtag_is_connected()) {
        if (output_length) ++dropped;
        output_length = output_offset = 0;
        return;
    }
    if (output_offset < output_length) {
        int sent = usb_serial_jtag_write_bytes(output + output_offset, output_length - output_offset, 0);
        if (sent > 0) output_offset += sent;
    }
    if (output_offset >= output_length) output_length = output_offset = 0;
}

bool usb_console_send(cJSON *message)
{
    if (!driver_ready) { ++dropped; return false; }
    pump();
    if (output_length || !usb_serial_jtag_is_connected()) { ++dropped; return false; }
    if (!cJSON_PrintPreallocated(message, output, sizeof(output) - 2, false)) { ++dropped; return false; }
    output_length = strlen(output);
    output[output_length++] = '\n';
    output_offset = 0;
    pump();
    return true;
}

void usb_console_poll(usb_line_fn callback, void *context)
{
    if (!driver_ready) return;
    uint8_t bytes[128];
    pump();
    int count = usb_serial_jtag_read_bytes(bytes, sizeof(bytes), 0);
    for (int index = 0; index < count; ++index) {
        char value = bytes[index];
        if (value == '\n') {
            if (discarding) {
                cJSON *error = cJSON_CreateObject();
                cJSON_AddStringToObject(error, "type", "error");
                cJSON_AddStringToObject(error, "error", "usb_line_too_long");
                usb_console_send(error);
                cJSON_Delete(error);
            } else if (input_length) {
                input[input_length] = 0;
                callback(input, context);
                // Do not retain completed console input.
                memset(input, 0, sizeof(input));
            }
            input_length = 0;
            discarding = false;
        } else if (value != '\r' && !discarding) {
            if (input_length + 1 >= sizeof(input) || value == 0) {
                discarding = true;
                memset(input, 0, sizeof(input));
                input_length = 0;
            } else input[input_length++] = value;
        }
    }
}

uint32_t usb_console_dropped(void) { return dropped; }
