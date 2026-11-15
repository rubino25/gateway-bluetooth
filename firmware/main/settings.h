// Impostazioni persistenti (NVS). Tutte le funzioni sono thread-safe.
// I valori di default vengono da menuconfig (Kconfig.projbuild).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    char device_name[32]; // nome mostrato nella pagina web
    char hostname[32];    // hostname mDNS, senza ".local"

    bool eth_dhcp;        // true = DHCP, false = IP statico qui sotto
    char eth_ip[16];
    char eth_netmask[16];
    char eth_gateway[16];
    char eth_dns[16];

    char pc_host[64];     // IP del PC; stringa vuota = broadcast
    uint16_t pc_port;     // porta UDP del protocollo v1

    uint32_t mg24_baud;     // 115200, 460800 o 921600
    uint16_t mg24_tcp_port; // porta TCP del ponte seriale (default 6638)

    bool led_enabled;

    // Minuti senza uso del controller dopo cui il gateway lo scollega
    // (0 = mai, ammessi 0..240). Ha effetto subito, senza riavvio.
    uint16_t bt_idle_timeout_min;
} gw_settings_t;

// Inizializza NVS e carica le impostazioni (default da menuconfig se assenti).
esp_err_t settings_init(void);

// Copia le impostazioni correnti in *out.
void settings_get(gw_settings_t *out);

// Valida e salva. Ritorna ESP_ERR_INVALID_ARG se un campo non è valido.
// Le modifiche di rete e hostname hanno effetto al riavvio.
esp_err_t settings_set(const gw_settings_t *in);

// Cancella impostazioni e password; non riavvia.
esp_err_t settings_factory_reset(void);

// Password della pagina web: si salva solo l'hash con salt, mai il testo.
// Lunghezza ammessa 8..64 caratteri (ESP_ERR_INVALID_ARG altrimenti).
// settings_password_check() può impiegare fino a circa 1 s: non chiamarla a
// ogni richiesta.
bool settings_password_is_set(void);
esp_err_t settings_password_set(const char *plain);
bool settings_password_check(const char *plain);
