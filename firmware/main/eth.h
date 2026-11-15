// Ethernet (IP101 in RMII) + esp_netif + mDNS. Il Wi-Fi resta spento.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool link_up;
    bool has_ip;
    bool dhcp;
    char ip[16];
    char netmask[16];
    char gateway[16];
    char dns[16];
    uint8_t mac[6];
} eth_status_t;

// Avvia il driver, DHCP o IP statico secondo settings, e mDNS con l'hostname
// configurato. Non blocca in attesa dell'indirizzo.
esp_err_t eth_start(void);

// Thread-safe.
void eth_get_status(eth_status_t *out);
