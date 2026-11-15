// Pagina web di gestione (esp_http_server) e API JSON.
#pragma once

#include "esp_err.h"

// Avvia il server HTTP sulla porta 80. Legge lo stato dagli altri moduli
// tramite le loro funzioni *_get_status().
esp_err_t web_ui_start(void);
