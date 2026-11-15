// Impostazioni persistenti in NVS.
//
// - Le impostazioni sono salvate come un unico blob con versione e dimensione,
//   in un formato indipendente dalla disposizione in memoria di gw_settings_t.
// - La password della pagina web è salvata solo come PBKDF2-HMAC-SHA256 con
//   salt casuale; il confronto è a tempo costante.
// - Il timeout di inattività del controller sta in una chiave NVS a parte
//   ("bt_idle"): le impostazioni salvate prima che esistesse restano valide e
//   per quel campo si usa il default di menuconfig.
// - Due mutex: s_lock protegge lo stato in RAM ed è tenuto solo per copiarlo;
//   s_write_lock mette in fila chi scrive in NVS. Così settings_get(), chiamata
//   anche dal run loop di BTstack, non aspetta mai una scrittura in flash.
// - La derivazione PBKDF2 (fino a circa 1 s) gira in un task a bassa priorità
//   sul core che non ospita il Bluetooth: chi la chiama (il server HTTP) può
//   stare su qualunque core senza fermare BTstack.

#include "settings.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "gw"
#define KEY_CFG       "cfg"
#define KEY_PW        "pw"
#define KEY_IDLE      "bt_idle" // u16, minuti; aggiunta dopo il formato v1

#define BT_IDLE_MAX_MIN 240

// ---------------------------------------------------------------------------
// Formato salvato (versione 1). Se gw_settings_t cambia, si aggiunge una nuova
// versione qui e una conversione in load_cfg(); non si modifica questa struct.
// ---------------------------------------------------------------------------

#define CFG_BLOB_VERSION 1

typedef struct __attribute__((packed)) {
    uint16_t version;
    uint16_t size;
    char device_name[32];
    char hostname[32];
    uint8_t eth_dhcp;
    char eth_ip[16];
    char eth_netmask[16];
    char eth_gateway[16];
    char eth_dns[16];
    char pc_host[64];
    uint16_t pc_port;
    uint32_t mg24_baud;
    uint16_t mg24_tcp_port;
    uint8_t led_enabled;
} cfg_blob_v1_t;

_Static_assert(sizeof(((cfg_blob_v1_t *)0)->device_name) == sizeof(((gw_settings_t *)0)->device_name), "device_name");
_Static_assert(sizeof(((cfg_blob_v1_t *)0)->hostname) == sizeof(((gw_settings_t *)0)->hostname), "hostname");
_Static_assert(sizeof(((cfg_blob_v1_t *)0)->eth_ip) == sizeof(((gw_settings_t *)0)->eth_ip), "eth_ip");
_Static_assert(sizeof(((cfg_blob_v1_t *)0)->pc_host) == sizeof(((gw_settings_t *)0)->pc_host), "pc_host");

// Password: PBKDF2-HMAC-SHA256. Il numero di iterazioni è salvato nel record,
// così si può aumentare in futuro senza invalidare le password esistenti.
// 10000 iterazioni sono un compromesso per l'ESP32 (ordine di 1 s per verifica):
// chi chiama settings_password_check() deve limitare i tentativi.
#define PW_RECORD_VERSION 1
#define PW_ITERATIONS     10000
#define PW_ITER_MIN       1000
#define PW_ITER_MAX       1000000
#define PW_SALT_LEN       16
#define PW_HASH_LEN       32
#define PW_MIN_LEN        8
#define PW_MAX_LEN        64

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t reserved[3];
    uint32_t iterations;
    uint8_t salt[PW_SALT_LEN];
    uint8_t hash[PW_HASH_LEN];
} pw_record_t;

// Core del task PBKDF2: quello che non ospita il Bluetooth (controller e
// BTstack stanno sul core 0, vedi sdkconfig).
#if CONFIG_FREERTOS_UNICORE
#define KDF_CORE 0
#elif defined(CONFIG_BTDM_CTRL_PINNED_TO_CORE) && CONFIG_BTDM_CTRL_PINNED_TO_CORE == 1
#define KDF_CORE 0
#else
#define KDF_CORE 1
#endif
// Priorità minima sopra l'idle: usa solo il tempo libero del core e non ritarda
// il ponte Zigbee, la rete o i keepalive verso il PC.
#define KDF_TASK_PRIO  1
#define KDF_TASK_STACK 4096

static SemaphoreHandle_t s_lock;       // stato in RAM, tenuto per poco
static SemaphoreHandle_t s_write_lock; // scritture NVS, una alla volta
static SemaphoreHandle_t s_kdf_lock; // una derivazione alla volta
static SemaphoreHandle_t s_kdf_done; // dato dal task PBKDF2 a fine lavoro
static gw_settings_t s_cfg;
static pw_record_t s_pw;
static bool s_pw_set;

// ---------------------------------------------------------------------------
// Validazione
// ---------------------------------------------------------------------------

static bool is_terminated(const char *s, size_t cap)
{
    return memchr(s, '\0', cap) != NULL;
}

// Azzera i byte dopo il terminatore: il blob salvato resta deterministico e non
// porta in flash residui di memoria.
static void tidy_str(char *s, size_t cap)
{
    size_t n = strnlen(s, cap);
    if (n < cap) {
        memset(s + n, 0, cap - n);
    }
}

// IPv4 in forma "a.b.c.d", decimale, senza zeri iniziali. Risultato in ordine host.
static bool parse_ipv4(const char *s, uint32_t *out)
{
    uint32_t addr = 0;
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        const char *start = s;
        unsigned v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s - '0');
            s++;
            if (s - start > 3 || v > 255) {
                return false;
            }
        }
        if (s - start > 1 && *start == '0') {
            return false;
        }
        addr = (addr << 8) | v;
        if (i < 3) {
            if (*s != '.') {
                return false;
            }
            s++;
        }
    }
    if (*s != '\0') {
        return false;
    }
    *out = addr;
    return true;
}

static bool is_unicast(uint32_t addr)
{
    uint32_t a = addr >> 24;
    return a != 0 && a != 127 && a < 224;
}

static bool is_netmask(uint32_t m)
{
    uint32_t inv = ~m;
    return m != 0 && m != 0xFFFFFFFFu && (inv & (inv + 1)) == 0;
}

static bool is_alnum_ascii(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// Etichetta DNS (RFC 1123): lettere, cifre e '-', non all'inizio né alla fine.
static bool is_dns_label(const char *s, size_t len)
{
    if (len == 0 || len > 63 || s[0] == '-' || s[len - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!is_alnum_ascii(s[i]) && s[i] != '-') {
            return false;
        }
    }
    return true;
}

static bool is_dns_name(const char *s)
{
    while (true) {
        const char *dot = strchr(s, '.');
        size_t len = dot ? (size_t)(dot - s) : strlen(s);
        if (!is_dns_label(s, len)) {
            return false;
        }
        if (!dot) {
            return true;
        }
        s = dot + 1;
    }
}

static bool only_digits_and_dots(const char *s)
{
    for (; *s; s++) {
        if ((*s < '0' || *s > '9') && *s != '.') {
            return false;
        }
    }
    return true;
}

static bool is_printable_name(const char *s)
{
    if (s[0] == '\0' || s[0] == ' ') {
        return false;
    }
    size_t n = strlen(s);
    if (s[n - 1] == ' ') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (s[i] < 0x20 || s[i] > 0x7E) {
            return false;
        }
    }
    return true;
}

static bool is_valid_baud(uint32_t baud)
{
    return baud == 115200 || baud == 460800 || baud == 921600;
}

// Campo IP facoltativo: vuoto oppure un indirizzo unicast valido.
static bool is_optional_unicast(const char *s)
{
    uint32_t a;
    return s[0] == '\0' || (parse_ipv4(s, &a) && is_unicast(a));
}

// Ritorna NULL se tutto è valido, altrimenti il nome del campo sbagliato.
static const char *validate(const gw_settings_t *s)
{
    if (!is_terminated(s->device_name, sizeof(s->device_name)) ||
        !is_terminated(s->hostname, sizeof(s->hostname)) ||
        !is_terminated(s->eth_ip, sizeof(s->eth_ip)) ||
        !is_terminated(s->eth_netmask, sizeof(s->eth_netmask)) ||
        !is_terminated(s->eth_gateway, sizeof(s->eth_gateway)) ||
        !is_terminated(s->eth_dns, sizeof(s->eth_dns)) ||
        !is_terminated(s->pc_host, sizeof(s->pc_host))) {
        return "stringa non terminata";
    }

    if (!is_printable_name(s->device_name)) {
        return "device_name";
    }
    if (!is_dns_label(s->hostname, strlen(s->hostname))) {
        return "hostname";
    }

    uint32_t ip = 0, mask = 0, gw = 0;
    if (s->eth_dhcp) {
        // Con il DHCP i campi statici non si usano, ma se compilati devono
        // essere comunque indirizzi ben formati.
        if (!is_optional_unicast(s->eth_ip)) {
            return "eth_ip";
        }
        if (s->eth_netmask[0] != '\0' && !(parse_ipv4(s->eth_netmask, &mask) && is_netmask(mask))) {
            return "eth_netmask";
        }
    } else {
        if (!parse_ipv4(s->eth_ip, &ip) || !is_unicast(ip)) {
            return "eth_ip";
        }
        if (!parse_ipv4(s->eth_netmask, &mask) || !is_netmask(mask)) {
            return "eth_netmask";
        }
        // L'indirizzo non può essere quello di rete né il broadcast della sottorete.
        if ((ip & ~mask) == 0 || (ip & ~mask) == ~mask) {
            return "eth_ip";
        }
        if (s->eth_gateway[0] != '\0') {
            if (!parse_ipv4(s->eth_gateway, &gw) || !is_unicast(gw) ||
                (gw & mask) != (ip & mask) || gw == ip) {
                return "eth_gateway";
            }
        }
    }
    if (!is_optional_unicast(s->eth_gateway)) {
        return "eth_gateway";
    }
    if (!is_optional_unicast(s->eth_dns)) {
        return "eth_dns";
    }

    // PC: vuoto (broadcast), IPv4 (anche broadcast di sottorete) o nome host.
    if (s->pc_host[0] != '\0') {
        if (only_digits_and_dots(s->pc_host)) {
            uint32_t pc;
            if (!parse_ipv4(s->pc_host, &pc) || (pc >> 24) == 0 || (pc >> 24) == 127 ||
                ((pc >> 24) >= 224 && pc != 0xFFFFFFFFu)) {
                return "pc_host";
            }
        } else if (!is_dns_name(s->pc_host)) {
            return "pc_host";
        }
    }
    if (s->pc_port == 0) {
        return "pc_port";
    }

    if (!is_valid_baud(s->mg24_baud)) {
        return "mg24_baud";
    }
    // La porta 80 è della pagina web.
    if (s->mg24_tcp_port == 0 || s->mg24_tcp_port == 80) {
        return "mg24_tcp_port";
    }
    if (s->bt_idle_timeout_min > BT_IDLE_MAX_MIN) {
        return "bt_idle_timeout_min";
    }
    return NULL;
}

static void tidy(gw_settings_t *s)
{
    tidy_str(s->device_name, sizeof(s->device_name));
    tidy_str(s->hostname, sizeof(s->hostname));
    tidy_str(s->eth_ip, sizeof(s->eth_ip));
    tidy_str(s->eth_netmask, sizeof(s->eth_netmask));
    tidy_str(s->eth_gateway, sizeof(s->eth_gateway));
    tidy_str(s->eth_dns, sizeof(s->eth_dns));
    tidy_str(s->pc_host, sizeof(s->pc_host));
}

// ---------------------------------------------------------------------------
// Default da menuconfig
// ---------------------------------------------------------------------------

static void load_defaults(gw_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    strlcpy(s->device_name, CONFIG_GW_DEVICE_NAME, sizeof(s->device_name));
    strlcpy(s->hostname, CONFIG_GW_HOSTNAME, sizeof(s->hostname));
    s->eth_dhcp = true;
    strlcpy(s->pc_host, CONFIG_GW_PC_HOST, sizeof(s->pc_host));
    s->pc_port = (CONFIG_GW_UDP_PORT > 0 && CONFIG_GW_UDP_PORT <= 65535) ? (uint16_t)CONFIG_GW_UDP_PORT : 0;
    s->mg24_baud = (uint32_t)CONFIG_GW_MG24_DEFAULT_BAUD;
    s->mg24_tcp_port =
        (CONFIG_GW_MG24_TCP_PORT > 0 && CONFIG_GW_MG24_TCP_PORT <= 65535) ? (uint16_t)CONFIG_GW_MG24_TCP_PORT : 0;
    s->led_enabled = true;
    s->bt_idle_timeout_min = (CONFIG_GW_BT_IDLE_TIMEOUT_MIN >= 0 && CONFIG_GW_BT_IDLE_TIMEOUT_MIN <= BT_IDLE_MAX_MIN)
                                 ? (uint16_t)CONFIG_GW_BT_IDLE_TIMEOUT_MIN
                                 : 0;

    // Se menuconfig contiene valori non validi si ripiega su quelli noti, un
    // campo alla volta, così il dispositivo parte comunque.
    const char *bad;
    int guard = 0;
    while ((bad = validate(s)) != NULL && guard++ < 8) {
        ESP_LOGW(TAG, "default di menuconfig non valido (%s): uso il valore predefinito", bad);
        if (strcmp(bad, "device_name") == 0) {
            strlcpy(s->device_name, "Xbox Gateway", sizeof(s->device_name));
        } else if (strcmp(bad, "hostname") == 0) {
            strlcpy(s->hostname, "dongle-m", sizeof(s->hostname));
        } else if (strcmp(bad, "pc_host") == 0) {
            s->pc_host[0] = '\0';
        } else if (strcmp(bad, "pc_port") == 0) {
            s->pc_port = 47900;
        } else if (strcmp(bad, "mg24_baud") == 0) {
            s->mg24_baud = 115200;
        } else if (strcmp(bad, "mg24_tcp_port") == 0) {
            s->mg24_tcp_port = 6638;
        } else if (strcmp(bad, "bt_idle_timeout_min") == 0) {
            s->bt_idle_timeout_min = 0;
        } else {
            break;
        }
    }
    tidy(s);
}

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------

static void cfg_to_blob(const gw_settings_t *s, cfg_blob_v1_t *b)
{
    memset(b, 0, sizeof(*b));
    b->version = CFG_BLOB_VERSION;
    b->size = sizeof(*b);
    memcpy(b->device_name, s->device_name, sizeof(b->device_name));
    memcpy(b->hostname, s->hostname, sizeof(b->hostname));
    b->eth_dhcp = s->eth_dhcp ? 1 : 0;
    memcpy(b->eth_ip, s->eth_ip, sizeof(b->eth_ip));
    memcpy(b->eth_netmask, s->eth_netmask, sizeof(b->eth_netmask));
    memcpy(b->eth_gateway, s->eth_gateway, sizeof(b->eth_gateway));
    memcpy(b->eth_dns, s->eth_dns, sizeof(b->eth_dns));
    memcpy(b->pc_host, s->pc_host, sizeof(b->pc_host));
    b->pc_port = s->pc_port;
    b->mg24_baud = s->mg24_baud;
    b->mg24_tcp_port = s->mg24_tcp_port;
    b->led_enabled = s->led_enabled ? 1 : 0;
}

static void blob_to_cfg(const cfg_blob_v1_t *b, gw_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    memcpy(s->device_name, b->device_name, sizeof(s->device_name));
    memcpy(s->hostname, b->hostname, sizeof(s->hostname));
    s->eth_dhcp = b->eth_dhcp != 0;
    memcpy(s->eth_ip, b->eth_ip, sizeof(s->eth_ip));
    memcpy(s->eth_netmask, b->eth_netmask, sizeof(s->eth_netmask));
    memcpy(s->eth_gateway, b->eth_gateway, sizeof(s->eth_gateway));
    memcpy(s->eth_dns, b->eth_dns, sizeof(s->eth_dns));
    memcpy(s->pc_host, b->pc_host, sizeof(s->pc_host));
    s->pc_port = b->pc_port;
    s->mg24_baud = b->mg24_baud;
    s->mg24_tcp_port = b->mg24_tcp_port;
    s->led_enabled = b->led_enabled != 0;
}

static esp_err_t save_blob(const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

// Scrive solo le parti cambiate (blob v1 e/o chiave del timeout), con un solo
// commit. Nessuna scrittura se non è cambiato nulla.
static esp_err_t save_cfg(const gw_settings_t *old, const gw_settings_t *s)
{
    cfg_blob_v1_t cur, blob;
    cfg_to_blob(old, &cur);
    cfg_to_blob(s, &blob);
    bool blob_changed = memcmp(&cur, &blob, sizeof(blob)) != 0;
    bool idle_changed = old->bt_idle_timeout_min != s->bt_idle_timeout_min;
    if (!blob_changed && !idle_changed) {
        return ESP_OK;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (blob_changed) {
        err = nvs_set_blob(h, KEY_CFG, &blob, sizeof(blob));
    }
    if (err == ESP_OK && idle_changed) {
        err = nvs_set_u16(h, KEY_IDLE, s->bt_idle_timeout_min);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

// Carica impostazioni e password in s_cfg / s_pw. Chiamare prima che esistano
// altri task.
static void load_from_nvs(void)
{
    load_defaults(&s_cfg);
    memset(&s_pw, 0, sizeof(s_pw));
    s_pw_set = false;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        // ESP_ERR_NVS_NOT_FOUND al primo avvio: è normale.
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "nvs_open: %s, uso i default", esp_err_to_name(err));
        }
        return;
    }

    cfg_blob_v1_t blob;
    size_t len = sizeof(blob);
    err = nvs_get_blob(h, KEY_CFG, &blob, &len);
    if (err == ESP_OK && len == sizeof(blob) && blob.version == CFG_BLOB_VERSION && blob.size == sizeof(blob)) {
        gw_settings_t loaded;
        blob_to_cfg(&blob, &loaded);
        const char *bad = validate(&loaded);
        if (bad == NULL) {
            tidy(&loaded);
            s_cfg = loaded;
        } else {
            ESP_LOGW(TAG, "impostazioni salvate non valide (%s): uso i default", bad);
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "impostazioni salvate illeggibili o di versione sconosciuta: uso i default");
    }

    // Timeout di inattività: chiave assente (impostazioni salvate da una
    // versione precedente) = default di menuconfig, già in s_cfg.
    uint16_t idle = 0;
    err = nvs_get_u16(h, KEY_IDLE, &idle);
    if (err == ESP_OK && idle <= BT_IDLE_MAX_MIN) {
        s_cfg.bt_idle_timeout_min = idle;
    } else if (err == ESP_OK) {
        ESP_LOGW(TAG, "timeout di inattività salvato non valido (%u): uso il default", (unsigned)idle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "timeout di inattività illeggibile (%s): uso il default", esp_err_to_name(err));
    }

    pw_record_t rec;
    len = sizeof(rec);
    err = nvs_get_blob(h, KEY_PW, &rec, &len);
    if (err == ESP_OK && len == sizeof(rec) && rec.version == PW_RECORD_VERSION &&
        rec.iterations >= PW_ITER_MIN && rec.iterations <= PW_ITER_MAX) {
        s_pw = rec;
        s_pw_set = true;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "record della password illeggibile: la password risulta non impostata");
    }
    mbedtls_platform_zeroize(&rec, sizeof(rec));

    nvs_close(h);
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

esp_err_t settings_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Partizione NVS piena o di formato diverso: va cancellata. Si perdono
        // anche gli abbinamenti Bluetooth, che stanno nella stessa partizione.
        ESP_LOGW(TAG, "NVS da reinizializzare (%s)", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init");

    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    s_write_lock = xSemaphoreCreateMutex();
    // Semafori del task PBKDF2: non vengono mai cancellati, così il task può
    // segnalare la fine senza rischiare di usare un oggetto già liberato.
    s_kdf_lock = xSemaphoreCreateMutex();
    s_kdf_done = xSemaphoreCreateBinary();
    if (lock == NULL || s_write_lock == NULL || s_kdf_lock == NULL || s_kdf_done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    load_from_nvs();
    s_lock = lock;

    ESP_LOGI(TAG, "hostname=%s dhcp=%d pc=%s:%u mg24=%lu baud, tcp %u, idle %u min, password %s",
             s_cfg.hostname, s_cfg.eth_dhcp, s_cfg.pc_host[0] ? s_cfg.pc_host : "(broadcast)",
             (unsigned)s_cfg.pc_port, (unsigned long)s_cfg.mg24_baud, (unsigned)s_cfg.mg24_tcp_port,
             (unsigned)s_cfg.bt_idle_timeout_min, s_pw_set ? "impostata" : "non impostata");
    return ESP_OK;
}

void settings_get(gw_settings_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_lock == NULL) {
        // Chiamata prima di settings_init(): solo i default.
        load_defaults(out);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

esp_err_t settings_set(const gw_settings_t *in)
{
    if (in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    gw_settings_t s = *in;
    const char *bad = validate(&s);
    if (bad != NULL) {
        ESP_LOGW(TAG, "impostazioni rifiutate: campo %s", bad);
        return ESP_ERR_INVALID_ARG;
    }
    tidy(&s);

    // Solo chi tiene s_write_lock modifica s_cfg: qui lo si può leggere senza
    // s_lock. La scrittura in flash avviene fuori da s_lock, così i lettori
    // vedono il valore vecchio fino al salvataggio riuscito, senza aspettare.
    // Niente scrittura se non è cambiato nulla: ogni scrittura NVS sospende la
    // cache di entrambi i core per qualche millisecondo.
    xSemaphoreTake(s_write_lock, portMAX_DELAY);
    esp_err_t err = save_cfg(&s_cfg, &s);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg = s;
        xSemaphoreGive(s_lock);
    }
    xSemaphoreGive(s_write_lock);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "salvataggio fallito: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t settings_factory_reset(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Cancella tutto il namespace, chiave del timeout compresa: al prossimo
    // caricamento ogni campo torna al default di menuconfig.
    xSemaphoreTake(s_write_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err == ESP_OK) {
        gw_settings_t def;
        load_defaults(&def);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_cfg = def;
        mbedtls_platform_zeroize(&s_pw, sizeof(s_pw));
        s_pw_set = false;
        xSemaphoreGive(s_lock);
    }
    xSemaphoreGive(s_write_lock);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory reset fallito: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "impostazioni e password cancellate");
    }
    return err;
}

bool settings_password_is_set(void)
{
    if (s_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool set = s_pw_set;
    xSemaphoreGive(s_lock);
    return set;
}

static int pw_derive_here(const char *plain, size_t len, const pw_record_t *rec, uint8_t out[PW_HASH_LEN])
{
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)plain, len, rec->salt,
                                         PW_SALT_LEN, rec->iterations, PW_HASH_LEN, out);
}

typedef struct {
    const char *plain;
    size_t len;
    const pw_record_t *rec;
    uint8_t *out;
    int ret;
} kdf_job_t;

// Task temporaneo: esegue la derivazione e segnala la fine. Dopo il "give" non
// tocca più il job, che sta sullo stack del chiamante.
static void kdf_task(void *arg)
{
    kdf_job_t *job = arg;
    job->ret = pw_derive_here(job->plain, job->len, job->rec, job->out);
    xSemaphoreGive(s_kdf_done);
    vTaskDelete(NULL);
}

// PBKDF2 sul core senza Bluetooth. Il chiamante resta bloccato fino alla fine.
// Se il task non si può creare, la derivazione si fa nel task del chiamante.
static int pw_derive(const char *plain, size_t len, const pw_record_t *rec, uint8_t out[PW_HASH_LEN])
{
    kdf_job_t job = {.plain = plain, .len = len, .rec = rec, .out = out, .ret = -1};

    xSemaphoreTake(s_kdf_lock, portMAX_DELAY);
    if (xTaskCreatePinnedToCore(kdf_task, "pw_kdf", KDF_TASK_STACK, &job, KDF_TASK_PRIO, NULL, KDF_CORE) == pdPASS) {
        xSemaphoreTake(s_kdf_done, portMAX_DELAY);
    } else {
        ESP_LOGW(TAG, "task PBKDF2 non creato: derivazione nel task chiamante");
        job.ret = pw_derive_here(plain, len, rec, out);
    }
    xSemaphoreGive(s_kdf_lock);
    return job.ret;
}

esp_err_t settings_password_set(const char *plain)
{
    if (plain == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t len = strnlen(plain, PW_MAX_LEN + 1);
    if (len < PW_MIN_LEN || len > PW_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    // La derivazione è lenta: si fa fuori dal mutex.
    pw_record_t rec = {
        .version = PW_RECORD_VERSION,
        .iterations = PW_ITERATIONS,
    };
    esp_fill_random(rec.salt, sizeof(rec.salt));
    if (pw_derive(plain, len, &rec, rec.hash) != 0) {
        mbedtls_platform_zeroize(&rec, sizeof(rec));
        return ESP_FAIL;
    }

    xSemaphoreTake(s_write_lock, portMAX_DELAY);
    esp_err_t err = save_blob(KEY_PW, &rec, sizeof(rec));
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_pw = rec;
        s_pw_set = true;
        xSemaphoreGive(s_lock);
    }
    xSemaphoreGive(s_write_lock);

    mbedtls_platform_zeroize(&rec, sizeof(rec));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "salvataggio della password fallito: %s", esp_err_to_name(err));
    }
    return err;
}

bool settings_password_check(const char *plain)
{
    if (plain == NULL || s_lock == NULL) {
        return false;
    }

    pw_record_t rec;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool set = s_pw_set;
    rec = s_pw;
    xSemaphoreGive(s_lock);
    if (!set) {
        return false;
    }

    // Una password troppo lunga è comunque sbagliata, ma la derivazione si fa
    // lo stesso (sui primi byte) per non rivelarlo con il tempo di risposta.
    size_t len = strnlen(plain, PW_MAX_LEN + 1);
    bool too_long = len > PW_MAX_LEN;
    if (too_long) {
        len = PW_MAX_LEN;
    }

    uint8_t hash[PW_HASH_LEN];
    bool ok = false;
    if (pw_derive(plain, len, &rec, hash) == 0) {
        ok = mbedtls_ct_memcmp(hash, rec.hash, PW_HASH_LEN) == 0 && !too_long;
    }
    mbedtls_platform_zeroize(hash, sizeof(hash));
    mbedtls_platform_zeroize(&rec, sizeof(rec));
    return ok;
}
