// Ponte seriale tra l'EFR32MG24 (Zigbee/Thread) e un client TCP (Zigbee2MQTT, ZHA).
// Stesso comportamento del firmware originale: porta 6638, un solo client alla
// volta, nessun hardware flow control. Tutte le funzioni sono thread-safe.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool client_connected;
    char client_ip[16];
    uint32_t connected_s;     // secondi dall'inizio della connessione corrente
    uint32_t baud;            // baud rate attuale della UART
    bool bootloader_mode;     // true se l'MG24 è stato messo in bootloader
    uint32_t bytes_to_radio;  // TCP -> UART
    uint32_t bytes_from_radio; // UART -> TCP
} mg24_status_t;

// Configura UART e GPIO, resetta l'MG24 in modalità applicazione e apre il
// server TCP.
esp_err_t mg24_bridge_start(void);

void mg24_get_status(mg24_status_t *out);

// Reset dell'MG24 in modalità applicazione (pin bootloader basso).
esp_err_t mg24_reset(void);

// Entra nel bootloader Gecko: UART a 115200, pin bootloader alto, reset.
esp_err_t mg24_enter_bootloader(void);

// Esce dal bootloader e ripristina il baud rate dell'applicazione.
esp_err_t mg24_exit_bootloader(void);

// Cambia il baud rate dell'applicazione e lo salva nelle impostazioni.
// Valori ammessi: 115200, 460800, 921600.
esp_err_t mg24_set_baud(uint32_t baud);
