#include "station.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "app_config.h"
#include "link_crypto.h"
#include "link_key.h"
#include "local_web.h"
#include "link_protocol.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "sx1278.h"
#include "status_led.h"
#include "telemetry_cache.h"
#include "usb_console.h"

#ifndef CONFIG_STATION_LED_ENABLED
#define CONFIG_STATION_LED_ENABLED 0
#endif
#ifndef CONFIG_STATION_WEB_ENABLED
#define CONFIG_STATION_WEB_ENABLED 0
#endif

#define PI_NODE 1
#define GROUND_NODE 2
#define MAX_REQUEST_ATTEMPTS 3
#define REQUEST_LIFETIME_US 60000000LL
#define ACK_PAYLOAD_SIZE 16
#define ACK_START_WINDOW_US 150000LL

static const char *TAG = "station";
static SemaphoreHandle_t snapshot_mutex;
static cJSON *published_status;
static int64_t next_publish_us, next_debug_us;
static esp_err_t cache_error;

typedef struct {
    uint32_t sequence;
    uint16_t bytes;
    uint8_t kind;
    float rssi, snr;
    int64_t received_us;
} packet_summary_t;

typedef struct {
    bool active;
    uint32_t id;
    uint8_t opcode, argument, attempts;
    int64_t deadline_us;
} request_t;

static struct {
    sx1278_t radio;
    bool radio_ready, storage_ready, key_ready, have_peer, ack_pending, have_sequence;
    esp_err_t initial_error;
    const char *initial_stage;
    uint8_t key[32];
    uint64_t session, peer_session, retired_sessions[4], sequence_mask;
    unsigned retired_count;
    uint32_t sequence, last_sequence;
    uint64_t rx_valid, rx_duplicates, rx_old, rx_missing, rx_invalid, rx_crc, tx_count, tx_timeout, ack_skipped, spi_errors;
    uint64_t rx_auth_errors, rx_format_errors, rx_decode_errors, session_rx_valid;
    float last_rssi, last_snr;
    int64_t last_rx_us, last_tx_us, next_tx_us, next_status_us, ack_due_us, ack_deadline_us;
    int64_t tx_started_us;
    int64_t next_radio_retry_us;
    uint32_t tx_airtime_us, ack_peer_sequence;
    uint64_t ack_peer_session;
    struct { uint32_t id; uint8_t opcode, argument; int result; const char *status; int64_t finished_us; } completed[8];
    unsigned completed_index;
    packet_summary_t recent[8];
    unsigned recent_count, recent_index;
    request_t request;
} station;

static int64_t now_us(void) { return esp_timer_get_time(); }

static cJSON *message(const char *type)
{
    cJSON *object = cJSON_CreateObject();
    if (object) {
        cJSON_AddStringToObject(object, "type", type);
        cJSON_AddNumberToObject(object, "uptime_ms", now_us() / 1000);
    }
    return object;
}

static void emit(cJSON *object)
{
    if (!object) return;

    if (app_config_terminal_print_enabled()) {
        usb_console_send(object);
    }

    cJSON_Delete(object);
}

static void error_message(const char *error, const char *operation)
{
    cJSON *object = message("error");
    cJSON_AddStringToObject(object, "error", error);
    if (operation) cJSON_AddStringToObject(object, "op", operation);
    emit(object);
}

static void request_status(uint32_t id, const char *status, int result)
{
    cJSON *object = message("request_status");
    cJSON_AddNumberToObject(object, "id", id);
    cJSON_AddStringToObject(object, "status", status);
    if (result >= 0) cJSON_AddNumberToObject(object, "result", result);
    emit(object);
}

static void finish_request(const char *status, int result)
{
    unsigned slot = station.completed_index++ % 8;
    station.completed[slot].id = station.request.id;
    station.completed[slot].opcode = station.request.opcode;
    station.completed[slot].argument = station.request.argument;
    station.completed[slot].status = status;
    station.completed[slot].result = result;
    station.completed[slot].finished_us = now_us();
    request_status(station.request.id, status, result);
    station.request.active = false;
}

static uint32_t expected_period_ms(void)
{
    uint64_t airtime = sx1278_airtime_us(&station.radio.config, LP_MAX_FRAME);
    uint64_t minimum = airtime * 1000 / CONFIG_STATION_DUTY_PERMILLE;
    uint64_t full_exchange = airtime + sx1278_airtime_us(&station.radio.config, LP_MIN_FRAME + ACK_PAYLOAD_SIZE) + ACK_START_WINDOW_US;
    if (full_exchange > minimum) minimum = full_exchange;
    uint32_t period = (minimum + 999) / 1000;
    return period < CONFIG_STATION_EXPECTED_PERIOD_MS ? CONFIG_STATION_EXPECTED_PERIOD_MS : period;
}

static const char *uplink_state(void)
{
    if (!station.have_peer) return "waiting";
    int64_t age_ms = (now_us() - station.last_rx_us) / 1000;
    uint32_t period = expected_period_ms();
    return age_ms > (int64_t)period * 8 ? "lost" : age_ms > (int64_t)period * 3 ? "degraded" : "connected";
}

static cJSON *status_snapshot(const cJSON *id)
{
    cJSON *object = message("status");
    if (!object) return NULL;
    if (id) cJSON_AddItemToObject(object, "id", cJSON_Duplicate(id, true));
    cJSON_AddBoolToObject(object, "radio_ready", station.radio_ready);
    cJSON_AddBoolToObject(object, "key_configured", station.key_ready);
    cJSON_AddNumberToObject(object, "protocol_version", LP_VERSION);
    cJSON_AddStringToObject(object, "cipher", "AES-256-GCM");
    cJSON_AddBoolToObject(object, "nonce_ready", link_crypto_nonce_available());
    cJSON_AddNumberToObject(object, "tx_nonces_used", (double)link_crypto_nonces_used());
    cJSON_AddBoolToObject(object, "storage_ready", station.storage_ready);
    cJSON_AddStringToObject(object, "uplink_state", uplink_state());
    if (station.initial_error != ESP_OK) {
        cJSON_AddStringToObject(object, "initial_stage", station.initial_stage);
        cJSON_AddStringToObject(object, "initial_error", esp_err_to_name(station.initial_error));
    }
    char session[17], peer[17];
    snprintf(session, sizeof(session), "%016" PRIx64, station.session);
    snprintf(peer, sizeof(peer), "%016" PRIx64, station.peer_session);
    cJSON_AddStringToObject(object, "session", session);
    if (station.have_peer) {
        cJSON_AddStringToObject(object, "peer_session", peer);
        cJSON_AddNumberToObject(object, "last_sequence", station.last_sequence);
        cJSON_AddNumberToObject(object, "last_rx_age_ms", (now_us() - station.last_rx_us) / 1000);
        cJSON_AddNumberToObject(object, "rssi_dbm", station.last_rssi);
        cJSON_AddNumberToObject(object, "snr_db", station.last_snr);
    }
    cJSON_AddNumberToObject(object, "rx_valid", station.rx_valid);
    cJSON_AddNumberToObject(object, "rx_duplicates", station.rx_duplicates);
    cJSON_AddNumberToObject(object, "rx_old", station.rx_old);
    cJSON_AddNumberToObject(object, "rx_missing", station.rx_missing);
    cJSON_AddNumberToObject(object, "rx_invalid", station.rx_invalid);
    cJSON_AddNumberToObject(object, "rx_auth_errors", station.rx_auth_errors);
    cJSON_AddNumberToObject(object, "rx_format_errors", station.rx_format_errors);
    cJSON_AddNumberToObject(object, "rx_crc_errors", station.rx_crc);
    cJSON_AddNumberToObject(object, "rx_decode_errors", station.rx_decode_errors);
    cJSON_AddNumberToObject(object, "session_rx_valid", station.session_rx_valid);
    cJSON_AddNumberToObject(object, "tx_count", station.tx_count);
    cJSON_AddNumberToObject(object, "tx_timeouts", station.tx_timeout);
    cJSON_AddNumberToObject(object, "ack_skipped", station.ack_skipped);
    cJSON_AddNumberToObject(object, "spi_errors", station.spi_errors);
    cJSON_AddNumberToObject(object, "usb_dropped", usb_console_dropped());
    cJSON_AddNumberToObject(object, "frequency_hz", CONFIG_STATION_FREQUENCY_HZ);
    cJSON_AddNumberToObject(object, "bandwidth_hz", CONFIG_STATION_BANDWIDTH_HZ);
    cJSON_AddNumberToObject(object, "spreading_factor", CONFIG_STATION_SF);
    cJSON_AddNumberToObject(object, "coding_rate", CONFIG_STATION_CODING_RATE);
    cJSON_AddNumberToObject(object, "sync_word", CONFIG_STATION_SYNC_WORD);
    cJSON_AddNumberToObject(object, "preamble", CONFIG_STATION_PREAMBLE);
    cJSON_AddNumberToObject(object, "tx_power_dbm", CONFIG_STATION_TX_POWER_DBM);
    cJSON_AddNumberToObject(object, "duty_cycle", CONFIG_STATION_DUTY_PERMILLE / 1000.0);
    cJSON_AddNumberToObject(object, "expected_period_ms", expected_period_ms());
    cJSON_AddBoolToObject(object, "terminal_print_enabled", app_config_terminal_print_enabled());
    cJSON_AddBoolToObject(object, "usb_connected", usb_serial_jtag_is_connected());
    cJSON_AddBoolToObject(object, "web_ready", local_web_is_ready());
    cJSON_AddBoolToObject(object, "led_ready", status_led_ready());
    cJSON_AddNumberToObject(object, "led_gpio", CONFIG_STATION_LED_GPIO);
    cJSON_AddNumberToObject(object, "free_heap_bytes", esp_get_free_heap_size());
    cJSON_AddNumberToObject(object, "minimum_free_heap_bytes", esp_get_minimum_free_heap_size());
    if (cache_error != ESP_OK) cJSON_AddStringToObject(object, "telemetry_cache_error", esp_err_to_name(cache_error));
    if (station.request.active) {
        cJSON_AddNumberToObject(object, "pending_request_id", station.request.id);
        cJSON_AddNumberToObject(object, "request_attempts", station.request.attempts);
    }
    return object;
}

static void status_message(const cJSON *id) { emit(status_snapshot(id)); }

static status_led_state_t led_state(void)
{
    if (!station.radio_ready || !station.storage_ready || cache_error != ESP_OK) return STATUS_LED_ERROR;
    if (!station.key_ready) return STATUS_LED_KEY_MISSING;
    if (!link_crypto_nonce_available()) return STATUS_LED_ERROR;
    if (!station.have_peer) return STATUS_LED_WAITING;
    const char *state = uplink_state();
    if (!strcmp(state, "lost")) return STATUS_LED_LOST;
    if (!strcmp(state, "degraded")) return STATUS_LED_DEGRADED;
    return STATUS_LED_CONNECTED;
}

static void publish_web_status(void)
{
    cJSON *fresh = status_snapshot(NULL);
    if (!fresh) return;
    cJSON_AddStringToObject(fresh, "led_state", status_led_state_name(led_state()));
    cJSON *packets = cJSON_AddArrayToObject(fresh, "recent_packets");
    for (unsigned index = 0; index < station.recent_count; ++index) {
        unsigned slot = (station.recent_index + 8 - 1 - index) % 8;
        const packet_summary_t *item = &station.recent[slot];
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "sequence", item->sequence);
        cJSON_AddNumberToObject(entry, "kind", item->kind);
        cJSON_AddNumberToObject(entry, "bytes", item->bytes);
        cJSON_AddNumberToObject(entry, "rssi_dbm", item->rssi);
        cJSON_AddNumberToObject(entry, "snr_db", item->snr);
        cJSON_AddNumberToObject(entry, "received_ms", item->received_us / 1000);
        cJSON_AddItemToArray(packets, entry);
    }
    cJSON *requests = cJSON_AddArrayToObject(fresh, "request_history");
    unsigned count = station.completed_index < 8 ? station.completed_index : 8;
    for (unsigned index = 0; index < count; ++index) {
        unsigned slot = (station.completed_index + 8 - 1 - index) % 8;
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "id", station.completed[slot].id);
        cJSON_AddNumberToObject(entry, "opcode", station.completed[slot].opcode);
        cJSON_AddNumberToObject(entry, "argument", station.completed[slot].argument);
        cJSON_AddNumberToObject(entry, "result", station.completed[slot].result);
        cJSON_AddStringToObject(entry, "status", station.completed[slot].status);
        cJSON_AddNumberToObject(entry, "finished_ms", station.completed[slot].finished_us / 1000);
        cJSON_AddItemToArray(requests, entry);
    }
    cJSON *previous = NULL;
    if (snapshot_mutex && xSemaphoreTake(snapshot_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        previous = published_status;
        published_status = fresh;
        xSemaphoreGive(snapshot_mutex);
    } else {
        cJSON_Delete(fresh);
    }
    cJSON_Delete(previous);
}

static cJSON *web_status_snapshot(bool live, int64_t snapshot_us)
{
    static const char *const fields[] = {
        "uptime_ms", "radio_ready", "key_configured", "protocol_version", "cipher", "nonce_ready",
        "uplink_state", "session", "peer_session", "last_sequence", "last_rx_age_ms", "rssi_dbm", "snr_db",
        "rx_valid", "rx_missing", "expected_period_ms", "initial_stage", "initial_error", "telemetry_cache_error",
    };
    cJSON *stats = NULL;
    if (snapshot_mutex && xSemaphoreTake(snapshot_mutex, pdMS_TO_TICKS(30)) == pdTRUE) {
        if (published_status && !live) stats = cJSON_Duplicate(published_status, true);
        else if (published_status) {
            stats = cJSON_CreateObject();
            for (size_t index = 0; stats && index < sizeof(fields) / sizeof(fields[0]); ++index) {
                const cJSON *item = cJSON_GetObjectItemCaseSensitive(published_status, fields[index]);
                if (!item) continue;
                cJSON *copy = cJSON_Duplicate(item, true);
                if (!copy || !cJSON_AddItemToObject(stats, fields[index], copy)) {
                    cJSON_Delete(copy); cJSON_Delete(stats); stats = NULL;
                }
            }
        }
        xSemaphoreGive(snapshot_mutex);
    }
    /* A failed compact status copy is unavailable, not an empty "healthy" reply. */
    if (!stats && !live) stats = cJSON_CreateObject();
    if (!stats) return NULL;
    cJSON *uptime = cJSON_GetObjectItemCaseSensitive(stats, "uptime_ms");
    const double now_ms = snapshot_us / 1000.0;
    const double advance_ms = cJSON_IsNumber(uptime) ? fmax(0, now_ms - uptime->valuedouble) : 0;
    cJSON *age = cJSON_GetObjectItemCaseSensitive(stats, "last_rx_age_ms");
    if (cJSON_IsNumber(age)) cJSON_SetNumberValue(age, age->valuedouble + advance_ms);
    if (cJSON_IsNumber(uptime)) cJSON_SetNumberValue(uptime, now_ms);
    else cJSON_AddNumberToObject(stats, "uptime_ms", now_ms);
    return stats;
}

static cJSON *web_snapshot(bool live)
{
    const int64_t snapshot_us = now_us();
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON *stats = web_status_snapshot(live, snapshot_us);
    if (!stats) { cJSON_Delete(root); return NULL; }
    cJSON_AddItemToObject(root, "station", stats);
    cJSON *telemetry = live ? telemetry_cache_live_snapshot(snapshot_us) : telemetry_cache_snapshot(snapshot_us);
    if (!telemetry) { cJSON_Delete(root); return NULL; }
    cJSON_AddItemToObject(root, "telemetry", telemetry);
    cJSON *device = cJSON_AddObjectToObject(root, "device");
    if (!device) { cJSON_Delete(root); return NULL; }
    cJSON_AddNumberToObject(device, "uptime_ms", snapshot_us / 1000.0);
    cJSON_AddBoolToObject(device, "terminal_print_enabled", app_config_terminal_print_enabled());
    cJSON_AddBoolToObject(device, "web_ready", local_web_is_ready());
    cJSON_AddStringToObject(device, "web_ip", local_web_ip());
    cJSON_AddStringToObject(device, "web_hostname", local_web_hostname());
    cJSON_AddStringToObject(device, "web_url", local_web_url());
    cJSON_AddBoolToObject(device, "mdns_ready", local_web_mdns_is_ready());
    if (local_web_mdns_error() != ESP_OK)
        cJSON_AddStringToObject(device, "mdns_error", esp_err_to_name(local_web_mdns_error()));
    cJSON_AddStringToObject(device, "ap_ssid", CONFIG_STATION_AP_SSID);
    cJSON_AddNumberToObject(device, "ap_clients", local_web_client_count());
    cJSON_AddNumberToObject(device, "free_heap_bytes", esp_get_free_heap_size());
    cJSON_AddNumberToObject(device, "minimum_free_heap_bytes", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(device, "led_gpio", CONFIG_STATION_LED_GPIO);
    cJSON_AddBoolToObject(device, "led_enabled", CONFIG_STATION_LED_ENABLED);
    cJSON_AddBoolToObject(device, "web_enabled", CONFIG_STATION_WEB_ENABLED);
    return root;
}

cJSON *station_web_snapshot(void) { return web_snapshot(false); }
cJSON *station_web_live_snapshot(void) { return web_snapshot(true); }

static bool uint32_field(const cJSON *field, uint32_t *value, bool nonzero)
{
    if (!cJSON_IsNumber(field) || !isfinite(field->valuedouble) || field->valuedouble < (nonzero ? 1 : 0) ||
        field->valuedouble > UINT32_MAX || floor(field->valuedouble) != field->valuedouble) return false;
    *value = (uint32_t)field->valuedouble;
    return true;
}

static void handle_request(const cJSON *object)
{
    uint32_t id;
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(object, "request");
    if (!uint32_field(cJSON_GetObjectItemCaseSensitive(object, "id"), &id, true) || !cJSON_IsString(kind)) {
        error_message("request_requires_positive_uint32_id_and_request_name", "request"); return;
    }
    if (!station.key_ready) { request_status(id, "key_not_configured", -1); return; }
    if (!link_crypto_nonce_available()) { request_status(id, "nonce_exhausted", -1); return; }
    if (!station.radio_ready) { request_status(id, "radio_not_ready", -1); return; }
    uint8_t opcode, argument = 0;
    if (!strcmp(kind->valuestring, "ping")) opcode = LP_OPCODE_PING;
    else if (!strcmp(kind->valuestring, "detail")) {
        opcode = LP_OPCODE_DETAIL;
        const cJSON *page = cJSON_GetObjectItemCaseSensitive(object, "page");
        const char *pages[] = {"overview", "sensors", "motors", "health"};
        if (!cJSON_IsString(page)) { request_status(id, "invalid_page", -1); return; }
        for (argument = 0; argument < 4; ++argument) if (!strcmp(page->valuestring, pages[argument])) break;
        if (argument == 4) { request_status(id, "invalid_page", -1); return; }
    } else { request_status(id, "unsupported_request", -1); return; }
    if (station.request.active) {
        const char *status = station.request.id != id ? "busy" :
            station.request.opcode == opcode && station.request.argument == argument ? "pending" : "id_conflict";
        request_status(id, status, -1); return;
    }
    for (size_t index = 0; index < 8; ++index) if (station.completed[index].id == id) {
        if (station.completed[index].opcode != opcode || station.completed[index].argument != argument)
            request_status(id, "id_conflict", -1);
        else request_status(id, station.completed[index].status, station.completed[index].result);
        return;
    }
    station.request = (request_t){.active = true, .id = id, .opcode = opcode, .argument = argument,
                                 .deadline_us = now_us() + REQUEST_LIFETIME_US};
    request_status(id, "queued", -1);
}

static void console_line(const char *line, void *context)
{
    (void)context;
    const char *end = NULL;
    cJSON *object = cJSON_ParseWithOpts(line, &end, true);
    if (!cJSON_IsObject(object)) { cJSON_Delete(object); error_message("invalid_json_object", NULL); return; }
    const cJSON *operation = cJSON_GetObjectItemCaseSensitive(object, "op");
    if (!cJSON_IsString(operation)) error_message("missing_op", NULL);
    else if (!strcmp(operation->valuestring, "request")) handle_request(object);
    else if (!strcmp(operation->valuestring, "status")) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(object, "id");
        uint32_t nonce;
        if (id && !uint32_field(id, &nonce, false)) error_message("status_id_must_be_uint32", "status");
        else status_message(id);
    } else error_message("unsupported_op", NULL);
    cJSON_Delete(object);
}

static bool sequence_is_new(uint32_t sequence)
{
    if (!station.have_sequence) { station.have_sequence = true; station.last_sequence = sequence; station.sequence_mask = 1; return true; }
    int32_t difference = (int32_t)(sequence - station.last_sequence);
    if (difference > 0) {
        station.rx_missing += (uint32_t)difference - 1;
        station.sequence_mask = difference >= 64 ? 1 : (station.sequence_mask << difference) | 1;
        station.last_sequence = sequence;
        return true;
    }
    uint32_t behind = station.last_sequence - sequence;
    if (behind >= 64) { ++station.rx_old; return false; }
    uint64_t bit = 1ULL << behind;
    if (station.sequence_mask & bit) { ++station.rx_duplicates; return false; }
    station.sequence_mask |= bit;
    if (station.rx_missing) --station.rx_missing;
    ++station.rx_old;
    return false; // Count a recovered late packet, but do not replay old state.
}

static bool accept_session(uint64_t session)
{
    if (station.have_peer && station.peer_session == session) return true;
    for (unsigned index = 0; index < station.retired_count && index < 4; ++index)
        if (station.retired_sessions[index] == session) return false;
    if (station.have_peer) {
        station.retired_sessions[station.retired_count % 4] = station.peer_session;
        ++station.retired_count;
        if (station.request.active) {
            finish_request("peer_restarted", -1);
        }
    }
    station.have_peer = true;
    station.peer_session = session;
    station.have_sequence = false;
    station.rx_missing = 0;
    station.session_rx_valid = 0;
    station.recent_count = station.recent_index = 0;
    telemetry_cache_reset(session);
    station.ack_pending = false;
    return true;
}

static void consume_result(const lp_frame_t *frame)
{
    if (frame->kind != LP_TYPE_RESULT || frame->payload_len < 6 || !station.request.active) return;
    uint32_t id = (uint32_t)frame->payload[0] | (uint32_t)frame->payload[1] << 8 |
                  (uint32_t)frame->payload[2] << 16 | (uint32_t)frame->payload[3] << 24;
    if (id != station.request.id || frame->payload[4] != station.request.opcode || frame->payload[5] > LP_RESULT_EXPIRED) return;
    finish_request("completed", frame->payload[5]);
}

static void received(const sx_event_t *event)
{
    if (!station.key_ready) { ++station.rx_invalid; return; }
    lp_frame_t frame;
    uint8_t plaintext[LP_MAX_PAYLOAD];
    int error = lp_decode(event->payload, event->length, station.key, link_crypto_open, NULL,
                          PI_NODE, GROUND_NODE, plaintext, sizeof(plaintext), &frame);
    if (error != LP_OK) {
        ++station.rx_invalid;
        if (error == LP_E_AUTH) ++station.rx_auth_errors;
        else ++station.rx_format_errors;
        return;
    }
    if (frame.kind == LP_TYPE_ACK || !accept_session(frame.session)) { ++station.rx_invalid; return; }
    int64_t now = now_us();
    station.last_rssi = event->rssi_dbm;
    station.last_snr = event->snr_db;
    // Even a duplicate may be a retry after its previous ACK was lost.
    station.ack_pending = true;
    station.ack_peer_sequence = frame.sequence;
    station.ack_peer_session = frame.session;
    station.ack_due_us = now + CONFIG_STATION_REPLY_GUARD_MS * 1000;
    station.ack_deadline_us = now + ACK_START_WINDOW_US;
    if (!sequence_is_new(frame.sequence)) return;
    station.last_rx_us = now;
    ++station.rx_valid;
    ++station.session_rx_valid;
    /* Publish the fresh radio state this iteration, after servicing its ACK. */
    next_publish_us = 0;
    cache_error = telemetry_cache_accept(&frame, now);
    if (cache_error != ESP_OK) ++station.rx_decode_errors;
    unsigned slot = station.recent_index++ % 8;
    station.recent[slot] = (packet_summary_t){.sequence=frame.sequence, .bytes=event->length,
        .kind=frame.kind, .rssi=event->rssi_dbm, .snr=event->snr_db, .received_us=now};
    if (station.recent_count < 8) ++station.recent_count;
    consume_result(&frame);
    char hex[LP_MAX_FRAME * 2 + 1];
    const char digits[] = "0123456789abcdef";
    for (size_t index = 0; index < event->length; ++index) {
        hex[index * 2] = digits[event->payload[index] >> 4];
        hex[index * 2 + 1] = digits[event->payload[index] & 15];
    }
    hex[event->length * 2] = 0;
    cJSON *object = message("rx");
    cJSON_AddStringToObject(object, "frame", hex);
    cJSON_AddNumberToObject(object, "received_ms", now / 1000);
    cJSON_AddNumberToObject(object, "age_ms", 0);
    cJSON_AddNumberToObject(object, "rssi_dbm", event->rssi_dbm);
    cJSON_AddNumberToObject(object, "snr_db", event->snr_db);
    emit(object);
}

static void send_ack(void)
{
    int64_t now = now_us();
    if (!station.ack_pending || station.radio.transmitting || now < station.ack_due_us) return;
    station.ack_pending = false;
    if (now > station.ack_deadline_us || now < station.next_tx_us) { ++station.ack_skipped; return; }
    lp_ack_t ack = {.peer_session = station.ack_peer_session};
    if (station.request.active && station.request.attempts < MAX_REQUEST_ATTEMPTS) {
        ack.request_id = station.request.id;
        ack.opcode = station.request.opcode;
        ack.argument = station.request.argument;
        int64_t remaining_ms = (station.request.deadline_us - now) / 1000;
        ack.ttl_ms = remaining_ms < 1 ? 1 : remaining_ms > 60000 ? 60000 : remaining_ms;
    }
    uint8_t payload[ACK_PAYLOAD_SIZE], packet[LP_MAX_FRAME], nonce[LP_NONCE_SIZE];
    if (!station.key_ready || !link_crypto_next_nonce(nonce)) {
        ++station.ack_skipped;
        if (station.request.active) finish_request("nonce_exhausted", -1);
        return;
    }
    lp_ack_pack(payload, &ack);
    lp_frame_t frame = {.kind = LP_TYPE_ACK, .src = GROUND_NODE, .dst = PI_NODE, .session = station.session,
                        .sequence = station.sequence++, .reply_to = station.ack_peer_sequence,
                        .payload_len = sizeof(payload), .payload = payload};
    size_t length;
    if (lp_encode(&frame, station.key, nonce, link_crypto_seal, NULL, packet, sizeof(packet), &length) != LP_OK) { ++station.rx_invalid; return; }
    esp_err_t error = sx1278_transmit(&station.radio, packet, length);
    if (error != ESP_OK) { ++station.spi_errors; return; }
    station.tx_started_us = now;
    station.tx_airtime_us = sx1278_airtime_us(&station.radio.config, length);
    ++station.tx_count;
    if (ack.opcode != LP_OPCODE_NONE) {
        ++station.request.attempts;
        request_status(station.request.id, "transmitted", -1);
    }
}

esp_err_t station_start(void)
{
    memset(&station, 0, sizeof(station));
    snapshot_mutex = xSemaphoreCreateMutex();
    cache_error = telemetry_cache_init();
    station.session = link_crypto_session();
    station.key_ready = link_crypto_parse_key(STATION_SHARED_KEY_HEX, station.key);
    if (!station.key_ready) {
        station.initial_error = ESP_ERR_INVALID_ARG;
        station.initial_stage = "compiled_key_invalid";
    } else if (!link_crypto_nonce_available()) {
        station.initial_error = ESP_ERR_INVALID_STATE;
        station.initial_stage = "crypto_init";
    }
    esp_err_t error = nvs_flash_init();
    station.storage_ready = error == ESP_OK;
    // NVS supports Wi-Fi/PHY infrastructure only. Never erase existing flash.
    if (!station.storage_ready) { station.initial_error = error; station.initial_stage = "nvs_init"; }
    sx1278_config_t config = {
        .mosi = CONFIG_STATION_SPI_MOSI, .miso = CONFIG_STATION_SPI_MISO, .sclk = CONFIG_STATION_SPI_SCLK,
        .cs = CONFIG_STATION_SPI_CS, .reset = CONFIG_STATION_RESET,
        .frequency_hz = CONFIG_STATION_FREQUENCY_HZ, .bandwidth_hz = CONFIG_STATION_BANDWIDTH_HZ,
        .sf = CONFIG_STATION_SF, .coding_rate = CONFIG_STATION_CODING_RATE,
        .sync_word = CONFIG_STATION_SYNC_WORD, .preamble = CONFIG_STATION_PREAMBLE, .tx_power_dbm = CONFIG_STATION_TX_POWER_DBM,
    };
    // Keep a valid profile for status even if SPI initialisation fails.
    station.radio.config = config;
    error = sx1278_open(&station.radio, &config);
    station.radio_ready = error == ESP_OK;
    if (error != ESP_OK) { station.initial_error = error; station.initial_stage = "sx1278_init"; }
    station.next_status_us = now_us();
    station.next_radio_retry_us = now_us() + 5000000;
    publish_web_status();
    return station.initial_error;
}

void station_poll(void)
{
    usb_console_poll(console_line, NULL);
    int64_t now = now_us();
    if (station.request.active && now >= station.request.deadline_us) {
        finish_request("expired_outcome_unknown", -1);
    }
    if (!station.radio_ready && now >= station.next_radio_retry_us) {
        sx1278_config_t config = station.radio.config;
        sx1278_close(&station.radio);
        esp_err_t error = sx1278_open(&station.radio, &config);
        station.radio_ready = error == ESP_OK;
        if (error == ESP_OK && station.storage_ready && station.key_ready && link_crypto_nonce_available()) station.initial_error = ESP_OK;
        else if (error != ESP_OK) { station.initial_error = error; station.initial_stage = "sx1278_retry"; }
        station.next_radio_retry_us = now_us() + 5000000;
    }
    if (station.radio_ready) {
        sx_event_t event;
        esp_err_t error = sx1278_poll(&station.radio, &event);
        if (error != ESP_OK) {
            ++station.spi_errors;
            station.radio_ready = false;
            station.initial_error = error;
            station.initial_stage = "sx1278_poll";
            station.next_radio_retry_us = now + 5000000;
        } else if (event.kind == SX_EVENT_RX) received(&event);
        else if (event.kind == SX_EVENT_CRC_ERROR) ++station.rx_crc;
        else if (event.kind == SX_EVENT_TX_DONE || event.kind == SX_EVENT_TX_TIMEOUT) {
            if (event.kind == SX_EVENT_TX_TIMEOUT) ++station.tx_timeout;
            int64_t elapsed = now - station.tx_started_us;
            uint32_t charge = elapsed > station.tx_airtime_us ? elapsed : station.tx_airtime_us;
            station.last_tx_us = now;
            station.next_tx_us = now + (int64_t)charge * (1000 - CONFIG_STATION_DUTY_PERMILLE) / CONFIG_STATION_DUTY_PERMILLE;
        }
        if (station.radio_ready) send_ack();
    }
    if (now >= station.next_status_us) {
        status_message(NULL);
        station.next_status_us = now + 2000000;
    }
    status_led_update(led_state(), now);
    if (now >= next_publish_us) {
        publish_web_status();
        next_publish_us = now + 500000;
    }
    if (now >= next_debug_us) {
        ESP_LOGI(TAG, "RF=%s key=%s link=%s rx=%" PRIu64 " gaps=%" PRIu64 " tx=%" PRIu64 " crc=%" PRIu64 " auth=%" PRIu64 " decode=%" PRIu64 " heap=%" PRIu32,
            station.radio_ready ? "ready" : "retrying", station.key_ready ? "loaded" : "invalid", uplink_state(),
            station.rx_valid, station.rx_missing, station.tx_count, station.rx_crc,
            station.rx_auth_errors, station.rx_decode_errors, esp_get_free_heap_size());
        next_debug_us = now + 5000000;
    }
}
