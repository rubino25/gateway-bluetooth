// Pagina web di gestione: file statici incorporati e API JSON sotto /api/.
//
// Sicurezza:
// - nessuna password predefinita: al primo avvio la imposta l'utente (/api/setup);
// - sessione in un cookie casuale HttpOnly + SameSite=Strict, tenuta solo in RAM
//   (un riavvio chiude tutte le sessioni);
// - limite ai tentativi sulla password (blocco crescente dopo 5 errori);
// - tutte le API tranne /api/session, /api/setup e /api/login richiedono la sessione;
// - ogni POST deve avere un'intestazione Origin uguale all'host richiesto.
//
// Tutti gli handler girano nell'unico task di esp_http_server: lo stato di sessioni
// e tentativi non ha bisogno di lock. Solo il buffer dei log è condiviso con gli
// altri task. Le stesse API sono simulate da firmware/tools/mock_server.py.

#include "web_ui.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_chip_info.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "eth.h"
#include "gw_platform.h"
#include "led.h"
#include "mg24_bridge.h"
#include "net_link.h"
#include "settings.h"

static const char *TAG = "web_ui";

#define COOKIE_NAME "gw_session"
#define SESSION_COUNT 4
#define SESSION_TOKEN_LEN 32 // 16 byte casuali in esadecimale
#define SESSION_IDLE_US (30LL * 60 * 1000000)

#define PASSWORD_MIN 8
#define PASSWORD_MAX 64
#define LOGIN_FREE_ATTEMPTS 5
#define LOGIN_LOCK_BASE_S 30
#define LOGIN_LOCK_MAX_S (15 * 60)

#define MAX_JSON_BODY 1024
#define OTA_CHUNK 4096
#define RUMBLE_MAX_MS 2000
#define RESTART_DELAY_MS 1000

// Deve essere una potenza di due: la posizione nell'anello è il contatore dei
// byte modulo la dimensione, e resta coerente anche quando il contatore va in wrap.
#define LOG_RING_SIZE 8192
#define LOG_LINE_MAX 256

#define STATUS_400 "400 Bad Request"
#define STATUS_401 "401 Unauthorized"
#define STATUS_403 "403 Forbidden"
#define STATUS_404 "404 Not Found"
#define STATUS_409 "409 Conflict"
#define STATUS_413 "413 Content Too Large"
#define STATUS_429 "429 Too Many Requests"
#define STATUS_500 "500 Internal Server Error"

// --- File incorporati (vedi EMBED_TXTFILES / EMBED_FILES in CMakeLists.txt) ---

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char app_css_start[] asm("_binary_app_css_start");
extern const char app_css_end[] asm("_binary_app_css_end");
extern const char app_js_start[] asm("_binary_app_js_start");
extern const char app_js_end[] asm("_binary_app_js_end");
extern const char dongle_png_start[] asm("_binary_dongle_png_start");
extern const char dongle_png_end[] asm("_binary_dongle_png_end");

typedef struct {
    const char *uri;
    const char *type;
    const char *start;
    const char *end;
    bool text; // i file EMBED_TXTFILES hanno un terminatore in più da non inviare
} static_file_t;

static const static_file_t s_static_files[] = {
    {"/", "text/html; charset=utf-8", index_html_start, index_html_end, true},
    {"/app.css", "text/css; charset=utf-8", app_css_start, app_css_end, true},
    {"/app.js", "application/javascript; charset=utf-8", app_js_start, app_js_end, true},
    {"/dongle.png", "image/png", dongle_png_start, dongle_png_end, false},
};

// --- Stato ---------------------------------------------------------------------

typedef struct {
    bool used;
    char token[SESSION_TOKEN_LEN + 1];
    int64_t last_used_us;
} session_t;

static httpd_handle_t s_server;
static session_t s_sessions[SESSION_COUNT];
static uint8_t s_fail_count;
static int64_t s_lock_until_us;
static bool s_app_marked_valid;
static esp_timer_handle_t s_restart_timer;

static SemaphoreHandle_t s_log_lock;
static vprintf_like_t s_prev_vprintf;
static char s_log_ring[LOG_RING_SIZE];
static char s_log_line[LOG_LINE_MAX];
static uint32_t s_log_total; // byte scritti dall'avvio

// --- Log recenti -------------------------------------------------------------------

// Agganciato a esp_log_set_vprintf: copia ogni riga nell'anello e poi la passa
// all'uscita precedente (la seriale). Non deve mai chiamare ESP_LOGx.
static int log_hook(const char *fmt, va_list args)
{
    if (s_log_lock != NULL && !xPortInIsrContext() &&
        xSemaphoreTake(s_log_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        va_list copy;
        va_copy(copy, args);
        int n = vsnprintf(s_log_line, sizeof(s_log_line), fmt, copy);
        va_end(copy);
        if (n >= (int)sizeof(s_log_line)) {
            n = sizeof(s_log_line) - 1;
        }
        for (int i = 0; i < n; i++) {
            s_log_ring[s_log_total % LOG_RING_SIZE] = s_log_line[i];
            s_log_total++;
        }
        xSemaphoreGive(s_log_lock);
    }
    return s_prev_vprintf != NULL ? s_prev_vprintf(fmt, args) : vprintf(fmt, args);
}

// --- Risposte ------------------------------------------------------------------------

static void set_api_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
}

// Invia l'oggetto e lo libera. Un oggetto NULL (memoria finita) diventa un 500.
static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *obj)
{
    char *text = obj != NULL ? cJSON_PrintUnformatted(obj) : NULL;
    cJSON_Delete(obj);
    set_api_headers(req);
    if (text == NULL) {
        httpd_resp_set_status(req, STATUS_500);
        return httpd_resp_sendstr(req, "{\"error\":\"Out of memory\"}");
    }
    httpd_resp_set_status(req, status);
    esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    set_api_headers(req);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "error", message);
    return send_json(req, status, obj);
}

static esp_err_t send_locked(httpd_req_t *req, int retry_after_s)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "error", "Too many attempts. Try again later.");
    cJSON_AddNumberToObject(obj, "retry_after", retry_after_s);
    return send_json(req, STATUS_429, obj);
}

static esp_err_t send_esp_error(httpd_req_t *req, esp_err_t err)
{
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(req, STATUS_400, "Invalid value");
    }
    return send_error(req, STATUS_500, esp_err_to_name(err));
}

// --- Lettura del corpo e del JSON ---------------------------------------------------

static esp_err_t recv_exact(httpd_req_t *req, char *buf, size_t len)
{
    size_t got = 0;
    int timeouts = 0;
    while (got < len) {
        int n = httpd_req_recv(req, buf + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 3) {
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += (size_t)n;
        timeouts = 0;
    }
    return ESP_OK;
}

static bool json_bool(const cJSON *obj, const char *key, bool *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsBool(item)) {
        return false;
    }
    *out = cJSON_IsTrue(item);
    return true;
}

static bool item_int(const cJSON *item, int lo, int hi, int *out)
{
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    double v = item->valuedouble;
    if (v < lo || v > hi || v != (double)(int)v) {
        return false;
    }
    *out = (int)v;
    return true;
}

static bool json_int(const cJSON *obj, const char *key, int lo, int hi, int *out)
{
    return item_int(cJSON_GetObjectItemCaseSensitive(obj, key), lo, hi, out);
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

// --- Sessioni --------------------------------------------------------------------------

// Confronto a tempo costante di due token di lunghezza fissa.
static bool token_equal(const char *a, const char *b)
{
    uint8_t diff = 0;
    for (int i = 0; i < SESSION_TOKEN_LEN; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

// Ritorna l'indice della sessione del cookie, -1 se manca o è scaduta.
static int session_find(httpd_req_t *req)
{
    char token[SESSION_TOKEN_LEN + 8];
    size_t len = sizeof(token);
    if (httpd_req_get_cookie_val(req, COOKIE_NAME, token, &len) != ESP_OK ||
        strlen(token) != SESSION_TOKEN_LEN) {
        return -1;
    }
    int64_t now = esp_timer_get_time();
    int found = -1;
    for (int i = 0; i < SESSION_COUNT; i++) {
        session_t *s = &s_sessions[i];
        if (!s->used) {
            continue;
        }
        if (now - s->last_used_us > SESSION_IDLE_US) {
            s->used = false;
            continue;
        }
        if (token_equal(s->token, token)) {
            found = i;
        }
    }
    if (found >= 0) {
        s_sessions[found].last_used_us = now;
    }
    return found;
}

// Crea una sessione (riusando la più vecchia se sono tutte occupate) e scrive
// in `cookie` il valore dell'intestazione Set-Cookie. Il buffer deve restare
// valido fino all'invio della risposta.
static void session_create(char *cookie, size_t cookie_size)
{
    int slot = 0;
    for (int i = 0; i < SESSION_COUNT; i++) {
        if (!s_sessions[i].used) {
            slot = i;
            break;
        }
        if (s_sessions[i].last_used_us < s_sessions[slot].last_used_us) {
            slot = i;
        }
    }
    // esp_fill_random usa il generatore hardware: è casuale davvero quando la
    // radio è accesa, cioè sempre in questo firmware una volta partito il Bluetooth.
    uint8_t raw[SESSION_TOKEN_LEN / 2];
    esp_fill_random(raw, sizeof(raw));
    session_t *s = &s_sessions[slot];
    for (size_t i = 0; i < sizeof(raw); i++) {
        snprintf(&s->token[i * 2], 3, "%02x", raw[i]);
    }
    s->last_used_us = esp_timer_get_time();
    s->used = true;
    snprintf(cookie, cookie_size, COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Strict", s->token);
}

static void sessions_clear(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
}

// --- Limite ai tentativi sulla password ----------------------------------------------

typedef enum { PW_OK, PW_WRONG, PW_LOCKED } pw_result_t;

// Dopo LOGIN_FREE_ATTEMPTS errori di fila ogni errore blocca i tentativi per un
// tempo che raddoppia (30 s, 60 s… fino a 15 minuti). Durante il blocco la password
// non viene nemmeno controllata. Il contatore è unico per tutto il dispositivo.
static pw_result_t password_attempt(const char *plain, int *retry_after_s)
{
    int64_t now = esp_timer_get_time();
    if (now < s_lock_until_us) {
        *retry_after_s = (int)((s_lock_until_us - now) / 1000000) + 1;
        return PW_LOCKED;
    }
    if (plain != NULL && settings_password_check(plain)) {
        s_fail_count = 0;
        return PW_OK;
    }
    if (s_fail_count < UINT8_MAX) {
        s_fail_count++;
    }
    if (s_fail_count >= LOGIN_FREE_ATTEMPTS) {
        int shift = s_fail_count - LOGIN_FREE_ATTEMPTS;
        if (shift > 5) {
            shift = 5;
        }
        int lock_s = LOGIN_LOCK_BASE_S << shift;
        if (lock_s > LOGIN_LOCK_MAX_S) {
            lock_s = LOGIN_LOCK_MAX_S;
        }
        s_lock_until_us = esp_timer_get_time() + (int64_t)lock_s * 1000000;
        *retry_after_s = lock_s;
        ESP_LOGW(TAG, "troppi tentativi sulla password: blocco di %d s", lock_s);
        return PW_LOCKED;
    }
    return PW_WRONG;
}

static bool new_password_ok(const char *plain)
{
    if (plain == NULL) {
        return false;
    }
    size_t len = strlen(plain);
    return len >= PASSWORD_MIN && len <= PASSWORD_MAX;
}

// --- Riavvio differito -------------------------------------------------------------------

static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

// Riavvia dopo un attimo, così la risposta HTTP fa in tempo a partire.
static void schedule_restart(void)
{
    if (s_restart_timer == NULL) {
        const esp_timer_create_args_t args = {.callback = restart_cb, .name = "gw_restart"};
        if (esp_timer_create(&args, &s_restart_timer) != ESP_OK) {
            s_restart_timer = NULL;
        }
    }
    if (s_restart_timer == NULL ||
        esp_timer_start_once(s_restart_timer, RESTART_DELAY_MS * 1000ULL) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
        esp_restart();
    }
}

// --- API: sessione, login, password -------------------------------------------------------

typedef esp_err_t (*api_fn_t)(httpd_req_t *req, const cJSON *body, int session);

static esp_err_t api_session(httpd_req_t *req, const cJSON *body, int session)
{
    (void)body;
    (void)session;
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "setup_required", !settings_password_is_set());
    cJSON_AddBoolToObject(obj, "authenticated", session_find(req) >= 0);
    return send_json(req, HTTPD_200, obj);
}

static esp_err_t send_ok_with_new_session(httpd_req_t *req)
{
    char cookie[96];
    session_create(cookie, sizeof(cookie));
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    return send_ok(req);
}

static esp_err_t api_setup(httpd_req_t *req, const cJSON *body, int session)
{
    (void)session;
    if (settings_password_is_set()) {
        return send_error(req, STATUS_403, "Password already set");
    }
    const char *password = json_str(body, "password");
    if (!new_password_ok(password)) {
        return send_error(req, STATUS_400, "Password must be 8 to 64 characters");
    }
    esp_err_t err = settings_password_set(password);
    if (err != ESP_OK) {
        return send_esp_error(req, err);
    }
    ESP_LOGI(TAG, "password impostata al primo avvio");
    return send_ok_with_new_session(req);
}

static esp_err_t api_login(httpd_req_t *req, const cJSON *body, int session)
{
    (void)session;
    if (!settings_password_is_set()) {
        return send_error(req, STATUS_403, "Setup required");
    }
    int retry = 0;
    switch (password_attempt(json_str(body, "password"), &retry)) {
    case PW_OK:
        return send_ok_with_new_session(req);
    case PW_LOCKED:
        return send_locked(req, retry);
    default:
        return send_error(req, STATUS_401, "Wrong password");
    }
}

static esp_err_t api_logout(httpd_req_t *req, const cJSON *body, int session)
{
    (void)body;
    s_sessions[session].used = false;
    httpd_resp_set_hdr(req, "Set-Cookie", COOKIE_NAME "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    return send_ok(req);
}

static esp_err_t api_password(httpd_req_t *req, const cJSON *body, int session)
{
    (void)session;
    int retry = 0;
    pw_result_t res = password_attempt(json_str(body, "current"), &retry);
    if (res == PW_LOCKED) {
        return send_locked(req, retry);
    }
    if (res != PW_OK) {
        return send_error(req, STATUS_403, "Current password is wrong");
    }
    const char *next = json_str(body, "new");
    if (!new_password_ok(next)) {
        return send_error(req, STATUS_400, "Password must be 8 to 64 characters");
    }
    esp_err_t err = settings_password_set(next);
    if (err != ESP_OK) {
        return send_esp_error(req, err);
    }
    // Le altre sessioni decadono; questa viene sostituita da una nuova.
    sessions_clear();
    ESP_LOGI(TAG, "password cambiata");
    return send_ok_with_new_session(req);
}

// --- API: stato ---------------------------------------------------------------------------

static void format_mac(const uint8_t *mac, char *out, size_t out_size)
{
    snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static const char *reset_reason_text(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "Power on";
    case ESP_RST_EXT: return "External reset";
    case ESP_RST_SW: return "Software restart";
    case ESP_RST_PANIC: return "Crash";
    case ESP_RST_INT_WDT: return "Interrupt watchdog";
    case ESP_RST_TASK_WDT: return "Task watchdog";
    case ESP_RST_WDT: return "Watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep sleep";
    case ESP_RST_BROWNOUT: return "Brownout";
    default: return "Unknown";
    }
}

// Identificatore del tipo di controller per la pagina (che lo traduce in testo).
static const char *bt_kind_id(uint8_t kind)
{
    switch (kind) {
    case GW_BT_KIND_XBOX: return "xbox";
    case GW_BT_KIND_DUALSENSE: return "dualsense";
    case GW_BT_KIND_DUALSHOCK4: return "dualshock4";
    case GW_BT_KIND_OTHER: return "other";
    default: return "";
    }
}

static esp_err_t api_status(httpd_req_t *req, const cJSON *body, int session)
{
    (void)body;
    (void)session;
    gw_settings_t cfg;
    eth_status_t eth;
    gw_bt_status_t bt;
    mg24_status_t zb;
    net_link_status_t pc;
    settings_get(&cfg);
    eth_get_status(&eth);
    gw_bt_get_status(&bt);
    mg24_get_status(&zb);
    net_link_get_status(&pc);

    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char text[40];

    cJSON *root = cJSON_CreateObject();

    cJSON *dev = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(dev, "name", cfg.device_name);
    cJSON_AddStringToObject(dev, "hostname", cfg.hostname);
    cJSON_AddStringToObject(dev, "version", app->version);
    snprintf(text, sizeof(text), "%s %s", app->date, app->time);
    cJSON_AddStringToObject(dev, "build_date", text);
    cJSON_AddStringToObject(dev, "idf", app->idf_ver);
    cJSON_AddStringToObject(dev, "chip", CONFIG_IDF_TARGET);
    snprintf(text, sizeof(text), "%u.%u", (unsigned)(chip.revision / 100), (unsigned)(chip.revision % 100));
    cJSON_AddStringToObject(dev, "revision", text);
    cJSON_AddNumberToObject(dev, "cores", chip.cores);
    cJSON_AddStringToObject(dev, "flash", CONFIG_ESPTOOLPY_FLASHSIZE);
    cJSON_AddStringToObject(dev, "partition", running != NULL ? running->label : "");
    cJSON_AddNumberToObject(dev, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(dev, "free_heap", esp_get_free_heap_size());
    cJSON_AddStringToObject(dev, "reset_reason", reset_reason_text(esp_reset_reason()));

    cJSON *e = cJSON_AddObjectToObject(root, "eth");
    cJSON_AddBoolToObject(e, "link_up", eth.link_up);
    cJSON_AddBoolToObject(e, "has_ip", eth.has_ip);
    cJSON_AddBoolToObject(e, "dhcp", eth.dhcp);
    cJSON_AddStringToObject(e, "ip", eth.ip);
    cJSON_AddStringToObject(e, "netmask", eth.netmask);
    cJSON_AddStringToObject(e, "gateway", eth.gateway);
    cJSON_AddStringToObject(e, "dns", eth.dns);
    format_mac(eth.mac, text, sizeof(text));
    cJSON_AddStringToObject(e, "mac", text);

    cJSON *b = cJSON_AddObjectToObject(root, "bt");
    cJSON_AddBoolToObject(b, "connected", bt.connected);
    cJSON_AddBoolToObject(b, "pairing", bt.pairing_enabled);
    cJSON_AddStringToObject(b, "name", bt.connected ? bt.name : "");
    text[0] = '\0';
    if (bt.connected) {
        format_mac(bt.addr, text, sizeof(text));
    }
    cJSON_AddStringToObject(b, "addr", text);
    cJSON_AddNumberToObject(b, "battery", bt.connected ? bt.battery : -1);
    cJSON_AddNumberToObject(b, "bonded", bt.bonded_count);
    cJSON_AddStringToObject(b, "type", bt.connected ? bt_kind_id(bt.kind) : "");
    cJSON_AddStringToObject(b, "transport", !bt.connected ? "" : (bt.classic ? "classic" : "le"));
    // Senza BR/EDR nel firmware la pagina nasconde le istruzioni del DualSense.
#ifdef CONFIG_GW_BT_BREDR
    cJSON_AddBoolToObject(b, "classic_enabled", true);
#else
    cJSON_AddBoolToObject(b, "classic_enabled", false);
#endif

    cJSON *z = cJSON_AddObjectToObject(root, "zigbee");
    cJSON_AddBoolToObject(z, "client_connected", zb.client_connected);
    cJSON_AddStringToObject(z, "client_ip", zb.client_connected ? zb.client_ip : "");
    cJSON_AddNumberToObject(z, "connected_s", zb.client_connected ? zb.connected_s : 0);
    cJSON_AddNumberToObject(z, "baud", zb.baud);
    cJSON_AddBoolToObject(z, "bootloader", zb.bootloader_mode);
    cJSON_AddNumberToObject(z, "bytes_to_radio", zb.bytes_to_radio);
    cJSON_AddNumberToObject(z, "bytes_from_radio", zb.bytes_from_radio);
    cJSON_AddNumberToObject(z, "tcp_port", cfg.mg24_tcp_port);

    cJSON *p = cJSON_AddObjectToObject(root, "pc");
    cJSON_AddBoolToObject(p, "seen", pc.pc_seen);
    cJSON_AddStringToObject(p, "ip", pc.pc_ip);
    cJSON_AddNumberToObject(p, "tx_packets", pc.tx_packets);
    cJSON_AddNumberToObject(p, "rx_packets", pc.rx_packets);
    if (pc.last_rx_age_ms == UINT32_MAX) {
        cJSON_AddNullToObject(p, "last_rx_age_ms");
    } else {
        cJSON_AddNumberToObject(p, "last_rx_age_ms", pc.last_rx_age_ms);
    }
    cJSON_AddStringToObject(p, "host", cfg.pc_host);
    cJSON_AddNumberToObject(p, "port", cfg.pc_port);

    // Se la memoria è finita a metà, un oggetto incompleto sarebbe fuorviante.
    if (dev == NULL || e == NULL || b == NULL || z == NULL || p == NULL) {
        cJSON_Delete(root);
        root = NULL;
    }
    return send_json(req, HTTPD_200, root);
}

// --- API: impostazioni -----------------------------------------------------------------

static esp_err_t api_settings_get(httpd_req_t *req, const cJSON *body, int session)
{
    (void)body;
    (void)session;
    gw_settings_t cfg;
    settings_get(&cfg);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "device_name", cfg.device_name);
    cJSON_AddStringToObject(obj, "hostname", cfg.hostname);
    cJSON_AddBoolToObject(obj, "eth_dhcp", cfg.eth_dhcp);
    cJSON_AddStringToObject(obj, "eth_ip", cfg.eth_ip);
    cJSON_AddStringToObject(obj, "eth_netmask", cfg.eth_netmask);
    cJSON_AddStringToObject(obj, "eth_gateway", cfg.eth_gateway);
    cJSON_AddStringToObject(obj, "eth_dns", cfg.eth_dns);
    cJSON_AddStringToObject(obj, "pc_host", cfg.pc_host);
    cJSON_AddNumberToObject(obj, "pc_port", cfg.pc_port);
    cJSON_AddNumberToObject(obj, "mg24_baud", cfg.mg24_baud);
    cJSON_AddNumberToObject(obj, "mg24_tcp_port", cfg.mg24_tcp_port);
    cJSON_AddBoolToObject(obj, "led_enabled", cfg.led_enabled);
    cJSON_AddNumberToObject(obj, "bt_idle_timeout_min", cfg.bt_idle_timeout_min);
    return send_json(req, HTTPD_200, obj);
}

static bool copy_setting(char *dst, size_t dst_size, const cJSON *item)
{
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= dst_size) {
        return false;
    }
    strlcpy(dst, item->valuestring, dst_size);
    return true;
}

// Aggiornamento parziale: si cambiano solo i campi presenti. Il baud rate
// dell'MG24 non passa di qui ma da /api/zigbee/baud, che riconfigura anche la UART.
static esp_err_t api_settings_set(httpd_req_t *req, const cJSON *body, int session)
{
    (void)session;
    gw_settings_t old;
    settings_get(&old);
    gw_settings_t cfg = old;

    const cJSON *item;
    cJSON_ArrayForEach(item, body)
    {
        const char *key = item->string;
        bool ok;
        int num = 0;
        if (strcmp(key, "device_name") == 0) {
            ok = copy_setting(cfg.device_name, sizeof(cfg.device_name), item);
        } else if (strcmp(key, "hostname") == 0) {
            ok = copy_setting(cfg.hostname, sizeof(cfg.hostname), item);
        } else if (strcmp(key, "eth_ip") == 0) {
            ok = copy_setting(cfg.eth_ip, sizeof(cfg.eth_ip), item);
        } else if (strcmp(key, "eth_netmask") == 0) {
            ok = copy_setting(cfg.eth_netmask, sizeof(cfg.eth_netmask), item);
        } else if (strcmp(key, "eth_gateway") == 0) {
            ok = copy_setting(cfg.eth_gateway, sizeof(cfg.eth_gateway), item);
        } else if (strcmp(key, "eth_dns") == 0) {
            ok = copy_setting(cfg.eth_dns, sizeof(cfg.eth_dns), item);
        } else if (strcmp(key, "pc_host") == 0) {
            ok = copy_setting(cfg.pc_host, sizeof(cfg.pc_host), item);
        } else if (strcmp(key, "eth_dhcp") == 0) {
            ok = cJSON_IsBool(item);
            cfg.eth_dhcp = cJSON_IsTrue(item);
        } else if (strcmp(key, "led_enabled") == 0) {
            ok = cJSON_IsBool(item);
            cfg.led_enabled = cJSON_IsTrue(item);
        } else if (strcmp(key, "pc_port") == 0) {
            ok = item_int(item, 1, 65535, &num);
            cfg.pc_port = (uint16_t)num;
        } else if (strcmp(key, "mg24_tcp_port") == 0) {
            ok = item_int(item, 1, 65535, &num);
            cfg.mg24_tcp_port = (uint16_t)num;
        } else if (strcmp(key, "bt_idle_timeout_min") == 0) {
            // 0 = mai. Ha effetto subito, senza riavvio (vedi settings.h).
            ok = item_int(item, 0, 240, &num);
            cfg.bt_idle_timeout_min = (uint16_t)num;
        } else {
            return send_error(req, STATUS_400, "Unknown setting");
        }
        if (!ok) {
            return send_error(req, STATUS_400, "Invalid value");
        }
    }

    esp_err_t err = settings_set(&cfg);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(req, STATUS_400, "Invalid settings");
    }
    if (err != ESP_OK) {
        return send_esp_error(req, err);
    }
    if (cfg.led_enabled != old.led_enabled) {
        led_set_enabled(cfg.led_enabled);
    }

    // pc_host viene riletto da net_link senza riavvio; il resto no.
    bool restart = strcmp(cfg.hostname, old.hostname) != 0 || cfg.eth_dhcp != old.eth_dhcp ||
                   strcmp(cfg.eth_ip, old.eth_ip) != 0 || strcmp(cfg.eth_netmask, old.eth_netmask) != 0 ||
                   strcmp(cfg.eth_gateway, old.eth_gateway) != 0 || strcmp(cfg.eth_dns, old.eth_dns) != 0 ||
                   cfg.pc_port != old.pc_port || cfg.mg24_tcp_port != old.mg24_tcp_port;
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", true);
    cJSON_AddBoolToObject(obj, "restart_required", restart);
    return send_json(req, HTTPD_200, obj);
}

// --- API: controller ----------------------------------------------------------------------

static esp_err_t api_bt_pairing(httpd_req_t *req, const cJSON *body, int session)
{
    (void)session;
    bool enable;
    if (!json_bool(body, "enable", &enable)) {
        return send_error(req, STATUS_400, "Invalid 'enable'");
    }
    gw_bt_set_pairing(enable);
    return send_ok(req);
}

static esp_err_t api_bt_disconnect(httpd_req_t *req, const cJSON *body, int session)
{
    (void)body;
    (void)session;
    gw_bt_disconnect();
    return send_ok(req);
}

static esp_err_t api_bt_forget(httpd_req_t *req, const cJSON *body, int session)
{
    (void)body;
    (void)session;
    gw_bt_forget_all();
    return send_ok(req);
}

static esp_err_t api_bt_rumble(httpd_req_t *req, const cJSON *body, int session)
{
    (void)session;
    int weak, strong, duration;
    if (!json_int(body, "weak", 0, 255, &weak) || !json_int(body, "strong", 0, 255, &strong) ||
        !json_int(body, "duration_ms", 1, RUMBLE_MAX_MS, &duration)) {
        return send_error(req, STATUS_400, "Invalid vibration values");
    }
    gw_bt_status_t bt;
    gw_bt_get_status(&bt);
    if (!bt.connected) {
        return send_error(req, STATUS_409, "No controller connected");
    }
    gw_bt_rumble(0, (uint16_t)duration, (uint8_t)weak, (uint8_t)strong);
    return send_ok(req);
}

// --- API: MG24 ---------------------------------------------------------------------------

static esp_err_t api_zb_reset(httpd_req_t *req, const cJSON *body, int session)
{
    (void)body;
    (void)session;
    esp_err_t err = mg24_reset();
    return err == ESP_OK ? send_ok(req) : send_esp_error(req, err);
}

static esp_err_t api_zb_bootloader(httpd_req_t *req, const cJSON *body, int session)
{
    (void)session;
    bool enter;
    if (!json_bool(body, "enter", &enter)) {
        return send_error(req, STATUS_400, "Invalid 'enter'");
    }
    esp_err_t err = enter ? mg24_enter_bootloader() : mg24_exit_bootloader();
    return err == ESP_OK ? send_ok(req) : send_esp_error(req, err);
}

static esp_err_t api_zb_baud(httpd_req_t *req, const cJSON *body, int session)
{
    (void)session;
    int baud;
    if (!json_int(body, "baud", 1, 10000000, &baud)) {
        return send_error(req, STATUS_400, "Invalid 'baud'");
    }
    esp_err_t err = mg24_set_baud((uint32_t)baud);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(req, STATUS_400, "Unsupported baud rate");
    }
    return err == ESP_OK ? send_ok(req) : send_esp_error(req, err);
}

// --- API: dispositivo -------------------------------------------------------------------

static esp_err_t api_find(httpd_req_t *req, const cJSON *body, int session)
{
    (void)body;
    (void)session;
    led_find_device();
    return send_ok(req);
}

static esp_err_t api_reboot(httpd_req_t *req, const cJSON *body, int session)
{
    (void)body;
    (void)session;
    ESP_LOGW(TAG, "riavvio richiesto dalla pagina web");
    esp_err_t err = send_ok(req);
    schedule_restart();
    return err;
}

static esp_err_t api_factory_reset(httpd_req_t *req, const cJSON *body, int session)
{
    (void)session;
    bool confirm = false;
    if (!json_bool(body, "confirm", &confirm) || !confirm) {
        return send_error(req, STATUS_400, "Confirmation required");
    }
    esp_err_t err = settings_factory_reset();
    if (err != ESP_OK) {
        return send_esp_error(req, err);
    }
    sessions_clear();
    ESP_LOGW(TAG, "factory reset richiesto dalla pagina web");
    err = send_ok(req);
    schedule_restart();
    return err;
}

static esp_err_t api_logs(httpd_req_t *req, const cJSON *body, int session)
{
    (void)body;
    (void)session;
    uint32_t since = 0;
    char query[48];
    char value[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "since", value, sizeof(value)) == ESP_OK) {
        since = (uint32_t)strtoul(value, NULL, 10);
    }

    char *buf = malloc(LOG_RING_SIZE);
    if (buf == NULL) {
        return send_error(req, STATUS_500, "Out of memory");
    }
    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    uint32_t total = s_log_total;
    uint32_t avail = total < LOG_RING_SIZE ? total : LOG_RING_SIZE;
    // `since` vale solo se cade dentro quello che l'anello contiene ancora.
    uint32_t len = total - since;
    if (len > avail) {
        len = avail;
        since = total - avail;
    }
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = s_log_ring[(since + i) % LOG_RING_SIZE];
    }
    xSemaphoreGive(s_log_lock);

    char next[12];
    snprintf(next, sizeof(next), "%lu", (unsigned long)total);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Log-Next", next);
    esp_err_t err = httpd_resp_send(req, buf, (ssize_t)len);
    free(buf);
    return err;
}

// --- API: aggiornamento OTA ------------------------------------------------------------

// Corpo = immagine dell'applicazione così com'è (application/octet-stream).
// Controlli: dimensione rispetto alla partizione, intestazione dell'immagine e
// chip prima di cancellare la flash; esp_ota_end() verifica poi checksum e hash
// (e la firma, se il secure boot è attivo). In caso di errore la partizione di
// avvio non cambia. Se si risponde prima di aver letto tutto il corpo, il resto
// lo scarta esp_http_server alla chiusura della richiesta.
static esp_err_t api_ota(httpd_req_t *req, const cJSON *body, int session)
{
    (void)body;
    (void)session;
    const size_t header_len =
        sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    const size_t total = req->content_len;

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        return send_error(req, STATUS_500, "No OTA partition available");
    }
    if (total < header_len) {
        return send_error(req, STATUS_400, "Image too small");
    }
    if (total > part->size) {
        return send_error(req, STATUS_413, "Image larger than the OTA partition");
    }

    char *buf = malloc(OTA_CHUNK);
    if (buf == NULL) {
        return send_error(req, STATUS_500, "Out of memory");
    }

    esp_ota_handle_t handle = 0;
    bool begun = false;
    const char *fail_status = STATUS_500;
    const char *fail_text = NULL;
    char version[sizeof(((esp_app_desc_t *)0)->version)] = "";
    size_t received = 0;

    while (received < total) {
        size_t chunk = total - received < OTA_CHUNK ? total - received : OTA_CHUNK;
        if (recv_exact(req, buf, chunk) != ESP_OK) {
            // Connessione caduta: non c'è più nessuno a cui rispondere.
            if (begun) {
                esp_ota_abort(handle);
            }
            free(buf);
            ESP_LOGE(TAG, "OTA: ricezione interrotta a %u di %u byte", (unsigned)received, (unsigned)total);
            return ESP_FAIL;
        }
        if (!begun) {
            // Il primo blocco contiene intestazione e descrizione dell'applicazione.
            esp_image_header_t image;
            esp_app_desc_t desc;
            memcpy(&image, buf, sizeof(image));
            memcpy(&desc, buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(desc));
            if (image.magic != ESP_IMAGE_HEADER_MAGIC || desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
                fail_status = STATUS_400;
                fail_text = "Not an ESP32 application image";
                break;
            }
            if (image.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
                fail_status = STATUS_400;
                fail_text = "Image built for a different chip";
                break;
            }
            strlcpy(version, desc.version, sizeof(version));
            ESP_LOGI(TAG, "OTA: %u byte verso %s, versione %s", (unsigned)total, part->label, version);
            if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) {
                fail_text = "Cannot start the update";
                break;
            }
            begun = true;
        }
        if (esp_ota_write(handle, buf, chunk) != ESP_OK) {
            fail_text = "Flash write failed";
            break;
        }
        received += chunk;
    }
    free(buf);

    if (fail_text == NULL) {
        esp_err_t err = esp_ota_end(handle); // libera l'handle in ogni caso
        begun = false;
        if (err != ESP_OK) {
            fail_status = err == ESP_ERR_OTA_VALIDATE_FAILED ? STATUS_400 : STATUS_500;
            fail_text = "Image validation failed";
        } else if (esp_ota_set_boot_partition(part) != ESP_OK) {
            fail_text = "Cannot select the new firmware";
        }
    }
    if (fail_text != NULL) {
        if (begun) {
            esp_ota_abort(handle);
        }
        ESP_LOGE(TAG, "OTA fallito: %s", fail_text);
        return send_error(req, fail_status, fail_text);
    }

    ESP_LOGW(TAG, "OTA completato, riavvio");
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", true);
    cJSON_AddStringToObject(obj, "version", version);
    esp_err_t err = send_json(req, HTTPD_200, obj);
    schedule_restart();
    return err;
}

// --- Instradamento delle API -----------------------------------------------------------

#define ROUTE_PUBLIC 0x01 // non richiede la sessione
#define ROUTE_RAW 0x02    // il corpo non è JSON: lo legge l'handler

typedef struct {
    int method;
    const char *path;
    uint8_t flags;
    api_fn_t fn;
} api_route_t;

static const api_route_t s_routes[] = {
    {HTTP_GET, "/api/session", ROUTE_PUBLIC, api_session},
    {HTTP_POST, "/api/setup", ROUTE_PUBLIC, api_setup},
    {HTTP_POST, "/api/login", ROUTE_PUBLIC, api_login},
    {HTTP_POST, "/api/logout", 0, api_logout},
    {HTTP_POST, "/api/password", 0, api_password},
    {HTTP_GET, "/api/status", 0, api_status},
    {HTTP_GET, "/api/settings", 0, api_settings_get},
    {HTTP_POST, "/api/settings", 0, api_settings_set},
    {HTTP_GET, "/api/logs", 0, api_logs},
    {HTTP_POST, "/api/controller/pairing", 0, api_bt_pairing},
    {HTTP_POST, "/api/controller/disconnect", 0, api_bt_disconnect},
    {HTTP_POST, "/api/controller/forget", 0, api_bt_forget},
    {HTTP_POST, "/api/controller/rumble", 0, api_bt_rumble},
    {HTTP_POST, "/api/zigbee/reset", 0, api_zb_reset},
    {HTTP_POST, "/api/zigbee/bootloader", 0, api_zb_bootloader},
    {HTTP_POST, "/api/zigbee/baud", 0, api_zb_baud},
    {HTTP_POST, "/api/find", 0, api_find},
    {HTTP_POST, "/api/reboot", 0, api_reboot},
    {HTTP_POST, "/api/factory-reset", 0, api_factory_reset},
    {HTTP_POST, "/api/ota", ROUTE_RAW, api_ota},
};

// Una richiesta che modifica lo stato deve arrivare dalla pagina servita da
// questo stesso host: Origin = "http://" + Host. I browser inviano sempre Origin
// nelle POST; una richiesta senza Origin viene rifiutata.
static bool origin_ok(httpd_req_t *req)
{
    char origin[160];
    char host[128];
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }
    static const char scheme[] = "http://";
    if (strncasecmp(origin, scheme, sizeof(scheme) - 1) != 0) {
        return false;
    }
    return host[0] != '\0' && strcasecmp(origin + sizeof(scheme) - 1, host) == 0;
}

// Unico punto di ingresso delle API: qui stanno i controlli comuni, così nessun
// handler può dimenticarli.
static esp_err_t api_handler(httpd_req_t *req)
{
    // La prima richiesta servita dimostra che rete e server funzionano: se il
    // rollback del bootloader è attivo, l'immagine appena installata è confermata.
    if (!s_app_marked_valid) {
        s_app_marked_valid = true;
        esp_ota_mark_app_valid_cancel_rollback();
    }

    size_t path_len = strcspn(req->uri, "?");
    const api_route_t *route = NULL;
    for (size_t i = 0; i < sizeof(s_routes) / sizeof(s_routes[0]); i++) {
        if (s_routes[i].method == req->method && strlen(s_routes[i].path) == path_len &&
            strncmp(s_routes[i].path, req->uri, path_len) == 0) {
            route = &s_routes[i];
            break;
        }
    }
    if (route == NULL) {
        return send_error(req, STATUS_404, "Not found");
    }
    if (req->method == HTTP_POST && !origin_ok(req)) {
        return send_error(req, STATUS_403, "Origin check failed");
    }
    int session = -1;
    if (!(route->flags & ROUTE_PUBLIC)) {
        session = session_find(req);
        if (session < 0) {
            return send_error(req, STATUS_401, "Not authenticated");
        }
    }
    if (req->method != HTTP_POST || (route->flags & ROUTE_RAW)) {
        return route->fn(req, NULL, session);
    }

    // POST con corpo JSON: sempre un oggetto, vuoto se il corpo manca.
    cJSON *body = NULL;
    if (req->content_len > MAX_JSON_BODY) {
        return send_error(req, STATUS_413, "Request too large");
    }
    if (req->content_len == 0) {
        body = cJSON_CreateObject();
    } else {
        char *text = malloc(req->content_len);
        if (text == NULL) {
            return send_error(req, STATUS_500, "Out of memory");
        }
        if (recv_exact(req, text, req->content_len) != ESP_OK) {
            free(text);
            return ESP_FAIL;
        }
        body = cJSON_ParseWithLength(text, req->content_len);
        free(text);
    }
    if (!cJSON_IsObject(body)) {
        cJSON_Delete(body);
        return send_error(req, STATUS_400, "Invalid JSON");
    }
    esp_err_t err = route->fn(req, body, session);
    cJSON_Delete(body);
    return err;
}

// --- File statici ------------------------------------------------------------------------

static esp_err_t static_handler(httpd_req_t *req)
{
    const static_file_t *file = req->user_ctx;
    httpd_resp_set_type(req, file->type);
    // I file cambiano con il firmware: il browser deve sempre richiederli.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; "
                       "frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
    ssize_t len = file->end - file->start - (file->text ? 1 : 0);
    return httpd_resp_send(req, file->start, len);
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

// --- Avvio -------------------------------------------------------------------------------

esp_err_t web_ui_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    if (s_log_lock == NULL) {
        s_log_lock = xSemaphoreCreateMutex();
        if (s_log_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
        s_prev_vprintf = esp_log_set_vprintf(log_hook);
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192; // PBKDF2 della password, cJSON e scrittura OTA
    // BTstack gira nel task main, fissato sulla CPU0 a priorità 1: il server sta
    // sulla CPU1 e sotto net_link (5) e ponte MG24 (10), così il PBKDF2 del login
    // (~1 s di CPU) e il JSON dello stato non ritardano controller, RUMBLE e Zigbee.
    config.core_id = 1;
    config.task_priority = tskIDLE_PRIORITY + 2;
    config.max_uri_handlers = 8;
    config.max_open_sockets = 4; // il resto dei socket lwIP serve a ponte MG24 e UDP
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        s_server = NULL;
        return err;
    }

    for (size_t i = 0; i < sizeof(s_static_files) / sizeof(s_static_files[0]); i++) {
        const httpd_uri_t uri = {
            .uri = s_static_files[i].uri,
            .method = HTTP_GET,
            .handler = static_handler,
            .user_ctx = (void *)&s_static_files[i],
        };
        err = httpd_register_uri_handler(s_server, &uri);
        if (err != ESP_OK) {
            goto fail;
        }
    }
    const httpd_uri_t others[] = {
        {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler},
        {.uri = "/api/*", .method = HTTP_GET, .handler = api_handler},
        {.uri = "/api/*", .method = HTTP_POST, .handler = api_handler},
    };
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        err = httpd_register_uri_handler(s_server, &others[i]);
        if (err != ESP_OK) {
            goto fail;
        }
    }

    ESP_LOGI(TAG, "pagina web sulla porta %u%s", (unsigned)config.server_port,
             settings_password_is_set() ? "" : " (password da impostare al primo accesso)");
    return ESP_OK;

fail:
    httpd_stop(s_server);
    s_server = NULL;
    return err;
}
