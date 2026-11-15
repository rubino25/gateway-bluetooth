// Collegamento UDP con il PC (protocollo v2, vedi protocol.h e docs/protocol.md).
//
//  - net_link_send_input() viene chiamata dal run loop di BTstack a ogni cambio
//    di stato del controller: registra lo stato e lo invia subito, senza bloccare.
//  - Il task "gw_keepalive" reinvia l'ultimo stato ogni 100 ms, anche quando
//    non c'è nessun controller (flags.connected = 0): così il PC sa che il
//    gateway è vivo.
//  - Il task "gw_udp_rx" riceve i pacchetti RUMBLE (v1) e FEEDBACK (v2), li
//    valida e li passa a gw_bt_rumble() / gw_bt_set_lightbar() /
//    gw_bt_set_player_leds(), che li eseguono nel contesto BTstack.
//  - Gli INPUT partono sempre in formato v2 (52 byte).
//
// I pacchetti partono dalla stessa porta su cui si ascolta (pc_port): il PC
// risponde all'indirizzo e alla porta sorgente degli INPUT.
//
// Destinazione: l'IP del PC (pc_host) se impostato. Se pc_host è vuoto si parte
// in broadcast e, appena il PC si fa sentire (RUMBLE di presenza ogni secondo),
// si passa all'unicast verso l'IP da cui arriva; si torna al broadcast dopo
// PC_SEEN_TIMEOUT_MS di silenzio. In Wi-Fi il broadcast è lento: l'access
// point lo trattiene fino al beacon DTIM se il PC è in risparmio energetico e
// lo trasmette alla velocità minima, senza ritrasmissioni.
// L'ESP32 è little-endian come il protocollo: le struct packed si copiano così
// come sono.

#include "net_link.h"

#include <errno.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "protocol.h"
#include "settings.h"

#define KEEPALIVE_PERIOD_MS 100
#define SETTINGS_REFRESH_MS 1000 // ogni quanto si rilegge pc_host dalle impostazioni
#define PC_SEEN_TIMEOUT_MS 5000
#define RX_BUF_SIZE 64
#define TASK_STACK_SIZE 3072
#define TASK_PRIORITY 5

static const char *TAG = "net_link";

// Protegge tutto lo stato qui sotto. Spinlock e non mutex: lo prende anche il
// run loop di BTstack, che non deve mai restare in attesa, e le sezioni
// protette sono solo copie di pochi byte.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static int s_sock = -1;
static bool s_started;

static gw_input_t s_last_input;   // ultimo stato ricevuto dal Bluetooth
static uint16_t s_seq;            // numero del prossimo pacchetto INPUT
static struct sockaddr_in s_dest; // destinazione degli INPUT
static bool s_dest_valid;
static bool s_dest_auto; // pc_host vuoto: unicast verso il PC appena si fa sentire

static uint32_t s_tx_packets;
static uint32_t s_rx_packets;
static int64_t s_last_rx_us = -1; // -1 = mai ricevuto
static uint32_t s_last_rx_ip;     // network byte order

// Calcola la destinazione dalle impostazioni: IP del PC, oppure broadcast se
// pc_host è vuoto o non è un indirizzo IPv4 valido. Ritorna true se l'IP del PC
// è impostato, false se si usa il broadcast (e quindi l'unicast automatico).
static bool dest_from_settings(const gw_settings_t *cfg, struct sockaddr_in *out)
{
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(cfg->pc_port);
    out->sin_addr.s_addr = htonl(INADDR_BROADCAST);

    if (cfg->pc_host[0] == '\0') {
        return false;
    }

    ip4_addr_t ip;
    if (ip4addr_aton(cfg->pc_host, &ip)) {
        out->sin_addr.s_addr = ip.addr;
        return true;
    }

    static bool warned;
    if (!warned) {
        warned = true;
        ESP_LOGW(TAG, "pc_host \"%s\" non è un IPv4 valido: uso il broadcast", cfg->pc_host);
    }
    return false;
}

// Rilegge pc_host: un cambio dalla pagina web vale senza riavviare. La porta
// resta quella letta all'avvio, perché è anche la porta su cui si ascolta.
static void dest_refresh(void)
{
    gw_settings_t cfg;
    settings_get(&cfg);

    struct sockaddr_in dest;
    bool fixed = dest_from_settings(&cfg, &dest);

    portENTER_CRITICAL(&s_lock);
    dest.sin_port = s_dest.sin_port;
    s_dest = dest;
    s_dest_valid = true;
    s_dest_auto = !fixed;
    portEXIT_CRITICAL(&s_lock);
}

// Costruisce e invia un INPUT con l'ultimo stato. Stato e numero di sequenza
// vengono presi insieme sotto lock: se due task inviano nello stesso momento,
// il pacchetto con il seq più alto porta sempre lo stato più recente.
// Riempie un INPUT v2 dallo stato del Bluetooth (seq escluso).
static void build_input_v2(const gw_input_t *in, gw_proto_input_v2_t *pkt)
{
    memset(pkt, 0, sizeof(*pkt));
    pkt->base.hdr.magic = GW_PROTO_MAGIC;
    pkt->base.hdr.version = GW_PROTO_VERSION2;
    pkt->base.hdr.type = GW_PROTO_TYPE_INPUT;
    pkt->base.pad_idx = in->pad_idx;
    pkt->base.flags = in->connected ? GW_PROTO_FLAG_CONNECTED : 0;
    if (!in->connected) {
        pkt->kind = GW_PROTO_KIND_NONE;
        pkt->battery = GW_PROTO_BATTERY_UNKNOWN;
        return;
    }
    if (in->has_motion) {
        pkt->base.flags |= GW_PROTO_FLAG_MOTION;
    }
    if (in->has_touch) {
        pkt->base.flags |= GW_PROTO_FLAG_TOUCH;
    }
    pkt->base.buttons = in->buttons;
    pkt->base.dpad = in->dpad;
    pkt->base.misc = in->misc;
    pkt->base.lx = in->lx;
    pkt->base.ly = in->ly;
    pkt->base.rx = in->rx;
    pkt->base.ry = in->ry;
    pkt->base.lt = in->lt;
    pkt->base.rt = in->rt;

    pkt->kind = in->kind;
    pkt->battery = in->battery;
    pkt->power = in->power;
    pkt->ext_buttons = in->touchpad_click ? GW_PROTO_EXT_TOUCHPAD : 0;
    for (int i = 0; i < 2; i++) {
        pkt->touch[i].x = in->touch[i].x;
        pkt->touch[i].y = in->touch[i].y;
        if (in->touch[i].active) {
            pkt->touch_active |= (i == 0) ? GW_PROTO_TOUCH1_ACTIVE : GW_PROTO_TOUCH2_ACTIVE;
        }
    }
    for (int i = 0; i < 3; i++) {
        pkt->gyro[i] = in->gyro[i];
        pkt->accel[i] = in->accel[i];
    }
}

static void send_last_input(void)
{
    gw_proto_input_v2_t pkt;
    struct sockaddr_in dest;
    int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL(&s_lock);
    if (s_sock < 0 || !s_dest_valid) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    int sock = s_sock;
    dest = s_dest;
    if (s_dest_auto && s_last_rx_us >= 0 && now_us - s_last_rx_us <= (int64_t)PC_SEEN_TIMEOUT_MS * 1000) {
        dest.sin_addr.s_addr = s_last_rx_ip; // il PC si è fatto sentire: unicast
    }
    build_input_v2(&s_last_input, &pkt);
    pkt.base.seq = s_seq++;
    portEXIT_CRITICAL(&s_lock);

    // MSG_DONTWAIT: mai in attesa. Se l'Ethernet è giù l'invio fallisce e
    // basta: ci riprova il keepalive.
    int n = sendto(sock, &pkt, sizeof(pkt), MSG_DONTWAIT, (struct sockaddr *)&dest, sizeof(dest));
    if (n == (int)sizeof(pkt)) {
        portENTER_CRITICAL(&s_lock);
        s_tx_packets++;
        portEXIT_CRITICAL(&s_lock);
    }
}

static void keepalive_task(void *arg)
{
    (void)arg;

    TickType_t last_wake = xTaskGetTickCount();
    uint32_t since_refresh_ms = 0;

    for (;;) {
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(KEEPALIVE_PERIOD_MS));

        since_refresh_ms += KEEPALIVE_PERIOD_MS;
        if (since_refresh_ms >= SETTINGS_REFRESH_MS) {
            since_refresh_ms = 0;
            dest_refresh();
        }

        send_last_input();
    }
}

static void handle_packet(const uint8_t *buf, int len, const struct sockaddr_in *from)
{
    // Dal PC arrivano RUMBLE v1 (12 byte) o FEEDBACK v2 (20 byte, i primi 12
    // uguali al RUMBLE v1).
    gw_proto_feedback_v2_t fb;
    memset(&fb, 0, sizeof(fb));
    if (len == (int)sizeof(gw_proto_rumble_t)) {
        memcpy(&fb.base, buf, sizeof(fb.base));
        if (fb.base.hdr.version != GW_PROTO_VERSION) {
            return;
        }
    } else if (len == (int)sizeof(gw_proto_feedback_v2_t)) {
        memcpy(&fb, buf, sizeof(fb));
        if (fb.base.hdr.version != GW_PROTO_VERSION2) {
            return;
        }
    } else {
        return;
    }

    const gw_proto_rumble_t pkt = fb.base;
    if (pkt.hdr.magic != GW_PROTO_MAGIC || pkt.hdr.type != GW_PROTO_TYPE_RUMBLE) {
        return;
    }
    if (pkt.pad_idx > 3) {
        return;
    }

    // Barra luminosa e LED giocatore: il FEEDBACK li ripete a ogni invio, il
    // Bluetooth li inoltra al controller solo quando cambiano.
    if (fb.flags & GW_PROTO_FB_LIGHTBAR) {
        gw_bt_set_lightbar(pkt.pad_idx, fb.led_r, fb.led_g, fb.led_b);
    }
    if (fb.flags & GW_PROTO_FB_PLAYER_LEDS) {
        gw_bt_set_player_leds(pkt.pad_idx, fb.player_leds);
    }

    portENTER_CRITICAL(&s_lock);
    s_rx_packets++;
    s_last_rx_us = esp_timer_get_time();
    s_last_rx_ip = from->sin_addr.s_addr;
    portEXIT_CRITICAL(&s_lock);

    // Il ricevitore manda ogni secondo un RUMBLE a zero come segnale di presenza.
    // Uno stop va inoltrato solo se la vibrazione era stata avviata dalla rete:
    // altrimenti interromperebbe il test di vibrazione della pagina web.
    // La variabile è usata solo da questo task.
    static bool s_net_rumble_active;
    bool is_stop = pkt.duration_ms == 0 || (pkt.weak == 0 && pkt.strong == 0);
    if (is_stop && !s_net_rumble_active) {
        return;
    }
    s_net_rumble_active = !is_stop;

    gw_bt_rumble(pkt.pad_idx, pkt.duration_ms, pkt.weak, pkt.strong);
}

static void rx_task(void *arg)
{
    (void)arg;

    uint8_t buf[RX_BUF_SIZE];

    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (len < 0) {
            ESP_LOGW(TAG, "recvfrom: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (from_len < sizeof(from) || from.sin_family != AF_INET) {
            continue;
        }
        handle_packet(buf, len, &from);
    }
}

esp_err_t net_link_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    // Serve lo stack TCP/IP anche se l'Ethernet non è partita. La chiamata non
    // fa nulla se lo stack è già inizializzato.
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return err;
    }

    gw_settings_t cfg;
    settings_get(&cfg);
    if (cfg.pc_port == 0) {
        ESP_LOGE(TAG, "Porta UDP non valida");
        return ESP_ERR_INVALID_ARG;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        return ESP_FAIL;
    }

    int on = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) != 0) {
        ESP_LOGW(TAG, "SO_BROADCAST: errno %d", errno);
    }

    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(cfg.pc_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
        ESP_LOGE(TAG, "bind sulla porta %u: errno %d", (unsigned)cfg.pc_port, errno);
        close(sock);
        return ESP_FAIL;
    }

    struct sockaddr_in dest;
    bool fixed = dest_from_settings(&cfg, &dest);

    portENTER_CRITICAL(&s_lock);
    memset(&s_last_input, 0, sizeof(s_last_input)); // nessun controller collegato
    s_dest = dest;
    s_dest_valid = true;
    s_dest_auto = !fixed;
    s_sock = sock;
    portEXIT_CRITICAL(&s_lock);

    if (xTaskCreate(rx_task, "gw_udp_rx", TASK_STACK_SIZE, NULL, TASK_PRIORITY, NULL) != pdPASS ||
        xTaskCreate(keepalive_task, "gw_keepalive", TASK_STACK_SIZE, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        // Il socket resta aperto: un task potrebbe già usarlo.
        ESP_LOGE(TAG, "Creazione dei task fallita");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "UDP porta %u, destinazione %s", (unsigned)cfg.pc_port,
             fixed ? cfg.pc_host : "broadcast, poi unicast verso il PC appena risponde");
    return ESP_OK;
}

void net_link_send_input(const gw_input_t *in)
{
    if (in == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    s_last_input = *in;
    portEXIT_CRITICAL(&s_lock);

    send_last_input();
}

void net_link_get_status(net_link_status_t *out)
{
    if (out == NULL) {
        return;
    }

    int64_t last_rx_us;
    uint32_t last_rx_ip;

    memset(out, 0, sizeof(*out));

    portENTER_CRITICAL(&s_lock);
    out->tx_packets = s_tx_packets;
    out->rx_packets = s_rx_packets;
    last_rx_us = s_last_rx_us;
    last_rx_ip = s_last_rx_ip;
    portEXIT_CRITICAL(&s_lock);

    if (last_rx_us < 0) {
        out->pc_seen = false;
        out->last_rx_age_ms = UINT32_MAX;
        return;
    }

    int64_t age_ms = (esp_timer_get_time() - last_rx_us) / 1000;
    if (age_ms < 0) {
        age_ms = 0;
    }
    out->last_rx_age_ms = age_ms >= UINT32_MAX ? UINT32_MAX - 1 : (uint32_t)age_ms;
    out->pc_seen = age_ms <= PC_SEEN_TIMEOUT_MS;

    ip4_addr_t ip = {.addr = last_rx_ip};
    ip4addr_ntoa_r(&ip, out->pc_ip, sizeof(out->pc_ip));
}
