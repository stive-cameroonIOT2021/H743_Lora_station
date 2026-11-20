/* Typed VFPR telemetry v1. Wire field order mirrors lora_link/telemetry.py.
 * Parsing allocates an isolated patch; a full cache copy is committed only
 * after all validation/allocations succeed. No malformed frame updates data. */
#include "telemetry_cache.h"
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TC_MAX_RECORDS 32u
#define TC_MAX_BANK 16
#define TC_EVENT_IDS 128u
#define TC_RECEIVED "_cache_received_us"
#define TC_METADATA "_cache_metadata_us"

typedef struct {
    const char *name, *parent;
    unsigned char index, kind, maximum;
} field_spec_t;
typedef struct {
    const char *name;
    const field_spec_t *fields;
    unsigned count;
    bool bank;
} record_spec_t;

static const field_spec_t fields_1[] = {
    {"system", "target", 255, 'B', 0},
    {"component", "target", 255, 'B', 0},
    {"mode", NULL, 255, 's', 24},
    {"armed", NULL, 255, '?', 0},
    {"status", "link", 255, 's', 16},
    {"heartbeat_age_s", "link", 255, 'f', 0},
    {"elapsed_s", NULL, 255, 'f', 0},
    {"received_messages", NULL, 255, 'I', 0},
    {"bad_data_messages", NULL, 255, 'I', 0},
    {"_radio_period_s", NULL, 255, 'f', 0},
    {"_air_ack_age_s", NULL, 255, 'f', 0},
    {"_air_rtt_ms", NULL, 255, 'f', 0},
    {"_air_link_status", NULL, 255, 's', 16},
};
static const field_spec_t fields_2[] = {
    {"roll_deg", NULL, 255, 'f', 0},
    {"pitch_deg", NULL, 255, 'f', 0},
    {"yaw_deg", NULL, 255, 'f', 0},
    {"roll_rate_deg_s", NULL, 255, 'f', 0},
    {"pitch_rate_deg_s", NULL, 255, 'f', 0},
    {"yaw_rate_deg_s", NULL, 255, 'f', 0},
};
static const field_spec_t fields_3[] = {
    {"latitude_deg", NULL, 255, 'd', 0},
    {"longitude_deg", NULL, 255, 'd', 0},
    {"altitude_msl_m", NULL, 255, 'f', 0},
    {"relative_altitude_m", NULL, 255, 'f', 0},
    {"north_m_s", NULL, 255, 'f', 0},
    {"east_m_s", NULL, 255, 'f', 0},
    {"down_m_s", NULL, 255, 'f', 0},
    {"heading_deg", NULL, 255, 'f', 0},
};
static const field_spec_t fields_4[] = {
    {"fix_type", NULL, 255, 'B', 0},
    {"satellites", NULL, 255, 'B', 0},
    {"hdop", NULL, 255, 'f', 0},
};
static const field_spec_t fields_5[] = {
    {"voltage_v", NULL, 255, 'f', 0},
    {"current_a", NULL, 255, 'f', 0},
    {"remaining_pct", NULL, 255, 'f', 0},
    {"load_pct", NULL, 255, 'f', 0},
};
static const field_spec_t fields_6[] = {
    {"airspeed_m_s", NULL, 255, 'f', 0},
    {"groundspeed_m_s", NULL, 255, 'f', 0},
    {"heading_deg", NULL, 255, 'f', 0},
    {"throttle_pct", NULL, 255, 'f', 0},
    {"climb_m_s", NULL, 255, 'f', 0},
    {"altitude_msl_m", NULL, 255, 'f', 0},
};
static const field_spec_t fields_7[] = {
    {"channel_count", NULL, 255, 'B', 0},
    {"rssi_raw", NULL, 255, 'B', 0},
    {NULL, "channels", 0, 'H', 0},
    {NULL, "channels", 1, 'H', 0},
    {NULL, "channels", 2, 'H', 0},
    {NULL, "channels", 3, 'H', 0},
    {NULL, "channels", 4, 'H', 0},
    {NULL, "channels", 5, 'H', 0},
    {NULL, "channels", 6, 'H', 0},
    {NULL, "channels", 7, 'H', 0},
    {NULL, "channels", 8, 'H', 0},
    {NULL, "channels", 9, 'H', 0},
    {NULL, "channels", 10, 'H', 0},
    {NULL, "channels", 11, 'H', 0},
    {NULL, "channels", 12, 'H', 0},
    {NULL, "channels", 13, 'H', 0},
    {NULL, "channels", 14, 'H', 0},
    {NULL, "channels", 15, 'H', 0},
    {NULL, "channels", 16, 'H', 0},
    {NULL, "channels", 17, 'H', 0},
};
static const field_spec_t fields_8[] = {
    {NULL, "acceleration_m_s2", 0, 'f', 0},
    {NULL, "acceleration_m_s2", 1, 'f', 0},
    {NULL, "acceleration_m_s2", 2, 'f', 0},
    {NULL, "gyro_rad_s", 0, 'f', 0},
    {NULL, "gyro_rad_s", 1, 'f', 0},
    {NULL, "gyro_rad_s", 2, 'f', 0},
    {NULL, "magnetic_field_mgauss", 0, 'f', 0},
    {NULL, "magnetic_field_mgauss", 1, 'f', 0},
    {NULL, "magnetic_field_mgauss", 2, 'f', 0},
    {"temperature_c", NULL, 255, 'f', 0},
};
static const field_spec_t fields_9[] = {
    {"absolute_hpa", NULL, 255, 'f', 0},
    {"differential_hpa", NULL, 255, 'f', 0},
    {"temperature_c", NULL, 255, 'f', 0},
};
static const field_spec_t fields_10[] = {
    {"voltage_v", NULL, 255, 'f', 0},
    {"current_a", NULL, 255, 'f', 0},
    {"remaining_pct", NULL, 255, 'f', 0},
    {"consumed_mah", NULL, 255, 'f', 0},
    {"energy_wh", NULL, 255, 'f', 0},
    {"temperature_c", NULL, 255, 'f', 0},
    {NULL, "cell_voltages_v", 0, 'f', 0},
    {NULL, "cell_voltages_v", 1, 'f', 0},
    {NULL, "cell_voltages_v", 2, 'f', 0},
    {NULL, "cell_voltages_v", 3, 'f', 0},
    {NULL, "cell_voltages_v", 4, 'f', 0},
    {NULL, "cell_voltages_v", 5, 'f', 0},
    {NULL, "cell_voltages_v", 6, 'f', 0},
    {NULL, "cell_voltages_v", 7, 'f', 0},
    {NULL, "cell_voltages_v", 8, 'f', 0},
    {NULL, "cell_voltages_v", 9, 'f', 0},
    {NULL, "cell_voltages_v", 10, 'f', 0},
    {NULL, "cell_voltages_v", 11, 'f', 0},
    {NULL, "cell_voltages_v", 12, 'f', 0},
    {NULL, "cell_voltages_v", 13, 'f', 0},
};
static const field_spec_t fields_11[] = {
    {"rpm", NULL, 255, 'I', 0},
    {"voltage_v", NULL, 255, 'f', 0},
    {"current_a", NULL, 255, 'f', 0},
    {"temperature_c", NULL, 255, 'f', 0},
    {"consumed_mah", NULL, 255, 'f', 0},
    {"sample_count", NULL, 255, 'I', 0},
};
static const field_spec_t fields_12[] = {
    {"x_m_s2", NULL, 255, 'f', 0},
    {"y_m_s2", NULL, 255, 'f', 0},
    {"z_m_s2", NULL, 255, 'f', 0},
    {NULL, "clipping", 0, 'I', 0},
    {NULL, "clipping", 1, 'I', 0},
    {NULL, "clipping", 2, 'I', 0},
};
static const field_spec_t fields_13[] = {
    {"port", NULL, 255, 'B', 0},
    {NULL, "channels", 0, 'H', 0},
    {NULL, "channels", 1, 'H', 0},
    {NULL, "channels", 2, 'H', 0},
    {NULL, "channels", 3, 'H', 0},
    {NULL, "channels", 4, 'H', 0},
    {NULL, "channels", 5, 'H', 0},
    {NULL, "channels", 6, 'H', 0},
    {NULL, "channels", 7, 'H', 0},
    {NULL, "channels", 8, 'H', 0},
    {NULL, "channels", 9, 'H', 0},
    {NULL, "channels", 10, 'H', 0},
    {NULL, "channels", 11, 'H', 0},
    {NULL, "channels", 12, 'H', 0},
    {NULL, "channels", 13, 'H', 0},
    {NULL, "channels", 14, 'H', 0},
    {NULL, "channels", 15, 'H', 0},
};
static const field_spec_t fields_14[] = {
    {"flags", NULL, 255, 'I', 0},
    {"velocity_variance", NULL, 255, 'f', 0},
    {"horizontal_variance", NULL, 255, 'f', 0},
    {"vertical_variance", NULL, 255, 'f', 0},
    {"compass_variance", NULL, 255, 'f', 0},
    {"terrain_variance", NULL, 255, 'f', 0},
};
static const field_spec_t fields_15[] = {
    {"present", NULL, 255, 'I', 0},
    {"enabled", NULL, 255, 'I', 0},
    {"healthy", NULL, 255, 'I', 0},
    {"unhealthy_mask", NULL, 255, 'I', 0},
};
static const field_spec_t fields_16[] = {
    {"vcc_v", NULL, 255, 'f', 0},
    {"servo_v", NULL, 255, 'f', 0},
    {"flags", NULL, 255, 'I', 0},
};
static const field_spec_t fields_17[] = {
    {"free_bytes", NULL, 255, 'I', 0},
};
static const field_spec_t fields_18[] = {
    {"id", NULL, 255, 'B', 0},
    {"temperature_c", NULL, 255, 'f', 0},
    {"voltage_v", NULL, 255, 'f', 0},
    {"voltage_min_v", NULL, 255, 'f', 0},
    {"voltage_max_v", NULL, 255, 'f', 0},
};
static const field_spec_t fields_19[] = {
    {"north_m", NULL, 255, 'f', 0},
    {"east_m", NULL, 255, 'f', 0},
    {"down_m", NULL, 255, 'f', 0},
    {"north_m_s", NULL, 255, 'f', 0},
    {"east_m_s", NULL, 255, 'f', 0},
    {"down_m_s", NULL, 255, 'f', 0},
};
static const field_spec_t fields_20[] = {
    {"roll_deg", NULL, 255, 'f', 0},
    {"pitch_deg", NULL, 255, 'f', 0},
    {"bearing_deg", NULL, 255, 'f', 0},
    {"target_bearing_deg", NULL, 255, 'f', 0},
    {"waypoint_distance_m", NULL, 255, 'f', 0},
    {"altitude_error_m", NULL, 255, 'f', 0},
    {"airspeed_error_m_s", NULL, 255, 'f', 0},
    {"cross_track_error_m", NULL, 255, 'f', 0},
};
static const field_spec_t fields_21[] = {
    {"sequence", NULL, 255, 'I', 0},
    {"total", NULL, 255, 'I', 0},
};
static const field_spec_t fields_22[] = {
    {"latitude_deg", NULL, 255, 'd', 0},
    {"longitude_deg", NULL, 255, 'd', 0},
    {"altitude_msl_m", NULL, 255, 'f', 0},
};
static const field_spec_t fields_23[] = {
    {"id", NULL, 255, 'B', 0},
    {"distance_m", NULL, 255, 'f', 0},
    {"min_m", NULL, 255, 'f', 0},
    {"max_m", NULL, 255, 'f', 0},
    {"orientation", NULL, 255, 'B', 0},
};
static const field_spec_t fields_24[] = {
    {"id", NULL, 255, 'B', 0},
    {"quality", NULL, 255, 'B', 0},
    {"flow_x", NULL, 255, 'i', 0},
    {"flow_y", NULL, 255, 'i', 0},
    {"velocity_x_m_s", NULL, 255, 'f', 0},
    {"velocity_y_m_s", NULL, 255, 'f', 0},
    {"ground_distance_m", NULL, 255, 'f', 0},
};
static const field_spec_t fields_25[] = {
    {"connected", "link", 255, '?', 0},
    {"heartbeat_age_s", "link", 255, 'f', 0},
    {"_radio_period_s", NULL, 255, 'f', 0},
    {"_air_ack_age_s", NULL, 255, 'f', 0},
    {"_air_rtt_ms", NULL, 255, 'f', 0},
    {"_air_link_code", NULL, 255, 'B', 0},
    {"mode", NULL, 255, 's', 24},
    {"armed", NULL, 255, '?', 0},
    {"system", "target", 255, 'B', 0},
    {"component", "target", 255, 'B', 0},
};
static const record_spec_t schemas[26] = {
    [1] = {"metadata", fields_1, 13, false},
    [2] = {"attitude", fields_2, 6, false},
    [3] = {"position", fields_3, 8, false},
    [4] = {"gps", fields_4, 3, false},
    [5] = {"battery", fields_5, 4, false},
    [6] = {"motion", fields_6, 6, false},
    [7] = {"rc", fields_7, 20, false},
    [8] = {"imus", fields_8, 10, true},
    [9] = {"pressures", fields_9, 3, true},
    [10] = {"batteries", fields_10, 20, true},
    [11] = {"esc", fields_11, 6, true},
    [12] = {"vibration", fields_12, 6, false},
    [13] = {"outputs", fields_13, 17, false},
    [14] = {"ekf", fields_14, 6, false},
    [15] = {"health", fields_15, 4, false},
    [16] = {"power", fields_16, 3, false},
    [17] = {"memory", fields_17, 1, false},
    [18] = {"mcu", fields_18, 5, false},
    [19] = {"local_position", fields_19, 6, false},
    [20] = {"navigation", fields_20, 8, false},
    [21] = {"mission", fields_21, 2, false},
    [22] = {"home", fields_22, 3, false},
    [23] = {"distance", fields_23, 5, false},
    [24] = {"optical_flow", fields_24, 7, false},
    [25] = {"link_summary", fields_25, 10, false},
};

static const char *const link_states[] = {"connecting", "connected", "degraded", "lost", "hardware_error"};
static const char *const fix_labels[] = {"no GPS", "no fix", "2D", "3D", "DGPS", "RTK float", "RTK fixed", "static", "PPP"};
static const char *const sensor_names[] = {
    "3D_GYRO", "3D_ACCEL", "3D_MAG", "ABSOLUTE_PRESSURE", "DIFFERENTIAL_PRESSURE",
    "GPS", "OPTICAL_FLOW", "VISION_POSITION", "LASER_POSITION", "EXTERNAL_GROUND_TRUTH",
    "ANGULAR_RATE_CONTROL", "ATTITUDE_STABILIZATION", "YAW_POSITION", "Z_ALTITUDE_CONTROL",
    "XY_POSITION_CONTROL", "MOTOR_OUTPUTS", "RC_RECEIVER", "3D_GYRO2", "3D_ACCEL2", "3D_MAG2",
    "GEOFENCE", "AHRS", "TERRAIN", "REVERSE_MOTOR", "LOGGING", "BATTERY", "PROXIMITY",
    "SATCOM", "PREARM_CHECK", "OBSTACLE_AVOIDANCE", "PROPULSION", "EXTENSION_USED"
};
static SemaphoreHandle_t cache_mutex;
static cJSON *cache;
static uint64_t cache_session;
static bool session_set;
static int64_t session_started_us, last_received_us;
static bool received_any;
static uint32_t accepted_frames, invalid_frames;
static uint32_t event_ids[TC_EVENT_IDS];
static size_t event_id_count, event_id_next;

static cJSON *get(const cJSON *item, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(item, key);
}

/* Ownership of value is consumed, including on failure. */
static bool put(cJSON *object, const char *key, cJSON *value)
{
    if (!value) return false;
    /* Merged nodes already own their key. cJSON's replace helper frees that
     * key before copying its argument, so preserve it when the pointers alias.
     * All schema/internal keys are fixed strings shorter than this bound. */
    char stable_key[64];
    size_t length = strlen(key);
    if (length >= sizeof(stable_key)) { cJSON_Delete(value); return false; }
    memcpy(stable_key, key, length + 1);
    bool ok = get(object, stable_key) ? cJSON_ReplaceItemInObjectCaseSensitive(object, stable_key, value)
                                      : cJSON_AddItemToObject(object, stable_key, value);
    if (!ok) cJSON_Delete(value);
    return ok;
}

static bool append(cJSON *array, cJSON *value)
{
    if (!value) return false;
    if (cJSON_AddItemToArray(array, value)) return true;
    cJSON_Delete(value);
    return false;
}

static double numeric(const cJSON *object, const char *key, double fallback)
{
    const cJSON *value = get(object, key);
    return cJSON_IsNumber(value) ? value->valuedouble : fallback;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Strict UTF-8, including overlong/surrogate rejection. Event ASCII controls
 * become spaces, as on the Python receiver; JSON is later escaped by cJSON. */
static bool text_copy(const uint8_t *raw, size_t length, char *out, bool event)
{
    size_t i = 0, cursor = 0;
    while (i < length) {
        uint8_t first = raw[i];
        size_t count;
        uint32_t point;
        if (first < 0x80) { count = 1; point = first; }
        else if (first >= 0xc2 && first <= 0xdf) { count = 2; point = first & 31; }
        else if (first >= 0xe0 && first <= 0xef) { count = 3; point = first & 15; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 4; point = first & 7; }
        else return false;
        if (count > length - i) return false;
        for (size_t j = 1; j < count; ++j) {
            if ((raw[i + j] & 0xc0) != 0x80) return false;
            point = (point << 6) | (raw[i + j] & 63);
        }
        if ((count == 2 && point < 0x80) || (count == 3 && point < 0x800) ||
            (count == 4 && point < 0x10000) || (point >= 0xd800 && point <= 0xdfff) || point > 0x10ffff) return false;
        if (!event && point == 0) return false; /* cJSON strings cannot contain NUL. */
        if (event && (point < 32 || (point >= 0x7f && point <= 0x9f) ||
                      point == 0x2028 || point == 0x2029)) out[cursor++] = ' ';
        else { memcpy(out + cursor, raw + i, count); cursor += count; }
        i += count;
    }
    out[cursor] = 0;
    return true;
}

static esp_err_t read_value(const uint8_t *body, size_t length, size_t *cursor,
                            const field_spec_t *field, cJSON **output)
{
    size_t size;
    switch (field->kind) {
        case 'B': case '?': size = 1; break;
        case 'H': size = 2; break;
        case 'I': case 'i': case 'f': size = 4; break;
        case 'd': size = 8; break;
        case 's': size = 1; break;
        default: return ESP_ERR_INVALID_ARG;
    }
    if (*cursor > length || size > length - *cursor) return ESP_ERR_INVALID_SIZE;
    const uint8_t *p = body + *cursor;
    cJSON *value = NULL;
    if (field->kind == 's') {
        size_t count = p[0];
        if (count > field->maximum || count + 1 > length - *cursor) return ESP_ERR_INVALID_SIZE;
        char text[25];
        if (!text_copy(p + 1, count, text, false)) return ESP_ERR_INVALID_ARG;
        value = cJSON_CreateString(text);
        size += count;
    } else if (field->kind == '?') {
        if (p[0] > 1) return ESP_ERR_INVALID_ARG;
        value = cJSON_CreateBool(p[0]);
    } else {
        double number = 0;
        if (field->kind == 'B') number = p[0];
        else if (field->kind == 'H') number = le16(p);
        else if (field->kind == 'I') number = le32(p);
        else if (field->kind == 'i') {
            uint32_t bits = le32(p);
            number = bits <= INT32_MAX ? (double)bits : (double)bits - 4294967296.0;
        } else if (field->kind == 'f') {
            uint32_t bits = le32(p);
            float decoded;
            memcpy(&decoded, &bits, sizeof(decoded));
            number = decoded;
        } else {
            uint64_t bits = (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
            memcpy(&number, &bits, sizeof(number));
        }
        if (!isfinite(number)) return ESP_ERR_INVALID_ARG;
        value = cJSON_CreateNumber(number);
    }
    if (!value) return ESP_ERR_NO_MEM;
    *cursor += size;
    *output = value;
    return ESP_OK;
}

static bool set_field(cJSON *item, const field_spec_t *field, cJSON *value)
{
    cJSON *parent = item;
    if (field->parent) {
        parent = get(item, field->parent);
        if (!parent) {
            parent = field->index == 255 ? cJSON_CreateObject() : cJSON_CreateArray();
            if (!put(item, field->parent, parent)) { cJSON_Delete(value); return false; }
        }
    }
    return field->index == 255 ? put(parent, field->name, value) : append(parent, value);
}

static bool bank_id_valid(unsigned number, unsigned instance)
{
    if (number == 8 || number == 9) return instance >= 1 && instance <= 3;
    if (number == 10) return instance < TC_MAX_BANK;
    if (number == 11) return instance >= 1 && instance <= 8;
    return false;
}

static esp_err_t postprocess(unsigned number, unsigned instance, cJSON *item)
{
    if (schemas[number].bank && !put(item, "id", cJSON_CreateNumber(instance))) return ESP_ERR_NO_MEM;
    if (number == 10) {
        cJSON *cells = get(item, "cell_voltages_v");
        int length = cJSON_GetArraySize(cells);
        while (length && cJSON_IsNull(cJSON_GetArrayItem(cells, length - 1))) cJSON_DeleteItemFromArray(cells, --length);
    } else if (number == 4) {
        cJSON *fix = get(item, "fix_type");
        char fallback[12];
        const char *label = "--";
        if (cJSON_IsNumber(fix)) {
            unsigned index = (unsigned)fix->valuedouble;
            if (index < sizeof(fix_labels) / sizeof(fix_labels[0])) label = fix_labels[index];
            else { snprintf(fallback, sizeof(fallback), "%u", index); label = fallback; }
        }
        if (!put(item, "fix_label", cJSON_CreateString(label))) return ESP_ERR_NO_MEM;
    } else if (number == 15) {
        uint32_t mask = (uint32_t)numeric(item, "unhealthy_mask", 0);
        cJSON *faults = cJSON_CreateArray();
        if (!faults) return ESP_ERR_NO_MEM;
        for (unsigned bit = 0; bit < 32; ++bit) {
            if ((mask & (UINT32_C(1) << bit)) && !append(faults, cJSON_CreateString(sensor_names[bit]))) {
                cJSON_Delete(faults); return ESP_ERR_NO_MEM;
            }
        }
        if (!put(item, "unhealthy", faults)) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Consumes values. Used only on uncommitted objects, so an OOM cannot affect
 * the published cache even after earlier fields have been transferred. */
static bool merge_object(cJSON *destination, cJSON *source)
{
    while (source->child) {
        cJSON *child = source->child;
        const char *key = child->string;
        cJSON_DetachItemViaPointer(source, child);
        if (!put(destination, key, child)) return false;
    }
    return true;
}

static esp_err_t parse_records(const uint8_t *payload, size_t length, int64_t received_us, cJSON **output)
{
    cJSON *patch = cJSON_CreateObject(), *summary = NULL;
    if (!patch) return ESP_ERR_NO_MEM;
    size_t offset = 0;
    unsigned records = 0;
    esp_err_t error = ESP_OK;
    while (offset < length) {
        if (++records > TC_MAX_RECORDS || length - offset < 4) { error = ESP_ERR_INVALID_SIZE; goto fail; }
        unsigned number = payload[offset], instance = payload[offset + 1];
        size_t body_length = le16(payload + offset + 2);
        offset += 4;
        if (body_length > length - offset) { error = ESP_ERR_INVALID_SIZE; goto fail; }
        const uint8_t *body = payload + offset;
        offset += body_length;
        if (number == 0 || number >= sizeof(schemas) / sizeof(schemas[0])) continue;
        const record_spec_t *schema = &schemas[number];
        if (body_length < 9) { error = ESP_ERR_INVALID_SIZE; goto fail; }
        unsigned flags = body[0];
        uint32_t age_ms = le32(body + 1), bitmap = le32(body + 5);
        if ((flags & ~3u) || (bitmap >> schema->count) || (!(flags & 1) && bitmap)) {
            error = ESP_ERR_INVALID_ARG; goto fail;
        }
        if (schema->bank ? ((flags & 1) ? !bank_id_valid(number, instance) : instance != 255) : instance != 0) {
            error = ESP_ERR_INVALID_ARG; goto fail;
        }
        cJSON *item = cJSON_CreateObject();
        if (!item) { error = ESP_ERR_NO_MEM; goto fail; }
        size_t cursor = 9;
        for (unsigned field = 0; field < schema->count; ++field) {
            cJSON *value = NULL;
            if (bitmap & (UINT32_C(1) << field)) error = read_value(body, body_length, &cursor, &schema->fields[field], &value);
            else value = cJSON_CreateNull();
            if (error != ESP_OK || !value) { cJSON_Delete(item); if (error == ESP_OK) error = ESP_ERR_NO_MEM; goto fail; }
            if (!set_field(item, &schema->fields[field], value)) { cJSON_Delete(item); error = ESP_ERR_NO_MEM; goto fail; }
        }
        if (cursor != body_length) { cJSON_Delete(item); error = ESP_ERR_INVALID_SIZE; goto fail; }
        if (!(flags & 1)) {
            cJSON_Delete(item);
            if (number != 1 && !put(patch, schema->name, schema->bank ? cJSON_CreateArray() : cJSON_CreateNull())) {
                error = ESP_ERR_NO_MEM; goto fail;
            }
            continue;
        }
        if (number == 1 || number == 25) {
            if (number == 25) {
                cJSON *code = get(item, "_air_link_code");
                if (!cJSON_IsNumber(code) || code->valuedouble >= 5) { cJSON_Delete(item); error = ESP_ERR_INVALID_ARG; goto fail; }
                unsigned index = (unsigned)code->valuedouble;
                cJSON *link = get(item, "link");
                bool connected = cJSON_IsTrue(get(link, "connected"));
                cJSON_DeleteItemFromObjectCaseSensitive(item, "_air_link_code");
                cJSON_DeleteItemFromObjectCaseSensitive(link, "connected");
                if (!put(item, "_air_link_status", cJSON_CreateString(link_states[index])) ||
                    !put(link, "status", cJSON_CreateString(connected ? "connected" : "heartbeat_lost"))) {
                    cJSON_Delete(item); error = ESP_ERR_NO_MEM; goto fail;
                }
                cJSON_Delete(summary); summary = item;
            } else {
                if (!merge_object(patch, item)) { cJSON_Delete(item); error = ESP_ERR_NO_MEM; goto fail; }
                cJSON_Delete(item);
            }
        } else {
            error = postprocess(number, instance, item);
            if (error != ESP_OK || !put(item, "age_s", cJSON_CreateNumber(age_ms / 1000.0)) ||
                !put(item, "stale", cJSON_CreateBool(flags & 2)) ||
                !put(item, TC_RECEIVED, cJSON_CreateNumber((double)received_us))) {
                cJSON_Delete(item); if (error == ESP_OK) error = ESP_ERR_NO_MEM; goto fail;
            }
            if (schema->bank) {
                cJSON *bank = get(patch, schema->name);
                if (!bank) {
                    bank = cJSON_CreateArray();
                    if (!put(patch, schema->name, bank)) { cJSON_Delete(item); error = ESP_ERR_NO_MEM; goto fail; }
                }
                if (!append(bank, item)) { error = ESP_ERR_NO_MEM; goto fail; }
            } else if (!put(patch, schema->name, item)) { error = ESP_ERR_NO_MEM; goto fail; }
        }
    }
    if (summary && !merge_object(patch, summary)) { error = ESP_ERR_NO_MEM; goto fail; }
    cJSON_Delete(summary);
    if (get(patch, "link") && !put(patch, TC_METADATA, cJSON_CreateNumber((double)received_us))) { error = ESP_ERR_NO_MEM; goto fail_patch; }
    *output = patch;
    return ESP_OK;
fail:
    cJSON_Delete(summary);
fail_patch:
    cJSON_Delete(patch);
    return error;
}

static cJSON *new_cache(void)
{
    cJSON *root = cJSON_CreateObject(), *target = NULL, *link = NULL;
    if (!root) return NULL;
    for (unsigned i = 2; i <= 24; ++i) {
        if (!put(root, schemas[i].name, schemas[i].bank ? cJSON_CreateArray() : cJSON_CreateNull())) goto fail;
    }
    target = cJSON_CreateObject();
    if (!target || !put(target, "system", cJSON_CreateNumber(1)) || !put(target, "component", cJSON_CreateNumber(1))) goto fail;
    if (!put(root, "target", target)) { target = NULL; goto fail; }
    target = NULL;
    link = cJSON_CreateObject();
    if (!link || !put(link, "status", cJSON_CreateString("heartbeat_lost")) || !put(link, "heartbeat_age_s", cJSON_CreateNull())) goto fail;
    if (!put(root, "link", link)) { link = NULL; goto fail; }
    link = NULL;
    if (!put(root, "mode", cJSON_CreateNull()) || !put(root, "armed", cJSON_CreateNull()) ||
        !put(root, "elapsed_s", cJSON_CreateNumber(0)) || !put(root, "received_messages", cJSON_CreateNumber(0)) ||
        !put(root, "bad_data_messages", cJSON_CreateNumber(0)) || !put(root, "_radio_period_s", cJSON_CreateNumber(5)) ||
        !put(root, "_air_ack_age_s", cJSON_CreateNull()) || !put(root, "_air_rtt_ms", cJSON_CreateNull()) ||
        !put(root, "_air_link_status", cJSON_CreateString("connecting")) ||
        !put(root, "status_text", cJSON_CreateArray()) || !put(root, "last_result", cJSON_CreateNull())) goto fail;
    return root;
fail:
    cJSON_Delete(target); cJSON_Delete(link); cJSON_Delete(root); return NULL;
}

esp_err_t telemetry_cache_init(void)
{
    if (cache_mutex) return ESP_OK;
    cache_mutex = xSemaphoreCreateMutex();
    if (!cache_mutex) return ESP_ERR_NO_MEM;
    cache = new_cache();
    if (!cache) { vSemaphoreDelete(cache_mutex); cache_mutex = NULL; return ESP_ERR_NO_MEM; }
    session_started_us = esp_timer_get_time();
    return ESP_OK;
}

void telemetry_cache_reset(uint64_t session)
{
    if (!cache_mutex) return;
    cJSON *fresh = new_cache();
    xSemaphoreTake(cache_mutex, portMAX_DELAY);
    cJSON *old = cache;
    cache = fresh; /* On OOM, clear old flight data rather than retain it. */
    cache_session = session; session_set = true;
    session_started_us = esp_timer_get_time();
    received_any = false; accepted_frames = invalid_frames = 0;
    event_id_count = event_id_next = 0;
    xSemaphoreGive(cache_mutex);
    cJSON_Delete(old);
}

static bool is_bank_name(const char *name)
{
    return !strcmp(name, "imus") || !strcmp(name, "pressures") || !strcmp(name, "batteries") || !strcmp(name, "esc");
}

static bool merge_bank(cJSON *destination, cJSON *incoming)
{
    while (incoming->child) {
        cJSON *item = cJSON_DetachItemFromArray(incoming, 0);
        unsigned id = (unsigned)numeric(item, "id", 255);
        int index = 0;
        cJSON *existing = destination->child;
        while (existing && numeric(existing, "id", 255) < id) { existing = existing->next; ++index; }
        bool ok;
        if (existing && numeric(existing, "id", 255) == id) ok = cJSON_ReplaceItemInArray(destination, index, item);
        else {
            if (cJSON_GetArraySize(destination) >= TC_MAX_BANK) { cJSON_Delete(item); return false; }
            ok = cJSON_InsertItemInArray(destination, index, item);
        }
        if (!ok) { cJSON_Delete(item); return false; }
    }
    return true;
}

static bool apply_patch(cJSON *candidate, cJSON *patch)
{
    while (patch->child) {
        cJSON *child = patch->child;
        const char *key = child->string;
        cJSON *old = get(candidate, key);
        if (is_bank_name(key) && child->child && cJSON_IsArray(old)) {
            if (!merge_bank(old, child)) return false;
            cJSON_Delete(cJSON_DetachItemViaPointer(patch, child));
        } else {
            cJSON_DetachItemViaPointer(patch, child);
            if (!put(candidate, key, child)) return false;
        }
    }
    return true;
}

esp_err_t telemetry_cache_accept(const lp_frame_t *frame, int64_t received_us)
{
    if (!cache_mutex) return ESP_ERR_INVALID_STATE;
    esp_err_t error = ESP_OK;
    cJSON *patch = NULL, *event = NULL, *result = NULL;
    uint32_t event_id = 0;
    if (!frame || received_us < 0 || frame->payload_len > LP_MAX_PAYLOAD ||
        (frame->payload_len && !frame->payload)) { error = ESP_ERR_INVALID_ARG; goto rejected; }
    const uint8_t *payload = frame->payload;
    size_t length = frame->payload_len;
    if (frame->kind == LP_TYPE_TELEMETRY) error = parse_records(payload, length, received_us, &patch);
    else if (frame->kind == LP_TYPE_RESULT) {
        if (length < 6 || payload[5] > LP_RESULT_EXPIRED) { error = ESP_ERR_INVALID_ARG; goto rejected; }
        result = cJSON_CreateObject();
        if (!result || !put(result, "request_id", cJSON_CreateNumber(le32(payload))) ||
            !put(result, "opcode", cJSON_CreateNumber(payload[4])) || !put(result, "status", cJSON_CreateNumber(payload[5])) ||
            !put(result, "age_s", cJSON_CreateNumber(0)) || !put(result, TC_RECEIVED, cJSON_CreateNumber((double)received_us))) {
            error = ESP_ERR_NO_MEM; goto rejected;
        }
        /* Failed results retain their metadata, but their bytes never become
         * live readings (including cached EXPIRED diagnostic responses). */
        if (payload[5] == LP_RESULT_OK) error = parse_records(payload + 6, length - 6, received_us, &patch);
    } else if (frame->kind == LP_TYPE_EVENT) {
        if (length < 9 || length > 105 || payload[4] > 7) { error = ESP_ERR_INVALID_ARG; goto rejected; }
        char text[97];
        if (!text_copy(payload + 9, length - 9, text, true)) { error = ESP_ERR_INVALID_ARG; goto rejected; }
        event_id = le32(payload);
        event = cJSON_CreateObject();
        if (!event || !put(event, "event_id", cJSON_CreateNumber(event_id)) ||
            !put(event, "severity", cJSON_CreateNumber(payload[4])) || !put(event, "age_s", cJSON_CreateNumber(le32(payload + 5) / 1000.0)) ||
            !put(event, "text", cJSON_CreateString(text)) || !put(event, TC_RECEIVED, cJSON_CreateNumber((double)received_us))) {
            error = ESP_ERR_NO_MEM; goto rejected;
        }
    } else error = ESP_ERR_NOT_SUPPORTED;
    if (error != ESP_OK) goto rejected;

    xSemaphoreTake(cache_mutex, portMAX_DELAY);
    if (session_set && cache_session != frame->session) { error = ESP_ERR_INVALID_STATE; goto unlock; }
    cJSON *candidate = cache ? cJSON_Duplicate(cache, true) : new_cache();
    if (!candidate) { error = ESP_ERR_NO_MEM; goto unlock; }
    if (patch && !apply_patch(candidate, patch)) { cJSON_Delete(candidate); error = ESP_ERR_NO_MEM; goto unlock; }
    if (result) {
        cJSON *owned = result; result = NULL;
        if (!put(candidate, "last_result", owned)) { cJSON_Delete(candidate); error = ESP_ERR_NO_MEM; goto unlock; }
    }
    bool new_event = event != NULL;
    for (size_t i = 0; new_event && i < event_id_count; ++i) if (event_ids[i] == event_id) new_event = false;
    if (new_event) {
        cJSON *events = get(candidate, "status_text");
        cJSON *owned = event; event = NULL;
        if (!append(events, owned)) { cJSON_Delete(candidate); error = ESP_ERR_NO_MEM; goto unlock; }
        while (cJSON_GetArraySize(events) > 4) cJSON_DeleteItemFromArray(events, 0);
    }
    cJSON *old = cache;
    cache = candidate;
    if (!session_set) { cache_session = frame->session; session_set = true; session_started_us = received_us; }
    last_received_us = received_us; received_any = true; ++accepted_frames;
    if (new_event) {
        event_ids[event_id_next] = event_id;
        event_id_next = (event_id_next + 1) % TC_EVENT_IDS;
        if (event_id_count < TC_EVENT_IDS) ++event_id_count;
    }
    xSemaphoreGive(cache_mutex);
    cJSON_Delete(old); cJSON_Delete(patch); cJSON_Delete(event); cJSON_Delete(result);
    return ESP_OK;
unlock:
    ++invalid_frames;
    xSemaphoreGive(cache_mutex);
    cJSON_Delete(patch); cJSON_Delete(event); cJSON_Delete(result);
    return error;
rejected:
    cJSON_Delete(patch); cJSON_Delete(event); cJSON_Delete(result);
    xSemaphoreTake(cache_mutex, portMAX_DELAY); ++invalid_frames; xSemaphoreGive(cache_mutex);
    return error;
}

static double elapsed(int64_t now, double received)
{
    return fmax(0, ((double)now - received) / 1000000.0);
}

static bool advance_reading(cJSON *item, int64_t now_us, double period)
{
    if (!cJSON_IsObject(item)) return true;
    cJSON *age = get(item, "age_s");
    if (!cJSON_IsNumber(age)) return true;
    double seconds = age->valuedouble + elapsed(now_us, numeric(item, TC_RECEIVED, (double)now_us));
    cJSON_SetNumberValue(age, seconds);
    cJSON *stale = get(item, "stale");
    if (stale && !cJSON_IsTrue(stale) && seconds > fmax(3, period * 3)) {
        if (!put(item, "stale", cJSON_CreateBool(true))) return false;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(item, TC_RECEIVED);
    return true;
}

static cJSON *copy_live_cache(void)
{
    static const char *const fields[] = {
        "target", "mode", "armed", "link", "elapsed_s", "received_messages", "bad_data_messages",
        "_radio_period_s", "_air_ack_age_s", "_air_rtt_ms", "_air_link_status", TC_METADATA,
        "attitude", "battery", "gps", "position", "motion", "rc", "imus", "status_text",
    };
    if (!cache) return NULL;
    cJSON *output = cJSON_CreateObject();
    if (!output) return NULL;
    for (size_t index = 0; index < sizeof(fields) / sizeof(fields[0]); ++index) {
        const cJSON *item = get(cache, fields[index]);
        if (!item) continue; /* Only the private metadata timestamp is optional. */
        if (!put(output, fields[index], cJSON_Duplicate(item, true))) {
            cJSON_Delete(output); return NULL;
        }
    }
    return output;
}

static cJSON *snapshot_cache(int64_t now_us, bool live)
{
    if (!cache_mutex) return NULL;
    xSemaphoreTake(cache_mutex, portMAX_DELAY);
    cJSON *output = live ? copy_live_cache() : cache ? cJSON_Duplicate(cache, true) : NULL;
    uint64_t session = cache_session;
    bool has_session = session_set, has_rx = received_any;
    int64_t started = session_started_us, last_rx = last_received_us;
    uint32_t accepted = accepted_frames, invalid = invalid_frames;
    xSemaphoreGive(cache_mutex);
    if (!output) return NULL;
    double period = numeric(output, "_radio_period_s", 5);
    if (!isfinite(period) || period < 0.1) period = 5;
    cJSON *item;
    cJSON_ArrayForEach(item, output) {
        if (is_bank_name(item->string) || !strcmp(item->string, "status_text")) {
            cJSON *entry;
            cJSON_ArrayForEach(entry, item) if (!advance_reading(entry, now_us, period)) goto fail;
        } else if (!advance_reading(item, now_us, period)) goto fail;
    }
    if (cJSON_IsNumber(get(output, TC_METADATA))) {
        double delta = elapsed(now_us, numeric(output, TC_METADATA, (double)now_us));
        cJSON *link = get(output, "link"), *age = get(link, "heartbeat_age_s");
        if (cJSON_IsNumber(age)) {
            cJSON_SetNumberValue(age, age->valuedouble + delta);
            if (age->valuedouble > fmax(5, period * 3) && !put(link, "status", cJSON_CreateString("heartbeat_lost"))) goto fail;
        }
        age = get(output, "_air_ack_age_s");
        if (cJSON_IsNumber(age)) {
            cJSON_SetNumberValue(age, age->valuedouble + delta);
            const char *state = age->valuedouble > period * 8 ? "lost" : age->valuedouble > period * 3 ? "degraded" : NULL;
            if (state && !put(output, "_air_link_status", cJSON_CreateString(state))) goto fail;
        }
    }
    cJSON_DeleteItemFromObjectCaseSensitive(output, TC_METADATA);
    char identity[17];
    snprintf(identity, sizeof(identity), "%016" PRIx64, session);
    if (!put(output, "session", has_session ? cJSON_CreateString(identity) : cJSON_CreateNull())) goto fail;
    cJSON *diagnostics = cJSON_CreateObject();
    if (!diagnostics) goto fail;
    if (!put(diagnostics, "accepted_frames", cJSON_CreateNumber(accepted)) ||
        !put(diagnostics, "invalid_frames", cJSON_CreateNumber(invalid)) ||
        !put(diagnostics, "session_age_s", cJSON_CreateNumber(elapsed(now_us, (double)started))) ||
        !put(diagnostics, "receive_age_s", has_rx ? cJSON_CreateNumber(elapsed(now_us, (double)last_rx)) : cJSON_CreateNull())) {
        cJSON_Delete(diagnostics); goto fail;
    }
    if (!put(output, "cache", diagnostics)) goto fail;
    return output;
fail:
    cJSON_Delete(output); return NULL;
}

cJSON *telemetry_cache_snapshot(int64_t now_us) { return snapshot_cache(now_us, false); }
cJSON *telemetry_cache_live_snapshot(int64_t now_us) { return snapshot_cache(now_us, true); }
