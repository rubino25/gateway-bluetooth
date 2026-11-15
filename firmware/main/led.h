// LED RGB di stato (PWM LEDC) e pulsante utente.
// Indicazioni: blu lampeggiante = rete assente, blu fisso = rete presente,
// verde fisso = controller collegato, verde lampeggiante = abbinamento attivo,
// giallo lampeggiante 10 s = "Find Device".
#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Avvia LED e pulsante. Il modulo legge da solo lo stato di rete e Bluetooth.
// Pressione breve del pulsante: abilita l'abbinamento Bluetooth.
esp_err_t led_init(void);

// Accende o spegne del tutto il LED (viene salvato nelle impostazioni dal chiamante).
void led_set_enabled(bool enabled);

// Fa lampeggiare il LED di giallo per 10 secondi.
void led_find_device(void);
