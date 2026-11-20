#include "local_web.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "app_config.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_log.h"
#include "lwip/ip4_addr.h"
#include "web_origin.h"

static atomic_bool web_ready;
static atomic_uint ap_clients;
static atomic_bool mdns_ready;
static atomic_int mdns_error = ESP_ERR_INVALID_STATE;
static const char ap_ip[] = LOCAL_WEB_IP;

bool local_web_is_ready(void) { return atomic_load(&web_ready); }
uint32_t local_web_client_count(void) { return atomic_load(&ap_clients); }
const char *local_web_ip(void) { return ap_ip; }
const char *local_web_hostname(void) { return LOCAL_WEB_HOST; }
const char *local_web_url(void) { return LOCAL_WEB_URL; }
bool local_web_mdns_is_ready(void) { return atomic_load(&mdns_ready); }
esp_err_t local_web_mdns_error(void) { return atomic_load(&mdns_error); }

#if CONFIG_STATION_WEB_ENABLED

#include "mdns.h"

static httpd_handle_t server;
static esp_netif_t *ap_netif;
static local_web_snapshot_fn snapshot_provider;
static local_web_snapshot_fn live_snapshot_provider;
static esp_event_handler_instance_t wifi_events;
/* ESP-IDF EMBED_TXTFILES names these symbols from the file basename. */
extern const char html_start[] asm("_binary_index_html_start");
extern const char html_end[] asm("_binary_index_html_end");

static void response_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "same-origin");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
        "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
        "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
}

static esp_err_t json_error(httpd_req_t *req, const char *status, const char *body)
{
    response_headers(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    /* Invalid POSTs can leave unread input. Never reuse that connection. */
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t page_get(httpd_req_t *req)
{
    response_headers(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    size_t length = (size_t)(html_end - html_start);
    if (length && html_start[length - 1] == '\0') --length;
    return httpd_resp_send(req, html_start, length);
}

static esp_err_t snapshot_get(httpd_req_t *req)
{
    local_web_snapshot_fn provider = req->user_ctx ? live_snapshot_provider : snapshot_provider;
    cJSON *snapshot = provider ? provider() : NULL;
    if (!snapshot) return json_error(req, "503 Service Unavailable", "{\"error\":\"snapshot unavailable\"}");
    char *json = cJSON_PrintUnformatted(snapshot);
    cJSON_Delete(snapshot);
    if (!json) return json_error(req, "503 Service Unavailable", "{\"error\":\"snapshot allocation failed\"}");
    response_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t result = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return result;
}

/* Validate Host as well as Origin. The friendly mDNS name and fixed AP address
 * are both valid entry points, but cross-origin changes remain forbidden. */
static bool same_origin(httpd_req_t *req)
{
    char host[32], origin[48], site[24];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) return false;
    bool has_site = httpd_req_get_hdr_value_len(req, "Sec-Fetch-Site") != 0;
    if (has_site && httpd_req_get_hdr_value_str(req, "Sec-Fetch-Site", site, sizeof(site)) != ESP_OK) return false;
    return web_origin_allowed(host, origin, has_site ? site : NULL);
}

static void start_mdns(void)
{
    esp_err_t error = mdns_init();
    if (error == ESP_OK) {
        error = mdns_hostname_set(LOCAL_WEB_HOSTNAME);
        if (error == ESP_OK) error = mdns_instance_name_set("H743 LoRa Ground Station");
        if (error == ESP_OK) {
            mdns_txt_item_t text[] = {{"path", "/"}};
            error = mdns_service_add(NULL, "_http", "_tcp", 80, text, 1);
        }
        if (error != ESP_OK) mdns_free();
    }
    atomic_store(&mdns_error, error);
    atomic_store(&mdns_ready, error == ESP_OK);
    if (error != ESP_OK) {
        ESP_LOGW("local_web", "mDNS unavailable: %s; use http://%s", esp_err_to_name(error), ap_ip);
    }
}

static esp_err_t terminal_post(httpd_req_t *req)
{
    char content_type[48];
    if (!same_origin(req)) return json_error(req, "403 Forbidden", "{\"error\":\"same origin required\"}");
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        (strcmp(content_type, "application/json") && strcmp(content_type, "application/json; charset=utf-8")))
        return json_error(req, "415 Unsupported Media Type", "{\"error\":\"application/json required\"}");
    if (req->content_len < 1 || req->content_len > 64)
        return json_error(req, "413 Payload Too Large", "{\"error\":\"body must be 1 to 64 bytes\"}");
    char body[65];
    size_t received = 0;
    while (received < req->content_len) {
        int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0) return json_error(req, "400 Bad Request", "{\"error\":\"incomplete request\"}");
        received += (size_t)count;
    }
    body[received] = '\0';
    /* Require the entire body, rejecting embedded NULs and trailing data. */
    const char *end = NULL;
    cJSON *parsed = cJSON_ParseWithLengthOpts(body, received + 1, &end, true);
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(parsed, "enabled");
    bool valid = cJSON_IsObject(parsed) && cJSON_GetArraySize(parsed) == 1 &&
                 cJSON_IsBool(enabled) && !memchr(body, '\0', received);
    if (!valid) {
        cJSON_Delete(parsed);
        return json_error(req, "400 Bad Request", "{\"error\":\"expected one boolean enabled field\"}");
    }
    app_config_set_terminal_print(cJSON_IsTrue(enabled));
    cJSON_Delete(parsed);
    response_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, app_config_terminal_print_enabled() ?
        "{\"terminal_print_enabled\":true}" : "{\"terminal_print_enabled\":false}");
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        atomic_fetch_add(&ap_clients, 1);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        unsigned count = atomic_load(&ap_clients);
        if (count) atomic_fetch_sub(&ap_clients, 1);
    } else if (id == WIFI_EVENT_AP_STOP) {
        atomic_store(&ap_clients, 0);
        atomic_store(&web_ready, false);
        atomic_store(&mdns_ready, false);
    } else if (id == WIFI_EVENT_AP_START) {
        atomic_store(&mdns_ready, atomic_load(&mdns_error) == ESP_OK);
    }
}

static esp_err_t start_http(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 10240;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    esp_err_t error = httpd_start(&server, &config);
    if (error != ESP_OK) return error;
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/api/telemetry", .method = HTTP_GET, .handler = snapshot_get},
        {.uri = "/api/live", .method = HTTP_GET, .handler = snapshot_get, .user_ctx = (void *)1},
        {.uri = "/api/status", .method = HTTP_GET, .handler = snapshot_get},
        {.uri = "/api/terminal", .method = HTTP_POST, .handler = terminal_post},
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        error = httpd_register_uri_handler(server, &handlers[i]);
        if (error != ESP_OK) { httpd_stop(server); server = NULL; return error; }
    }
    return ESP_OK;
}

esp_err_t local_web_start(local_web_snapshot_fn snapshot_fn, local_web_snapshot_fn live_snapshot_fn)
{
    if (!snapshot_fn || !live_snapshot_fn) return ESP_ERR_INVALID_ARG;
    if (server) return ESP_ERR_INVALID_STATE;
    const size_t password_len = strlen(CONFIG_STATION_AP_PASSWORD);
    const size_t ssid_len = strlen(CONFIG_STATION_AP_SSID);
    if (password_len < 8 || password_len > 63 || !ssid_len || ssid_len > 32)
        return ESP_ERR_INVALID_ARG;
    esp_err_t error = esp_netif_init();
    if (error != ESP_OK) return error;
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return error;
    ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) return ESP_ERR_NO_MEM;
    error = esp_netif_set_hostname(ap_netif, LOCAL_WEB_HOSTNAME);
    if (error != ESP_OK) ESP_LOGW("local_web", "AP hostname: %s", esp_err_to_name(error));
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    /* AP settings come from Kconfig; do not persist a second copy in NVS. */
    init.nvs_enable = false;
    error = esp_wifi_init(&init);
    if (error != ESP_OK) goto netif_fail;
    error = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, &wifi_events);
    if (error != ESP_OK) goto wifi_fail;
    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, CONFIG_STATION_AP_SSID, ssid_len);
    ap.ap.ssid_len = (uint8_t)ssid_len;
    memcpy(ap.ap.password, CONFIG_STATION_AP_PASSWORD, password_len);
    ap.ap.channel = CONFIG_STATION_AP_CHANNEL;
    ap.ap.max_connection = CONFIG_STATION_AP_MAX_CLIENTS;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.capable = true;
    ap.ap.pmf_cfg.required = false;
    error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (error == ESP_OK) error = esp_wifi_set_mode(WIFI_MODE_AP);
    if (error == ESP_OK) error = esp_wifi_set_config(WIFI_IF_AP, &ap);
    memset(&ap, 0, sizeof(ap));
    if (error != ESP_OK) goto events_fail;
    error = esp_wifi_start();
    if (error != ESP_OK) goto events_fail;
    /* API units are quarter dBm. The driver maps this to its supported level. */
    error = esp_wifi_set_max_tx_power(34);
    if (error != ESP_OK) goto started_fail;
    snapshot_provider = snapshot_fn;
    live_snapshot_provider = live_snapshot_fn;
    error = start_http();
    if (error != ESP_OK) goto started_fail;
    /* Discovery is optional: an mDNS failure must not disable the HTTP server
     * or interfere with the independent LoRa task. The IP stays usable. */
    start_mdns();
    atomic_store(&web_ready, true);
    return ESP_OK;

started_fail:
    esp_wifi_stop();
events_fail:
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_events);
wifi_fail:
    esp_wifi_deinit();
netif_fail:
    esp_netif_destroy_default_wifi(ap_netif);
    ap_netif = NULL;
    return error;
}

#else

esp_err_t local_web_start(local_web_snapshot_fn snapshot_fn, local_web_snapshot_fn live_snapshot_fn)
{
    (void)snapshot_fn;
    (void)live_snapshot_fn;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
