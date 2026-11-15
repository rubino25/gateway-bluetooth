// Collegamento UDP con il PC: invio dei pacchetti INPUT, ricezione dei RUMBLE,
// keepalive. Protocollo in protocol.h.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_platform.h"

typedef struct {
    bool pc_seen;            // ricevuto almeno un pacchetto dal PC negli ultimi 5 s
    char pc_ip[16];          // indirizzo da cui è arrivato l'ultimo pacchetto
    uint32_t tx_packets;
    uint32_t rx_packets;
    uint32_t last_rx_age_ms; // UINT32_MAX se mai ricevuto
} net_link_status_t;

// Crea il socket e i task (ricezione RUMBLE, keepalive ogni 100 ms).
esp_err_t net_link_start(void);

// Registra il nuovo stato del controller e lo invia subito. Viene chiamata dal
// contesto BTstack: non deve bloccare.
void net_link_send_input(const gw_input_t *in);

// Thread-safe.
void net_link_get_status(net_link_status_t *out);
