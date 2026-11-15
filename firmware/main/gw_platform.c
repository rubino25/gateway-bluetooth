// Bluetooth: "custom platform" di Bluepad32 per il controller Xbox (BLE) e, con
// CONFIG_GW_BT_BREDR, per il DualSense e gli altri controller classic (BR/EDR).
//
// Regole di questo file:
//  - Bluepad32 e BTstack non sono thread-safe: tutto ciò che li tocca gira nel
//    run loop di BTstack (il task main, da cui viene chiamata gw_bt_run()).
//  - Le funzioni pubbliche gw_bt_*() si possono chiamare da qualsiasi task:
//    registrano la richiesta e la fanno eseguire nel run loop con
//    btstack_run_loop_execute_on_main_thread().
//  - Lo stato letto dagli altri moduli (s_status) e le richieste in attesa
//    (s_req) sono protetti da s_lock. È uno spinlock e non un mutex FreeRTOS
//    perché deve funzionare anche prima di gw_bt_run() (la pagina web parte
//    prima) e le sezioni protette sono solo copie di pochi byte.
//  - Si gestisce un solo controller alla volta: pad_idx è sempre 0.
//
// API verificate sul sorgente di Bluepad32 5.0.0-beta0 (third_party/bluepad32)
// e del BTstack incluso come submodule.

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <btstack.h>
#include <btstack_port_esp32.h>
#include <btstack_run_loop.h>
#include <btstack_stdio_esp32.h>
#include <uni.h>
#include "parser/uni_hid_parser_ds5.h"

#include "gw_platform.h"
#include "net_link.h"
#include "settings.h"

#ifndef CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#error "Serve CONFIG_BLUEPAD32_PLATFORM_CUSTOM=y (vedi sdkconfig.defaults)"
#endif

// Requisiti di abbinamento BLE, da menuconfig ("Xbox Gateway - Bluetooth").
#if defined(CONFIG_GW_BT_LE_PAIRING_SC_BOND)
#define GW_LE_AUTHREQ (SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING)
#define GW_LE_AUTHREQ_NAME "LE Secure Connections + bonding"
#elif defined(CONFIG_GW_BT_LE_PAIRING_SC_NO_BOND)
#define GW_LE_AUTHREQ (SM_AUTHREQ_SECURE_CONNECTION)
#define GW_LE_AUTHREQ_NAME "LE Secure Connections, senza bonding"
#elif defined(CONFIG_GW_BT_LE_PAIRING_LEGACY_NO_BOND)
#define GW_LE_AUTHREQ (SM_AUTHREQ_NO_BONDING)
#define GW_LE_AUTHREQ_NAME "legacy, senza bonding"
#else
#define GW_LE_AUTHREQ (SM_AUTHREQ_BONDING)
#define GW_LE_AUTHREQ_NAME "legacy + bonding"
#endif

#ifndef CONFIG_GW_BT_RECONNECT_HOLDOFF_S
#define CONFIG_GW_BT_RECONNECT_HOLDOFF_S 30
#endif

#define GW_PAD_IDX 0

// Voci del database LE di BTstack più, con il BR/EDR attivo, le link key classic.
#ifdef CONFIG_GW_BT_BREDR
#define GW_MAX_BONDS (NVM_NUM_DEVICE_DB_ENTRIES + NVM_NUM_LINK_KEYS)
#define GW_BREDR_NAME "attivo"
#else
#define GW_MAX_BONDS (NVM_NUM_DEVICE_DB_ENTRIES)
#define GW_BREDR_NAME "spento"
#endif

// Blocco dopo lo scollegamento manuale per i controller classic. È più corto
// di CONFIG_GW_BT_RECONNECT_HOLDOFF_S perché un controller classic torna solo
// quando l'utente preme il suo tasto di accensione, non da solo come in BLE.
#define GW_BREDR_HOLDOFF_S 5

// Scollegamento per inattività (impostazione bt_idle_timeout_min).
#define GW_IDLE_CHECK_MS 1000
// Spostamento dall'ultima posizione di riferimento che conta come uso: circa il
// 10 % della corsa, così deriva e rumore degli stick non tengono acceso il
// controller.
#define GW_IDLE_AXIS_DELTA 100    // stick: -512..511
#define GW_IDLE_TRIGGER_DELTA 100 // grilletti: 0..1023
// Dopo lo scollegamento per inattività il controller viene ignorato finché
// continua a farsi vedere (advertising BLE, tentativi di connessione BR/EDR).
// Torna appena si rifà vivo dopo almeno GW_IDLE_QUIET_US di silenzio, cioè dopo
// essersi spento ed essere stato riacceso con il tasto Xbox o PS. Se non smette
// mai, al più tardi dopo GW_IDLE_BLOCK_MAX_US.
#define GW_IDLE_QUIET_US (10 * 1000000LL)
#define GW_IDLE_BLOCK_MAX_US (300 * 1000000LL)

// Report di ingresso Bluetooth del DualSense (0x31, 78 byte), come lo descrive
// ds5_input_report_t in parser/uni_hid_parser_ds5.c di Bluepad32. Il parser di
// Bluepad32 non espone touchpad, clic del touchpad e stato di carica: si leggono
// qui dal report grezzo. Offset dall'inizio del report (report[0] = 0x31).
#define DS5_BT_REPORT_ID 0x31
#define DS5_BT_REPORT_LEN 78
#define DS5_OFF_BUTTONS2 (2 + 9)  // buttons[2]: bit 1 = clic del touchpad
#define DS5_OFF_TOUCH (2 + 32)    // 2 punti da 4 byte: contatto, x_lo, x_hi|y_lo, y_hi
#define DS5_OFF_STATUS (2 + 52)   // bit 0..3 carica 0..10, bit 4..7 stato
#define DS5_TOUCHPAD_CLICK 0x02
#define DS5_TOUCH_INACTIVE 0x80   // bit 7 del byte di contatto: dito sollevato

// Unità del protocollo v2 per i sensori (le stesse del report USB del DualSense
// e di VIIPER): 16,384 LSB per °/s e 8192 LSB per g.
#define GW_GYRO_LSB_PER_RAD_S (16.384f * 57.29577951308232f)
#define GW_ACCEL_LSB_PER_MS2 (8192.0f / UNI_STANDARD_GRAVITY)

// Se cambiano solo sensori o touchpad, al massimo un pacchetto ogni 4 ms:
// tasti, stick e grilletti partono comunque subito.
#define GW_MOTION_MIN_INTERVAL_US 4000

#ifndef CONFIG_GW_BT_TASK_PRIORITY
#define CONFIG_GW_BT_TASK_PRIORITY 1
#endif

// Parametri delle connessioni BLE aperte dal gateway (CONFIG_GW_BT_LE_FAST_CONN).
// Unità di BTstack: scansione in 0,625 ms, intervallo in 1,25 ms, timeout in
// 10 ms. Scansione e timeout restano quelli di default di BTstack (hci_init).
#define GW_LE_SCAN_INTERVAL 0x0060     // 60 ms
#define GW_LE_SCAN_WINDOW 0x0030       // 30 ms
#define GW_LE_CONN_INTERVAL 6          // 7,5 ms, il minimo della specifica
#define GW_LE_CONN_LATENCY 0
#define GW_LE_SUPERVISION_TIMEOUT 72   // 720 ms
#define GW_LE_ACCEPT_INTERVAL_MAX 8    // 10 ms, con CONFIG_GW_BT_LE_REJECT_SLOW_CONN

#ifdef CONFIG_GW_BT_LATENCY_STATS
#define GW_STATS_PERIOD_MS 10000
#define GW_STATS_PAUSE_US 100000 // intervalli più lunghi = controller fermo, fuori dalla media
#endif

static const char *TAG = "gw_bt";

// ---------------------------------------------------------------------------
// Stato condiviso con gli altri task (protetto da s_lock)
// ---------------------------------------------------------------------------

enum {
    CMD_PAIRING = 1 << 0,
    CMD_DISCONNECT = 1 << 1,
    CMD_FORGET = 1 << 2,
    CMD_RUMBLE = 1 << 3,
    CMD_LIGHTBAR = 1 << 4,
    CMD_PLAYER_LEDS = 1 << 5,
};

typedef struct {
    uint16_t duration_ms;
    uint8_t weak;
    uint8_t strong;
} rumble_req_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

// Copia pubblicata dello stato: la legge gw_bt_get_status().
static gw_bt_status_t s_status = {.battery = -1};

// Richieste in attesa di essere eseguite nel run loop. Per ogni tipo vale
// l'ultima richiesta arrivata.
static struct {
    uint32_t bits;
    bool pairing;
    rumble_req_t rumble;
    uint8_t lightbar[3];
    uint8_t player_leds;
} s_req;

// true da on_init_complete() in poi: prima il run loop non esiste ancora e le
// richieste restano in s_req.
static atomic_bool s_bt_ready;

// ---------------------------------------------------------------------------
// Stato usato solo nel run loop di BTstack (nessun lock)
// ---------------------------------------------------------------------------

static gw_bt_status_t s_shadow = {.battery = -1}; // copia di lavoro di s_status
static uni_hid_device_t *s_active;                 // controller in uso, NULL se nessuno
static bool s_pairing;                             // si accettano controller non memorizzati
static bool s_scanning;                            // scansione richiesta a Bluepad32
static gw_input_t s_last_in;                       // ultimo stato inviato al modulo di rete
static bool s_have_last;
static bool s_report_without_battery; // il controller manda anche report senza batteria
static int64_t s_last_sent_us;         // ultimo INPUT inviato da plat_on_controller_data

// Dati del DualSense letti dal report grezzo (ds5_parse_input_report).
static struct {
    bool valid;
    bool click;
    uint8_t status;
    struct {
        uint16_t x, y;
        bool active;
    } touch[2];
} s_ds5;

// Barra luminosa e LED giocatore applicati al controller in uso: si inviano
// solo i cambi. Azzerati a ogni nuovo controller.
static bool s_lightbar_set;
static uint8_t s_lightbar[3];
static bool s_player_leds_set;
static uint8_t s_player_leds;

static bd_addr_t s_bonded[GW_MAX_BONDS]; // indirizzi memorizzati (cache)
static int s_bonded_n;

// Dopo uno scollegamento manuale si ignora quel controller per un po'.
static bd_addr_t s_block_addr;
static int64_t s_block_until_us;

// Scollegamento per inattività.
static btstack_timer_source_t s_idle_timer;
static int s_idle_timeout_min = -1; // minuti, ultimo valore letto dalle impostazioni
static int64_t s_last_use_us;       // ultimo uso del controller in uso
static gw_input_t s_idle_ref;       // riferimento per tasti, stick e grilletti
static bool s_idle_ref_valid;
static struct {
    bool on;
    bool seen;         // si è fatto vedere dopo lo scollegamento
    bool quiet_logged;
    bd_addr_t addr;
    int64_t since_us;  // momento dello scollegamento
    int64_t seen_us;   // ultimo advertising o tentativo di connessione
} s_idle_block;

#ifdef CONFIG_GW_BT_BREDR
static bool s_inquiry;            // ricerca (inquiry) BR/EDR richiesta a Bluepad32
static bool s_connectable = true; // page scan: lo accende uni_bt_bredr_setup()
// Connessione entrante appena rifiutata: Bluepad32 crea comunque una voce per
// quell'indirizzo, la toglie bt_hci_event_handler().
static bd_addr_t s_reject_addr;
static bool s_reject_pending;
static btstack_packet_callback_registration_t s_hci_reg;
#endif

// Parametri della connessione BLE in uso, dagli eventi HCI (bt_le_event_handler).
static hci_con_handle_t s_le_handle = HCI_CON_HANDLE_INVALID;
static uint16_t s_le_interval; // unità di 1,25 ms, 0 = nessuna connessione BLE
static uint16_t s_le_latency;
static btstack_packet_callback_registration_t s_le_hci_reg;
static btstack_packet_callback_registration_t s_le_l2cap_reg;

#ifdef CONFIG_GW_BT_LATENCY_STATS
// Temporizzazione dei report del controller in uso, azzerata a ogni periodo.
static struct {
    int64_t last_report_us;  // 0 = nessun report dal collegamento
    uint32_t reports;        // report ricevuti
    uint32_t gaps;           // intervalli sotto GW_STATS_PAUSE_US
    uint32_t pauses;         // intervalli sopra GW_STATS_PAUSE_US
    int64_t gap_sum_us;
    int64_t gap_min_us;
    int64_t gap_max_us;
    uint32_t bins[5];        // < 5, < 9, < 17, < 33, < 100 ms
    uint32_t sent;           // report inoltrati al PC
    int64_t proc_sum_us;     // dal report al ritorno dell'invio UDP
    int64_t proc_max_us;
} s_stats;
static btstack_timer_source_t s_stats_timer;
static void bt_stats_reset(bool keep_last);
#endif

static void bt_cmd_handler(void *context);
static btstack_context_callback_registration_t s_cmd_reg = {
    .callback = &bt_cmd_handler,
    .context = NULL,
};

// ---------------------------------------------------------------------------
// Funzioni interne: vanno chiamate solo dal run loop di BTstack
// ---------------------------------------------------------------------------

static void bt_publish_status(void)
{
    portENTER_CRITICAL(&s_lock);
    s_status = s_shadow;
    portEXIT_CRITICAL(&s_lock);
}

// Rilegge da BTstack l'elenco dei controller memorizzati. Leggere il database
// costa accessi a NVS, per questo si tiene una copia in RAM.
static void bt_refresh_bonds(void)
{
    s_bonded_n = 0;

    for (int i = 0; i < le_device_db_max_count() && s_bonded_n < GW_MAX_BONDS; i++) {
        int addr_type = (int)BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t addr;
        le_device_db_info(i, &addr_type, addr, NULL);
        if (addr_type == (int)BD_ADDR_TYPE_UNKNOWN) {
            continue; // voce libera
        }
        bd_addr_copy(s_bonded[s_bonded_n++], addr);
    }

#ifdef CONFIG_GW_BT_BREDR
    btstack_link_key_iterator_t it;
    if (gap_link_key_iterator_init(&it)) {
        bd_addr_t addr;
        link_key_t key;
        link_key_type_t type;
        while (s_bonded_n < GW_MAX_BONDS && gap_link_key_iterator_get_next(&it, addr, key, &type)) {
            bd_addr_copy(s_bonded[s_bonded_n++], addr);
        }
        gap_link_key_iterator_done(&it);
    }
#endif

    s_shadow.bonded_count = (uint8_t)s_bonded_n;
}

// Nota: il confronto è sull'indirizzo visto in aria. Vale per i controller con
// indirizzo pubblico o statico (come l'Xbox); con indirizzi privati risolvibili
// l'indirizzo cambia e il controller non verrebbe riconosciuto.
static bool bt_is_bonded(const bd_addr_t addr)
{
    for (int i = 0; i < s_bonded_n; i++) {
        if (bd_addr_cmp(s_bonded[i], addr) == 0) {
            return true;
        }
    }
    return false;
}

// Blocco dopo lo scollegamento per inattività (vedi GW_IDLE_QUIET_US): ogni
// advertising o tentativo di connessione del controller lo prolunga.
static bool bt_idle_blocked(const bd_addr_t addr)
{
    if (!s_idle_block.on || bd_addr_cmp(s_idle_block.addr, addr) != 0) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    if (now - s_idle_block.seen_us >= GW_IDLE_QUIET_US) {
        ESP_LOGI(TAG, "Controller %s riacceso dopo lo scollegamento per inattività", bd_addr_to_str(addr));
        s_idle_block.on = false;
        return false;
    }
    if (now - s_idle_block.since_us >= GW_IDLE_BLOCK_MAX_US) {
        ESP_LOGW(TAG, "Il controller %s cerca il gateway da %d s senza spegnersi: lo lascio ricollegare",
                 bd_addr_to_str(addr), (int)(GW_IDLE_BLOCK_MAX_US / 1000000));
        s_idle_block.on = false;
        return false;
    }
    if (!s_idle_block.seen) {
        s_idle_block.seen = true;
        ESP_LOGI(TAG, "Il controller %s cerca ancora il gateway: ignorato finché non si spegne", bd_addr_to_str(addr));
    }
    s_idle_block.seen_us = now;
    return true;
}

// Dopo uno scollegamento manuale quel controller viene ignorato per un po'.
static bool bt_is_blocked(const bd_addr_t addr)
{
    if (bt_idle_blocked(addr)) {
        return true;
    }
    if (s_block_until_us == 0) {
        return false;
    }
    if (esp_timer_get_time() >= s_block_until_us) {
        s_block_until_us = 0;
        return false;
    }
    return bd_addr_cmp(s_block_addr, addr) == 0;
}

// La scansione serve solo quando non c'è un controller collegato: sia per
// abbinarne uno nuovo sia per far ricollegare quello memorizzato (in BLE è
// sempre il gateway ad aprire la connessione, dopo aver visto l'advertising).
// Con un controller collegato si spegne, così la radio resta alla connessione.
//
// Non si usano uni_bt_start_scanning_and_autoconnect_unsafe() e simili perché
// accendono insieme scansione BLE e inquiry BR/EDR. Qui le tre attività radio
// sono separate:
//  - scansione BLE: sempre, finché non c'è un controller;
//  - page scan BR/EDR (essere raggiungibili): come sopra. Un controller classic
//    già abbinato (DualSense) si ricollega chiamando lui il gateway;
//  - inquiry BR/EDR (cercare controller nuovi): solo con l'abbinamento attivo,
//    perché è l'attività che toglie più tempo radio a BLE e Zigbee.
static void bt_scan_update(void)
{
    bool want = (s_active == NULL);
    if (want != s_scanning) {
        s_scanning = want;
        if (want) {
            uni_bt_le_scan_start();
        } else {
            uni_bt_le_scan_stop();
        }
    }

#ifdef CONFIG_GW_BT_BREDR
    if (want != s_connectable) {
        s_connectable = want;
        gap_connectable_control(want ? 1 : 0);
    }

    bool inquiry = want && s_pairing;
    if (inquiry != s_inquiry) {
        s_inquiry = inquiry;
        if (inquiry) {
            uni_bt_bredr_scan_start();
        } else {
            uni_bt_bredr_scan_stop();
        }
    }
#endif
}

static bool bt_is_classic(const uni_hid_device_t *d)
{
    return gap_get_connection_type(d->conn.handle) == GAP_CONNECTION_ACL;
}

static uint8_t bt_kind(const uni_hid_device_t *d)
{
    switch (d->controller_type) {
    case CONTROLLER_TYPE_XBoxOneController:
        return GW_BT_KIND_XBOX;
    case CONTROLLER_TYPE_PS5Controller:
        return GW_BT_KIND_DUALSENSE;
    case CONTROLLER_TYPE_PS4Controller:
        return GW_BT_KIND_DUALSHOCK4;
    default:
        return GW_BT_KIND_OTHER;
    }
}

static void bt_set_pairing(bool enable)
{
    if (enable) {
        // Chi avvia l'abbinamento vuole collegare un controller adesso.
        s_block_until_us = 0;
        s_idle_block.on = false;
    }
    if (enable != s_pairing) {
        ESP_LOGI(TAG, "Abbinamento di nuovi controller: %s", enable ? "attivo" : "spento");
    }
    s_pairing = enable;
    s_shadow.pairing_enabled = enable;
    bt_publish_status();

    // L'inquiry BR/EDR segue l'abbinamento.
    bt_scan_update();
}

// Il controller in uso non c'è più: aggiorna lo stato e avvisa il PC.
static void bt_controller_gone(void)
{
    s_active = NULL;
    s_have_last = false;
    s_report_without_battery = false;
    memset(&s_ds5, 0, sizeof(s_ds5));
    s_lightbar_set = false;
    s_player_leds_set = false;
    s_idle_ref_valid = false;
#ifdef CONFIG_GW_BT_LATENCY_STATS
    bt_stats_reset(false);
#endif

    s_shadow.connected = false;
    s_shadow.name[0] = '\0';
    memset(s_shadow.addr, 0, sizeof(s_shadow.addr));
    s_shadow.battery = -1;
    s_shadow.kind = GW_BT_KIND_NONE;
    s_shadow.classic = false;
    bt_publish_status();

    gw_input_t in;
    memset(&in, 0, sizeof(in));
    in.pad_idx = GW_PAD_IDX;
    in.connected = false;
    net_link_send_input(&in);

    bt_scan_update();
}

static void bt_disconnect_active(bool holdoff)
{
    uni_hid_device_t *d = s_active;
    if (d == NULL) {
        return;
    }

    int holdoff_s = CONFIG_GW_BT_RECONNECT_HOLDOFF_S;
    if (bt_is_classic(d) && holdoff_s > GW_BREDR_HOLDOFF_S) {
        holdoff_s = GW_BREDR_HOLDOFF_S;
    }
    if (holdoff && holdoff_s > 0) {
        bd_addr_copy(s_block_addr, d->conn.btaddr);
        s_block_until_us = esp_timer_get_time() + (int64_t)holdoff_s * 1000000;
    }

    ESP_LOGI(TAG, "Scollego il controller %s", bd_addr_to_str(d->conn.btaddr));

    // Stessa sequenza usata da Bluepad32 per uni_bt_disconnect_device_safe().
    // uni_hid_device_disconnect() chiama on_device_disconnected(), che porta a
    // bt_controller_gone().
    uni_hid_device_disconnect(d);
    uni_hid_device_delete(d);

    if (s_active == d) {
        bt_controller_gone();
    }
}

// Scollegamento per inattività: stesso percorso di gw_bt_disconnect(), ma il
// blocco dura finché il controller cerca il gateway (bt_idle_blocked).
// Né Bluepad32 né i report HID dei due controller hanno un comando di
// spegnimento. Ci si aspetta (da verificare) che il DualSense si spenga quando
// l'host lo scollega e che l'Xbox si spenga dopo aver cercato l'host per un po'.
static void bt_disconnect_idle(void)
{
    uni_hid_device_t *d = s_active;
    if (d == NULL) {
        return;
    }
    int64_t now = esp_timer_get_time();
    bd_addr_copy(s_idle_block.addr, d->conn.btaddr);
    s_idle_block.since_us = now;
    s_idle_block.seen_us = now;
    s_idle_block.seen = false;
    s_idle_block.quiet_logged = false;
    s_idle_block.on = true;
    bt_disconnect_active(false);
}

static void bt_forget_all(void)
{
    bt_disconnect_active(false);
    uni_bt_del_keys_unsafe();
    bt_refresh_bonds();
    bt_publish_status();
    ESP_LOGI(TAG, "Controller memorizzati cancellati");
}

static void bt_rumble(const rumble_req_t *r)
{
    uni_hid_device_t *d = s_active;
    if (d == NULL || d->report_parser.play_dual_rumble == NULL) {
        return;
    }

    // Per Bluepad32 durata 0 significa "ferma la vibrazione".
    uint16_t duration_ms = r->duration_ms;
    if (r->weak == 0 && r->strong == 0) {
        duration_ms = 0;
    }
    d->report_parser.play_dual_rumble(d, 0 /* avvio immediato */, duration_ms, r->weak, r->strong);
}

static void bt_apply_lightbar(const uint8_t rgb[3])
{
    uni_hid_device_t *d = s_active;
    if (d == NULL || d->report_parser.set_lightbar_color == NULL) {
        return;
    }
    if (s_lightbar_set && memcmp(s_lightbar, rgb, sizeof(s_lightbar)) == 0) {
        return;
    }
    memcpy(s_lightbar, rgb, sizeof(s_lightbar));
    s_lightbar_set = true;
    d->report_parser.set_lightbar_color(d, rgb[0], rgb[1], rgb[2]);
}

// Bluepad32 accetta per i LED giocatore una maschera di 4 "posti"
// (GAMEPAD_SEAT_*) e la traduce negli schemi di Sony (uni_hid_parser_ds5_set_player_leds).
// Il PC manda invece i 5 LED così come li chiede il gioco: si riportano gli
// schemi standard ai posti e il resto, per quanto possibile, ai LED esterni
// (il LED centrale da solo esiste solo come giocatore 1).
static uint8_t ds5_leds_to_seats(uint8_t mask)
{
    switch (mask & 0x1f) {
    case 0x00: return GAMEPAD_SEAT_NONE;
    case 0x04: return GAMEPAD_SEAT_A;  // --X--
    case 0x0a: return GAMEPAD_SEAT_B;  // -X-X-
    case 0x15: return GAMEPAD_SEAT_C;  // X-X-X
    case 0x1b: return GAMEPAD_SEAT_D;  // XX-XX
    default: return (uint8_t)((mask & 0x03) | ((mask >> 1) & 0x0c));
    }
}

static void bt_apply_player_leds(uint8_t mask)
{
    uni_hid_device_t *d = s_active;
    if (d == NULL || d->report_parser.set_player_leds == NULL) {
        return;
    }
    if (s_player_leds_set && s_player_leds == mask) {
        return;
    }
    s_player_leds = mask;
    s_player_leds_set = true;
    d->report_parser.set_player_leds(d, ds5_leds_to_seats(mask));
}

// Esegue nel run loop le richieste arrivate dagli altri task.
static void bt_cmd_handler(void *context)
{
    (void)context;

    uint32_t bits;
    bool pairing;
    rumble_req_t rumble;
    uint8_t lightbar[3];
    uint8_t player_leds;

    portENTER_CRITICAL(&s_lock);
    bits = s_req.bits;
    s_req.bits = 0;
    pairing = s_req.pairing;
    rumble = s_req.rumble;
    memcpy(lightbar, s_req.lightbar, sizeof(lightbar));
    player_leds = s_req.player_leds;
    portEXIT_CRITICAL(&s_lock);

    if (bits & CMD_FORGET) {
        bt_forget_all();
    } else if (bits & CMD_DISCONNECT) {
        bt_disconnect_active(true);
    }
    if (bits & CMD_PAIRING) {
        bt_set_pairing(pairing);
    }
    if (bits & CMD_RUMBLE) {
        bt_rumble(&rumble);
    }
    if (bits & CMD_LIGHTBAR) {
        bt_apply_lightbar(lightbar);
    }
    if (bits & CMD_PLAYER_LEDS) {
        bt_apply_player_leds(player_leds);
    }
}

static int16_t clamp_axis(int32_t v)
{
    if (v < -512) {
        return -512;
    }
    if (v > 511) {
        return 511;
    }
    return (int16_t)v;
}

static uint16_t clamp_pedal(int32_t v)
{
    if (v < 0) {
        return 0;
    }
    if (v > 1023) {
        return 1023;
    }
    return (uint16_t)v;
}

// Arrotonda e limita all'intervallo di int16_t.
static int16_t float_to_i16(float v)
{
    v += (v >= 0.0f) ? 0.5f : -0.5f;
    if (v <= -32768.0f) {
        return INT16_MIN;
    }
    if (v >= 32767.0f) {
        return INT16_MAX;
    }
    return (int16_t)v;
}

// Sostituisce uni_hid_parser_ds5_parse_input_report per il controller in uso:
// chiama il parser di Bluepad32 e poi legge dal report grezzo quello che
// Bluepad32 non espone. Non tocca lo stato del parser.
static void ds5_parse_input_report(uni_hid_device_t *d, const uint8_t *report, uint16_t len)
{
    uni_hid_parser_ds5_parse_input_report(d, report, len);

    if (d != s_active || report == NULL || len != DS5_BT_REPORT_LEN || report[0] != DS5_BT_REPORT_ID) {
        return;
    }

    s_ds5.click = (report[DS5_OFF_BUTTONS2] & DS5_TOUCHPAD_CLICK) != 0;
    for (int i = 0; i < 2; i++) {
        const uint8_t *p = &report[DS5_OFF_TOUCH + 4 * i];
        s_ds5.touch[i].active = (p[0] & DS5_TOUCH_INACTIVE) == 0;
        s_ds5.touch[i].x = (uint16_t)(p[1] | ((p[2] & 0x0f) << 8));
        s_ds5.touch[i].y = (uint16_t)((p[2] >> 4) | (p[3] << 4));
    }
    s_ds5.status = report[DS5_OFF_STATUS];
    s_ds5.valid = true;
}

static uint8_t ds5_power(uint8_t status);

// Campi v2 comuni a tutti i controller: tipo, batteria, carica, touchpad.
static void bt_fill_extras(gw_input_t *in)
{
    in->kind = s_shadow.kind;
    in->battery = s_shadow.battery < 0 ? 255 : (uint8_t)s_shadow.battery;
    in->power = s_ds5.valid ? ds5_power(s_ds5.status) : GW_BT_POWER_UNKNOWN;
    if (s_ds5.valid) {
        in->has_touch = true;
        in->touchpad_click = s_ds5.click;
        for (int i = 0; i < 2; i++) {
            in->touch[i].x = s_ds5.touch[i].x;
            in->touch[i].y = s_ds5.touch[i].y;
            in->touch[i].active = s_ds5.touch[i].active;
        }
    }
}

// Uguali a parte sensori e posizione delle dita sul touchpad.
static bool input_controls_equal(const gw_input_t *a, const gw_input_t *b)
{
    gw_input_t x = *a;
    gw_input_t y = *b;
    memset(x.gyro, 0, sizeof(x.gyro));
    memset(x.accel, 0, sizeof(x.accel));
    memset(x.touch, 0, sizeof(x.touch));
    memset(y.gyro, 0, sizeof(y.gyro));
    memset(y.accel, 0, sizeof(y.accel));
    memset(y.touch, 0, sizeof(y.touch));
    return memcmp(&x, &y, sizeof(x)) == 0;
}

// Stato di carica del DualSense (come hid-playstation di Linux): bit 4..7 =
// 0 in scarica, 1 in carica, 2 carica completa; altri valori = errore.
static uint8_t ds5_power(uint8_t status)
{
    switch (status >> 4) {
    case 0x0: return GW_BT_POWER_DISCHARGING;
    case 0x1: return GW_BT_POWER_CHARGING;
    case 0x2: return GW_BT_POWER_FULL;
    default: return GW_BT_POWER_UNKNOWN;
    }
}

// Livello 0..10 → 0..100 %. Bluepad32 lo converte in 1..251 su 255 e il 100 %
// non comparirebbe mai.
static int16_t ds5_battery_percent(uint8_t status)
{
    if ((status >> 4) == 0x2) {
        return 100;
    }
    int level = status & 0x0f;
    return (int16_t)(level > 10 ? 100 : level * 10);
}

// Bluepad32: 0 = sconosciuta, 1 = scarica, 255 = carica.
static int16_t battery_percent(uint8_t raw)
{
    if (raw == UNI_CONTROLLER_BATTERY_NOT_AVAILABLE) {
        return -1;
    }
    return (int16_t)(((int)raw * 100) / UNI_CONTROLLER_BATTERY_FULL);
}

// Tutti i comandi a riposo, bit per bit (come li lascia init_report del parser).
static bool gamepad_is_blank(const uni_gamepad_t *gp)
{
    return gp->buttons == 0 && gp->dpad == 0 && gp->misc_buttons == 0 && gp->axis_x == 0 && gp->axis_y == 0 &&
           gp->axis_rx == 0 && gp->axis_ry == 0 && gp->brake == 0 && gp->throttle == 0;
}

#ifdef CONFIG_GW_BT_BREDR
// Connessioni BR/EDR entranti: un controller classic già abbinato, quando si
// accende, chiama lui il gateway e non passa da on_device_discovered().
// Bluepad32 le accetterebbe tutte; qui valgono le stesse regole dei controller
// trovati con la scansione. Il rifiuto avviene prima dell'autenticazione, così
// un controller dimenticato non può riabbinarsi con l'abbinamento spento.
// Gli indirizzi BR/EDR sono pubblici: il confronto con quelli memorizzati vale.
static int bt_classic_accept(bd_addr_t addr, hci_link_type_t link_type)
{
    const char *why = NULL;
    if (link_type != HCI_LINK_TYPE_ACL) {
        why = "collegamento non ACL";
    } else if (s_active != NULL) {
        why = "c'è già un controller collegato";
    } else if (bt_is_blocked(addr)) {
        why = "appena scollegato";
    } else if (!s_pairing && !bt_is_bonded(addr)) {
        why = "non memorizzato e abbinamento spento";
    }
    if (why == NULL) {
        return 1;
    }

    ESP_LOGW(TAG, "Connessione entrante da %s rifiutata: %s", bd_addr_to_str(addr), why);
    if (uni_hid_device_get_instance_for_address(addr) == NULL) {
        bd_addr_copy(s_reject_addr, addr);
        s_reject_pending = true;
    }
    return 0;
}

// Registrato dopo quello di Bluepad32, quindi per ogni evento viene chiamato
// dopo il suo.
static void bt_hci_event_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
    case HCI_EVENT_CONNECTION_REQUEST:
        // Bluepad32 ha appena creato una voce anche per la connessione
        // rifiutata; senza questo occuperebbe per 20 s uno dei
        // CONFIG_BLUEPAD32_MAX_DEVICES posti.
        if (s_reject_pending) {
            s_reject_pending = false;
            uni_hid_device_t *d = uni_hid_device_get_instance_for_address(s_reject_addr);
            if (d != NULL && d != s_active) {
                uni_hid_device_delete(d);
            }
        }
        break;
    case GAP_EVENT_INQUIRY_COMPLETE:
        // BTstack ha finito di fermare l'inquiry. Se intanto l'abbinamento è
        // stato riacceso, l'avvio richiesto durante lo spegnimento è fallito:
        // si riprova adesso.
        if (s_inquiry) {
            s_inquiry = false;
            bt_scan_update();
        }
        break;
    default:
        break;
    }
}
#endif

// ---------------------------------------------------------------------------
// Parametri della connessione BLE e statistiche di temporizzazione
// ---------------------------------------------------------------------------

// Intervallo BLE (unità di 1,25 ms) in centesimi di ms: 6 -> 750, cioè 7,50 ms.
static unsigned le_interval_cms(uint16_t interval)
{
    return (unsigned)interval * 125u;
}

static void bt_le_conn_params(const char *what, hci_con_handle_t handle, uint16_t interval, uint16_t latency,
                              uint16_t timeout)
{
    s_le_handle = handle;
    s_le_interval = interval;
    s_le_latency = latency;
    unsigned cms = le_interval_cms(interval);
    ESP_LOGI(TAG, "Connessione BLE %s: intervallo %u,%02u ms, latency %u, timeout %u ms", what, cms / 100, cms % 100,
             (unsigned)latency, (unsigned)timeout * 10u);
}

// Richiesta di nuovi parametri da parte del controller. BTstack la accetta se sta
// nel range di gap_set_connection_parameter_range(), altrimenti la rifiuta.
static void bt_le_param_request(const char *via, uint16_t min, uint16_t max, uint16_t latency, uint16_t timeout)
{
    le_connection_parameter_range_t range;
    gap_get_connection_parameter_range(&range);
    bool accepted = gap_connection_parameter_range_included(&range, min, max, latency, timeout) != 0;
    unsigned cmin = le_interval_cms(min);
    unsigned cmax = le_interval_cms(max);
    ESP_LOGI(TAG, "Il controller chiede (%s) intervallo %u,%02u-%u,%02u ms, latency %u, timeout %u ms: %s", via,
             cmin / 100, cmin % 100, cmax / 100, cmax % 100, (unsigned)latency, (unsigned)timeout * 10u,
             accepted ? "accettata" : "rifiutata");
}

// Registrato sia per gli eventi HCI sia per quelli L2CAP: i codici non si
// sovrappongono.
static void bt_le_event_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
    case HCI_EVENT_LE_META:
        switch (hci_event_le_meta_get_subevent_code(packet)) {
        case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
            if (hci_subevent_le_connection_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
                bt_le_conn_params("aperta", hci_subevent_le_connection_complete_get_connection_handle(packet),
                                  hci_subevent_le_connection_complete_get_conn_interval(packet),
                                  hci_subevent_le_connection_complete_get_conn_latency(packet),
                                  hci_subevent_le_connection_complete_get_supervision_timeout(packet));
            }
            break;
        case HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V1:
            if (hci_subevent_le_enhanced_connection_complete_v1_get_status(packet) == ERROR_CODE_SUCCESS) {
                bt_le_conn_params("aperta", hci_subevent_le_enhanced_connection_complete_v1_get_connection_handle(packet),
                                  hci_subevent_le_enhanced_connection_complete_v1_get_conn_interval(packet),
                                  hci_subevent_le_enhanced_connection_complete_v1_get_conn_latency(packet),
                                  hci_subevent_le_enhanced_connection_complete_v1_get_supervision_timeout(packet));
            }
            break;
        case HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE_V2:
            if (hci_subevent_le_enhanced_connection_complete_v2_get_status(packet) == ERROR_CODE_SUCCESS) {
                bt_le_conn_params("aperta", hci_subevent_le_enhanced_connection_complete_v2_get_connection_handle(packet),
                                  hci_subevent_le_enhanced_connection_complete_v2_get_conn_interval(packet),
                                  hci_subevent_le_enhanced_connection_complete_v2_get_conn_latency(packet),
                                  hci_subevent_le_enhanced_connection_complete_v2_get_supervision_timeout(packet));
            }
            break;
        case HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE:
            if (hci_subevent_le_connection_update_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
                bt_le_conn_params("aggiornata", hci_subevent_le_connection_update_complete_get_connection_handle(packet),
                                  hci_subevent_le_connection_update_complete_get_conn_interval(packet),
                                  hci_subevent_le_connection_update_complete_get_conn_latency(packet),
                                  hci_subevent_le_connection_update_complete_get_supervision_timeout(packet));
            }
            break;
        case HCI_SUBEVENT_LE_REMOTE_CONNECTION_PARAMETER_REQUEST:
            bt_le_param_request("LL", hci_subevent_le_remote_connection_parameter_request_get_interval_min(packet),
                                hci_subevent_le_remote_connection_parameter_request_get_interval_max(packet),
                                hci_subevent_le_remote_connection_parameter_request_get_latency(packet),
                                hci_subevent_le_remote_connection_parameter_request_get_timeout(packet));
            break;
        default:
            break;
        }
        break;
    case L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_REQUEST:
        bt_le_param_request("L2CAP", l2cap_event_connection_parameter_update_request_get_interval_min(packet),
                            l2cap_event_connection_parameter_update_request_get_interval_max(packet),
                            l2cap_event_connection_parameter_update_request_get_latency(packet),
                            l2cap_event_connection_parameter_update_request_get_timeout_multiplier(packet));
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        if (hci_event_disconnection_complete_get_connection_handle(packet) == s_le_handle) {
            s_le_handle = HCI_CON_HANDLE_INVALID;
            s_le_interval = 0;
            s_le_latency = 0;
        }
        break;
    default:
        break;
    }
}

#ifdef CONFIG_GW_BT_LATENCY_STATS
// keep_last: si continua a misurare l'intervallo dall'ultimo report ricevuto.
static void bt_stats_reset(bool keep_last)
{
    int64_t last = keep_last ? s_stats.last_report_us : 0;
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.last_report_us = last;
    s_stats.gap_min_us = INT64_MAX;
}

static void bt_stats_report(int64_t now)
{
    s_stats.reports++;
    if (s_stats.last_report_us != 0) {
        int64_t gap = now - s_stats.last_report_us;
        if (gap >= GW_STATS_PAUSE_US) {
            s_stats.pauses++;
        } else {
            s_stats.gaps++;
            s_stats.gap_sum_us += gap;
            if (gap < s_stats.gap_min_us) {
                s_stats.gap_min_us = gap;
            }
            if (gap > s_stats.gap_max_us) {
                s_stats.gap_max_us = gap;
            }
            int bin = gap < 5000 ? 0 : gap < 9000 ? 1 : gap < 17000 ? 2 : gap < 33000 ? 3 : 4;
            s_stats.bins[bin]++;
        }
    }
    s_stats.last_report_us = now;
}

static void bt_stats_sent(int64_t report_us)
{
    int64_t t = esp_timer_get_time() - report_us;
    s_stats.sent++;
    s_stats.proc_sum_us += t;
    if (t > s_stats.proc_max_us) {
        s_stats.proc_max_us = t;
    }
}

static void bt_stats_timer_handler(btstack_timer_source_t *ts)
{
    if (s_active != NULL && s_stats.reports > 0) {
        char conn[32];
        if (s_shadow.classic) {
            strlcpy(conn, "BR/EDR", sizeof(conn));
        } else if (s_le_interval != 0) {
            unsigned cms = le_interval_cms(s_le_interval);
            snprintf(conn, sizeof(conn), "BLE %u,%02u ms, latency %u", cms / 100, cms % 100, (unsigned)s_le_latency);
        } else {
            strlcpy(conn, "BLE", sizeof(conn));
        }
        int64_t gap_min = s_stats.gaps > 0 ? s_stats.gap_min_us : 0;
        int64_t gap_avg = s_stats.gaps > 0 ? s_stats.gap_sum_us / s_stats.gaps : 0;
        int64_t proc_avg = s_stats.sent > 0 ? s_stats.proc_sum_us / s_stats.sent : 0;
        ESP_LOGI(TAG,
                 "Tempi %u s (%s): %" PRIu32 " report, intervallo min/medio/max %" PRId64 "/%" PRId64 "/%" PRId64
                 " us, fasce <5/<9/<17/<33/<100 ms %" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32
                 ", pause %" PRIu32 "; %" PRIu32 " inviati al PC, report->UDP medio/max %" PRId64 "/%" PRId64 " us",
                 GW_STATS_PERIOD_MS / 1000, conn, s_stats.reports, gap_min, gap_avg, s_stats.gap_max_us,
                 s_stats.bins[0], s_stats.bins[1], s_stats.bins[2], s_stats.bins[3], s_stats.bins[4], s_stats.pauses,
                 s_stats.sent, proc_avg, s_stats.proc_max_us);
    }
    bt_stats_reset(true);
    btstack_run_loop_set_timer(ts, GW_STATS_PERIOD_MS);
    btstack_run_loop_add_timer(ts);
}

static void bt_stats_start(void)
{
    bt_stats_reset(false);
    btstack_run_loop_set_timer_handler(&s_stats_timer, &bt_stats_timer_handler);
    btstack_run_loop_set_timer(&s_stats_timer, GW_STATS_PERIOD_MS);
    btstack_run_loop_add_timer(&s_stats_timer);
}
#endif

// ---------------------------------------------------------------------------
// Scollegamento per inattività
// ---------------------------------------------------------------------------

static bool idle_moved(int a, int b, int delta)
{
    return abs(a - b) > delta;
}

// Conta come uso solo ciò che fa la persona: tasti, croce e tasti di sistema che
// cambiano, stick e grilletti spostati di oltre ~10 % dall'ultima posizione di
// riferimento, dita sul touchpad o clic (DualSense). Sensori di movimento,
// batteria e vibrazione no.
static void bt_idle_note_input(const gw_input_t *in)
{
    if (!s_idle_ref_valid) {
        // Primo report dopo il collegamento: diventa il riferimento.
        s_idle_ref = *in;
        s_idle_ref_valid = true;
        return;
    }
    const gw_input_t *r = &s_idle_ref;
    bool used = in->buttons != r->buttons || in->dpad != r->dpad || in->misc != r->misc ||
                idle_moved(in->lx, r->lx, GW_IDLE_AXIS_DELTA) || idle_moved(in->ly, r->ly, GW_IDLE_AXIS_DELTA) ||
                idle_moved(in->rx, r->rx, GW_IDLE_AXIS_DELTA) || idle_moved(in->ry, r->ry, GW_IDLE_AXIS_DELTA) ||
                idle_moved(in->lt, r->lt, GW_IDLE_TRIGGER_DELTA) || idle_moved(in->rt, r->rt, GW_IDLE_TRIGGER_DELTA) ||
                in->touchpad_click != r->touchpad_click || in->touch[0].active || in->touch[1].active;
    if (used) {
        s_idle_ref = *in;
        s_last_use_us = esp_timer_get_time();
    }
}

static void bt_idle_timer_handler(btstack_timer_source_t *ts)
{
    // L'impostazione si rilegge a ogni controllo: ha effetto subito.
    gw_settings_t cfg;
    settings_get(&cfg);
    int64_t now = esp_timer_get_time();

    if ((int)cfg.bt_idle_timeout_min != s_idle_timeout_min) {
        s_idle_timeout_min = cfg.bt_idle_timeout_min;
        s_last_use_us = now; // il conteggio riparte a ogni cambio
        if (s_idle_timeout_min == 0) {
            ESP_LOGI(TAG, "Scollegamento per inattività: mai");
        } else {
            ESP_LOGI(TAG, "Scollegamento per inattività: dopo %d min senza uso", s_idle_timeout_min);
        }
    }

    if (s_active != NULL && s_idle_timeout_min > 0 &&
        now - s_last_use_us >= (int64_t)s_idle_timeout_min * 60 * 1000000) {
        ESP_LOGI(TAG, "Controller scollegato dopo %d min senza uso", s_idle_timeout_min);
        bt_disconnect_idle();
    }

    // Solo per il log: se e per quanto il controller ha cercato il gateway dopo lo
    // scollegamento (da verificare sul campo).
    if (s_idle_block.on && !s_idle_block.quiet_logged && s_active == NULL &&
        now - s_idle_block.seen_us >= GW_IDLE_QUIET_US) {
        s_idle_block.quiet_logged = true;
        if (s_idle_block.seen) {
            ESP_LOGI(TAG, "Il controller ha smesso di cercare il gateway dopo %d s: torna alla prossima accensione",
                     (int)((s_idle_block.seen_us - s_idle_block.since_us) / 1000000));
        } else {
            ESP_LOGI(TAG, "Il controller non ha cercato il gateway dopo lo scollegamento: torna alla prossima accensione");
        }
    }

    btstack_run_loop_set_timer(ts, GW_IDLE_CHECK_MS);
    btstack_run_loop_add_timer(ts);
}

static void bt_idle_start(void)
{
    btstack_run_loop_set_timer_handler(&s_idle_timer, &bt_idle_timer_handler);
    btstack_run_loop_set_timer(&s_idle_timer, GW_IDLE_CHECK_MS);
    btstack_run_loop_add_timer(&s_idle_timer);
}

// ---------------------------------------------------------------------------
// Callback della custom platform (chiamate da Bluepad32 nel run loop)
// ---------------------------------------------------------------------------

static void plat_init(int argc, const char **argv)
{
    (void)argc;
    (void)argv;

#ifndef CONFIG_GW_BT_BREDR
    // Va fatto qui, prima che uni_init() chiami uni_bt_setup().
    uni_bt_bredr_set_enabled(false);
#endif
}

static void plat_on_init_complete(void)
{
    // Bluepad32 5.0.0-beta0 imposta SM_AUTHREQ_BONDING e toglie la modalità
    // "solo Secure Connections" in uni_bt_le_setup(); qui si applica la scelta
    // fatta in menuconfig.
    sm_set_authentication_requirements(GW_LE_AUTHREQ);

#ifdef CONFIG_GW_BT_LE_FAST_CONN
    // Bluepad32 apre le connessioni BLE con gap_connect(), che usa questi valori.
    gap_set_connection_parameters(GW_LE_SCAN_INTERVAL, GW_LE_SCAN_WINDOW, GW_LE_CONN_INTERVAL, GW_LE_CONN_INTERVAL,
                                  GW_LE_CONN_LATENCY, GW_LE_SUPERVISION_TIMEOUT, 0, 0);
#ifdef CONFIG_GW_BT_LE_REJECT_SLOW_CONN
    // Richieste del controller (L2CAP o LL): solo intervalli fino a 10 ms.
    le_connection_parameter_range_t range;
    gap_get_connection_parameter_range(&range);
    range.le_conn_interval_max = GW_LE_ACCEPT_INTERVAL_MAX;
    gap_set_connection_parameter_range(&range);
#endif
#endif
    // Parametri di connessione BLE nel log.
    s_le_hci_reg.callback = &bt_le_event_handler;
    hci_add_event_handler(&s_le_hci_reg);
    s_le_l2cap_reg.callback = &bt_le_event_handler;
    l2cap_add_event_handler(&s_le_l2cap_reg);
#ifdef CONFIG_GW_BT_LATENCY_STATS
    bt_stats_start();
#endif
    bt_idle_start();

#ifdef CONFIG_GW_BT_BREDR
    uni_bt_allow_incoming_connections(true);
    gap_register_classic_connection_filter(&bt_classic_accept);
    s_hci_reg.callback = &bt_hci_event_handler;
    hci_add_event_handler(&s_hci_reg);
#else
    uni_bt_allow_incoming_connections(false);
#endif

    bt_refresh_bonds();

#ifdef CONFIG_GW_BT_PAIRING_ALWAYS_AT_BOOT
    bool pairing = true;
#else
    // Senza controller memorizzati non c'è nulla da ricollegare: si parte in
    // abbinamento, così il primo collegamento non richiede la pagina web.
    bool pairing = (s_bonded_n == 0);
#endif

    ESP_LOGI(TAG, "Bluetooth pronto: abbinamento BLE \"%s\", BR/EDR %s, %d controller memorizzati",
             GW_LE_AUTHREQ_NAME, GW_BREDR_NAME, s_bonded_n);

    bt_set_pairing(pairing);
    bt_scan_update();

    // Da qui le gw_bt_*() possono usare il run loop; si eseguono subito le
    // richieste arrivate durante l'avvio.
    atomic_store(&s_bt_ready, true);
    bt_cmd_handler(NULL);
}

static uni_error_t plat_on_device_discovered(bd_addr_t addr, const char *name, uint16_t cod, uint8_t rssi)
{
    (void)name;
    (void)rssi;

    // Un solo controller alla volta.
    if (s_active != NULL) {
        return UNI_ERROR_IGNORE_DEVICE;
    }

    // Solo gamepad e joystick: niente mouse o tastiere.
    if ((cod & UNI_BT_COD_MINOR_MASK & (UNI_BT_COD_MINOR_GAMEPAD | UNI_BT_COD_MINOR_JOYSTICK)) == 0) {
        return UNI_ERROR_IGNORE_DEVICE;
    }

    if (bt_is_blocked(addr)) {
        return UNI_ERROR_IGNORE_DEVICE;
    }

    // Con l'abbinamento spento passano solo i controller già memorizzati.
    if (!s_pairing && !bt_is_bonded(addr)) {
        return UNI_ERROR_IGNORE_DEVICE;
    }

    return UNI_ERROR_SUCCESS;
}

static void plat_on_device_connected(uni_hid_device_t *d)
{
    // Collegato ma non ancora utilizzabile: si aspetta on_device_ready().
    ESP_LOGI(TAG, "Controller in collegamento: %s", bd_addr_to_str(d->conn.btaddr));
}

static void plat_on_device_disconnected(uni_hid_device_t *d)
{
    if (d != s_active) {
        return;
    }
    ESP_LOGI(TAG, "Controller scollegato: %s", bd_addr_to_str(d->conn.btaddr));
    bt_controller_gone();
}

static uni_error_t plat_on_device_ready(uni_hid_device_t *d)
{
    // Se si ritorna un errore Bluepad32 scollega e cancella il dispositivo.
    if (s_active != NULL && s_active != d) {
        ESP_LOGW(TAG, "Rifiutato %s: c'è già un controller collegato", bd_addr_to_str(d->conn.btaddr));
        return UNI_ERROR_NO_SLOTS;
    }
    if (!uni_hid_device_is_gamepad(d)) {
        ESP_LOGW(TAG, "Rifiutato %s: non è un gamepad", bd_addr_to_str(d->conn.btaddr));
        return UNI_ERROR_INVALID_CONTROLLER;
    }

    s_active = d;
    s_have_last = false;
    s_report_without_battery = false;
    memset(&s_ds5, 0, sizeof(s_ds5));
    s_lightbar_set = false;
    s_player_leds_set = false;
    s_idle_ref_valid = false;
    s_last_use_us = esp_timer_get_time(); // il conteggio dell'inattività riparte
#ifdef CONFIG_GW_BT_LATENCY_STATS
    bt_stats_reset(false);
#endif

    // DualSense: touchpad e stato di carica si leggono dal report grezzo.
    if (d->report_parser.parse_input_report == uni_hid_parser_ds5_parse_input_report) {
        d->report_parser.parse_input_report = ds5_parse_input_report;
    }

    // L'abbinamento, se c'è stato, è finito prima di arrivare qui.
    bt_refresh_bonds();

    s_shadow.connected = true;
    strlcpy(s_shadow.name, d->name[0] != '\0' ? d->name : "Controller", sizeof(s_shadow.name));
    memcpy(s_shadow.addr, d->conn.btaddr, sizeof(s_shadow.addr));
    s_shadow.battery = battery_percent(d->controller.battery);
    s_shadow.kind = bt_kind(d);
    s_shadow.classic = bt_is_classic(d);

    ESP_LOGI(TAG, "Controller pronto: \"%s\" %s (VID 0x%04x, PID 0x%04x, %s)", s_shadow.name,
             bd_addr_to_str(d->conn.btaddr), d->vendor_id, d->product_id, s_shadow.classic ? "BR/EDR" : "BLE");

#ifndef CONFIG_GW_BT_PAIRING_ALWAYS_AT_BOOT
    // Controller memorizzato: la finestra di abbinamento si chiude da sola.
    if (s_pairing && bt_is_bonded(d->conn.btaddr)) {
        bt_set_pairing(false);
    }
#endif
    bt_publish_status();

    // Con un controller collegato la scansione non serve più.
    bt_scan_update();

    // Stato iniziale a riposo: il PC sa subito che il controller c'è (e di che
    // tipo è, così crea subito il controller virtuale giusto).
    gw_input_t in;
    memset(&in, 0, sizeof(in));
    in.pad_idx = GW_PAD_IDX;
    in.connected = true;
    bt_fill_extras(&in);
    s_last_in = in;
    s_have_last = true;
    net_link_send_input(&in);

    return UNI_ERROR_SUCCESS;
}

static void plat_on_controller_data(uni_hid_device_t *d, uni_controller_t *ctl)
{
    if (d != s_active || ctl->klass != UNI_CONTROLLER_CLASS_GAMEPAD) {
        return;
    }

#ifdef CONFIG_GW_BT_LATENCY_STATS
    int64_t report_us = esp_timer_get_time();
    bt_stats_report(report_us);
#endif

    const uni_gamepad_t *gp = &ctl->gamepad;

    // Batteria. Bluepad32 non la chiede al controller (nessun client del Battery
    // Service GATT 0x180F): la ricava solo dall'uso HID "Battery Strength" dei
    // report in ingresso. Il parser Xbox (uni_hid_parser_xboxone_init_report)
    // azzera tutto lo stato, batteria compresa, all'inizio di ogni report e poi
    // valorizza solo i campi di quel report. Se la batteria viaggia in un report a
    // parte, nei report normali vale 0 ("non disponibile"): si tiene l'ultimo
    // valore ricevuto.
    bool has_battery = (ctl->battery != UNI_CONTROLLER_BATTERY_NOT_AVAILABLE);
    if (s_ds5.valid) {
        // DualSense: livello e stato di carica letti dal report grezzo.
        int16_t battery = ds5_battery_percent(s_ds5.status);
        if (battery != s_shadow.battery) {
            ESP_LOGI(TAG, "Batteria del controller: %d%%", battery);
            s_shadow.battery = battery;
            bt_publish_status();
        }
    } else if (!has_battery) {
        s_report_without_battery = true;
    } else {
        int16_t battery = battery_percent(ctl->battery);
        if (battery != s_shadow.battery) {
            ESP_LOGI(TAG, "Batteria del controller: %d%%", battery);
            s_shadow.battery = battery;
            bt_publish_status();
        }
    }

    // Per lo stesso motivo un report di sola batteria arriva con tutti i comandi a
    // zero. Non è lo stato reale: inoltrarlo rilascerebbe tasti, stick e grilletti
    // fino al report successivo, che il controller manda solo quando qualcosa
    // cambia. Si riconosce così solo se il controller manda anche report senza
    // batteria; con la batteria in ogni report, lo stato a riposo è vero.
    // Vale solo per i parser che leggono gli usi HID (Xbox): quelli a formato
    // fisso (DualSense, DualShock 4) hanno la batteria in ogni report, e per
    // loro un report a riposo è sempre lo stato reale.
    if (d->report_parser.parse_usage != NULL && has_battery && s_report_without_battery && gamepad_is_blank(gp)) {
        return;
    }

    gw_input_t in;
    memset(&in, 0, sizeof(in));
    in.pad_idx = GW_PAD_IDX;
    in.connected = true;
    in.buttons = gp->buttons;
    in.dpad = gp->dpad;
    in.misc = gp->misc_buttons;
    in.lx = clamp_axis(gp->axis_x);
    in.ly = clamp_axis(gp->axis_y);
    in.rx = clamp_axis(gp->axis_rx);
    in.ry = clamp_axis(gp->axis_ry);
    in.lt = clamp_pedal(gp->brake);
    in.rt = clamp_pedal(gp->throttle);
    bt_fill_extras(&in);

    if (s_shadow.kind == GW_BT_KIND_DUALSENSE) {
        // Bluepad32 dà i sensori calibrati in rad/s e m/s², con gli assi del
        // DualSense: si riportano alle unità del report USB.
        in.has_motion = true;
        for (int i = 0; i < 3; i++) {
            in.gyro[i] = float_to_i16(gp->gyro[i] * GW_GYRO_LSB_PER_RAD_S);
            in.accel[i] = float_to_i16(gp->accel[i] * GW_ACCEL_LSB_PER_MS2);
        }
    }

    bt_idle_note_input(&in);

    // Si inoltra solo ciò che cambia: il reinvio periodico lo fa net_link.
    if (s_have_last && memcmp(&in, &s_last_in, sizeof(in)) == 0) {
        return;
    }
    // I sensori cambiano a ogni report: se è cambiato solo il movimento (o il
    // touchpad) si limita la frequenza; tasti, stick e grilletti passano subito.
    int64_t now = esp_timer_get_time();
    if (s_have_last && input_controls_equal(&in, &s_last_in) && now - s_last_sent_us < GW_MOTION_MIN_INTERVAL_US) {
        return;
    }
    s_last_sent_us = now;
    s_last_in = in;
    s_have_last = true;
    net_link_send_input(&in);
#ifdef CONFIG_GW_BT_LATENCY_STATS
    bt_stats_sent(report_us);
#endif
}

static const uni_property_t *plat_get_property(uni_property_idx_t idx)
{
    (void)idx;
    return NULL;
}

static void plat_on_oob_event(uni_platform_oob_event_t event, void *data)
{
    // UNI_PLATFORM_OOB_GAMEPAD_SYSTEM_BUTTON: il tasto Xbox o PS arriva già al PC
    // dentro "misc". UNI_PLATFORM_OOB_BLUETOOTH_ENABLED: stato della scansione.
    (void)event;
    (void)data;
}

static struct uni_platform s_platform = {
    .name = "xbox-eth-gateway",
    .init = plat_init,
    .on_init_complete = plat_on_init_complete,
    .on_device_discovered = plat_on_device_discovered,
    .on_device_connected = plat_on_device_connected,
    .on_device_disconnected = plat_on_device_disconnected,
    .on_device_ready = plat_on_device_ready,
    .on_controller_data = plat_on_controller_data,
    .get_property = plat_get_property,
    .on_oob_event = plat_on_oob_event,
};

// ---------------------------------------------------------------------------
// API pubblica (gw_platform.h): chiamabile da qualsiasi task
// ---------------------------------------------------------------------------

// Registra la richiesta e sveglia il run loop. Se il Bluetooth non è ancora
// pronto la richiesta resta in s_req e la esegue plat_on_init_complete().
static void bt_post(uint32_t bits)
{
    portENTER_CRITICAL(&s_lock);
    s_req.bits |= bits;
    portEXIT_CRITICAL(&s_lock);

    if (atomic_load(&s_bt_ready)) {
        // Se la registrazione è già in coda BTstack non la duplica.
        btstack_run_loop_execute_on_main_thread(&s_cmd_reg);
    }
}

void gw_bt_run(void)
{
    // BTstack gira in questo task (il task main, core 0). Con la priorità di
    // default (1) ogni altro task occupato sul core 0 ritardava i report del
    // controller: vedi CONFIG_GW_BT_TASK_PRIORITY.
    vTaskPrioritySet(NULL, CONFIG_GW_BT_TASK_PRIORITY);
#if CONFIG_FREERTOS_HZ < 1000
    if (CONFIG_GW_BT_TASK_PRIORITY > 1) {
        ESP_LOGW(TAG, "CONFIG_FREERTOS_HZ=%d: con la priorità %d serve 1000 (sdkconfig.defaults)", CONFIG_FREERTOS_HZ,
                 CONFIG_GW_BT_TASK_PRIORITY);
    }
#endif

    // Come in examples/esp32/main/main.c di Bluepad32: senza la console di
    // Bluepad32 si usa la seriale bufferizzata di BTstack, così i log non
    // bloccano il run loop.
#ifdef CONFIG_ESP_CONSOLE_UART
#ifndef CONFIG_BLUEPAD32_USB_CONSOLE_ENABLE
    btstack_stdio_init();
#endif
#endif

    // Configura BTstack per il controller Bluetooth interno (VHCI).
    btstack_init();

    // Va chiamata prima di uni_init().
    uni_platform_set_custom(&s_platform);
    uni_init(0 /* argc */, NULL /* argv */);

    // Non ritorna.
    btstack_run_loop_execute();

    ESP_LOGE(TAG, "Il run loop di BTstack è terminato");
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

void gw_bt_get_status(gw_bt_status_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}

void gw_bt_set_pairing(bool enable)
{
    portENTER_CRITICAL(&s_lock);
    s_req.pairing = enable;
    portEXIT_CRITICAL(&s_lock);
    bt_post(CMD_PAIRING);
}

void gw_bt_disconnect(void)
{
    bt_post(CMD_DISCONNECT);
}

void gw_bt_forget_all(void)
{
    bt_post(CMD_FORGET);
}

void gw_bt_rumble(uint8_t pad_idx, uint16_t duration_ms, uint8_t weak, uint8_t strong)
{
    if (pad_idx != GW_PAD_IDX) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_req.rumble.duration_ms = duration_ms;
    s_req.rumble.weak = weak;
    s_req.rumble.strong = strong;
    portEXIT_CRITICAL(&s_lock);
    bt_post(CMD_RUMBLE);
}

void gw_bt_set_lightbar(uint8_t pad_idx, uint8_t r, uint8_t g, uint8_t b)
{
    if (pad_idx != GW_PAD_IDX) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_req.lightbar[0] = r;
    s_req.lightbar[1] = g;
    s_req.lightbar[2] = b;
    portEXIT_CRITICAL(&s_lock);
    bt_post(CMD_LIGHTBAR);
}

void gw_bt_set_player_leds(uint8_t pad_idx, uint8_t mask)
{
    if (pad_idx != GW_PAD_IDX) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_req.player_leds = mask;
    portEXIT_CRITICAL(&s_lock);
    bt_post(CMD_PLAYER_LEDS);
}
