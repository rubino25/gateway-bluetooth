// Bluetooth: "custom platform" di Bluepad32 (controller Xbox in BLE, DualSense in BR/EDR).
// Le funzioni qui sotto si possono chiamare da qualsiasi task: l'implementazione
// rimanda le operazioni al contesto BTstack.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Tipo del controller collegato (campo `kind` di gw_bt_status_t).
typedef enum {
    GW_BT_KIND_NONE = 0,    // nessun controller
    GW_BT_KIND_XBOX,        // Xbox Wireless Controller
    GW_BT_KIND_DUALSENSE,   // PlayStation 5
    GW_BT_KIND_DUALSHOCK4,  // PlayStation 4
    GW_BT_KIND_OTHER,       // altro gamepad riconosciuto da Bluepad32
} gw_bt_kind_t;

typedef struct {
    bool connected;        // un controller è collegato e pronto
    bool pairing_enabled;  // si accettano nuove connessioni
    char name[32];         // nome del controller, vuoto se nessuno
    uint8_t addr[6];       // indirizzo Bluetooth del controller
    int16_t battery;       // 0..100, -1 se sconosciuta
    uint8_t bonded_count;  // controller memorizzati
    uint8_t kind;          // gw_bt_kind_t, GW_BT_KIND_NONE se nessuno
    bool classic;          // collegato in Bluetooth classico (BR/EDR) e non in BLE
} gw_bt_status_t;

// Stato del controller come lo passa il Bluetooth al modulo di rete.
// Valori nei range di Bluepad32 (vedi protocol.h).
typedef struct {
    uint8_t pad_idx;
    bool connected;
    uint16_t buttons;
    uint8_t dpad;
    uint8_t misc;
    int16_t lx, ly, rx, ry;
    uint16_t lt, rt;

    // Campi del protocollo v2 (aggiunti il 2026-11-14 per il DualSense).
    uint8_t kind;          // gw_bt_kind_t
    uint8_t battery;       // 0..100, 255 se sconosciuta
    uint8_t power;         // gw_bt_power_t
    bool touchpad_click;
    bool has_touch;        // touch[] e touchpad_click validi
    bool has_motion;       // gyro[] e accel[] validi
    struct {
        uint16_t x, y;     // 0..1919, 0..1079
        bool active;
    } touch[2];
    int16_t gyro[3];       // assi del DualSense, 1 LSB = 1/16,384 °/s
    int16_t accel[3];      // assi del DualSense, 1 LSB = 1/8192 g
} gw_input_t;

// Stato di carica (campo `power` di gw_input_t).
typedef enum {
    GW_BT_POWER_UNKNOWN = 0,
    GW_BT_POWER_DISCHARGING,
    GW_BT_POWER_CHARGING,
    GW_BT_POWER_FULL,
} gw_bt_power_t;

// Inizializza BTstack e Bluepad32 ed entra nel run loop. Non ritorna:
// va chiamata per ultima da app_main().
void gw_bt_run(void) __attribute__((noreturn));

void gw_bt_get_status(gw_bt_status_t *out);

// Abilita o disabilita l'accettazione di nuovi controller.
void gw_bt_set_pairing(bool enable);

// Scollega il controller attuale.
void gw_bt_disconnect(void);

// Dimentica tutti i controller memorizzati.
void gw_bt_forget_all(void);

// Vibrazione: usata dal modulo di rete (pacchetti RUMBLE) e dal test della
// pagina web. weak/strong 0..255.
void gw_bt_rumble(uint8_t pad_idx, uint16_t duration_ms, uint8_t weak, uint8_t strong);

// Barra luminosa (DualSense, DualShock 4). Viene inviata al controller solo se
// diversa dall'ultima applicata; ignorata dai controller che non l'hanno.
void gw_bt_set_lightbar(uint8_t pad_idx, uint8_t r, uint8_t g, uint8_t b);

// LED giocatore del DualSense: maschera dei 5 LED (bit 0 = sinistra), come nel
// report di uscita USB. Solo se diversa dall'ultima applicata.
void gw_bt_set_player_leds(uint8_t pad_idx, uint8_t mask);
