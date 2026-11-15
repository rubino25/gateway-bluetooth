// Protocollo UDP tra ESP32 e ricevitore PC (little-endian): v1 e v2.
// Specifica completa in docs/protocol.md. Non cambiare
// il layout senza aggiornare anche pc-receiver/protocol.py.
#pragma once

#include <stdint.h>

#define GW_PROTO_MAGIC 0x4758 // "XG"
#define GW_PROTO_VERSION 1
#define GW_PROTO_TYPE_INPUT 0x01
#define GW_PROTO_TYPE_RUMBLE 0x02

#define GW_PROTO_FLAG_CONNECTED 0x01

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t version;
    uint8_t type;
} gw_proto_header_t;

// ESP32 -> PC, 24 byte.
typedef struct __attribute__((packed)) {
    gw_proto_header_t hdr;
    uint16_t seq;     // incrementale, con wrap
    uint8_t pad_idx;  // 0..3
    uint8_t flags;    // bit0 = connected
    uint16_t buttons; // maschera BUTTON_* di Bluepad32
    uint8_t dpad;     // maschera DPAD_* di Bluepad32
    uint8_t misc;     // maschera MISC_BUTTON_* di Bluepad32
    int16_t lx, ly, rx, ry; // -512..511
    uint16_t lt, rt;        // 0..1023
} gw_proto_input_t;

// PC -> ESP32, 12 byte.
typedef struct __attribute__((packed)) {
    gw_proto_header_t hdr;
    uint8_t pad_idx;
    uint8_t reserved0;
    uint16_t duration_ms;
    uint8_t weak;   // 0..255
    uint8_t strong; // 0..255
    uint16_t reserved1;
} gw_proto_rumble_t;

_Static_assert(sizeof(gw_proto_input_t) == 24, "INPUT deve essere di 24 byte");
_Static_assert(sizeof(gw_proto_rumble_t) == 12, "RUMBLE deve essere di 12 byte");

// ---------------------------------------------------------------------------
// Protocollo v2 (2026-11-14): estende la v1 per il DualSense (touchpad, sensori
// di movimento, barra luminosa, LED giocatore, batteria). Specifica completa in
// docs/protocol.md.
//
//  - Il firmware invia solo INPUT v2. Il ricevitore accetta v1 e v2 e risponde
//    nella stessa versione dell'ultimo INPUT ricevuto.
//  - Il firmware accetta sia RUMBLE v1 (12 byte) sia FEEDBACK v2 (20 byte), così
//    un ricevitore vecchio continua a far vibrare il controller.
// ---------------------------------------------------------------------------

#define GW_PROTO_VERSION2 2

// Altri bit di `flags` (solo v2).
#define GW_PROTO_FLAG_MOTION 0x02 // gyro e accel validi
#define GW_PROTO_FLAG_TOUCH 0x04  // touch e touchpad validi

// `kind`: tipo di controller (stessi valori di gw_bt_kind_t).
#define GW_PROTO_KIND_NONE 0
#define GW_PROTO_KIND_XBOX 1
#define GW_PROTO_KIND_DUALSENSE 2
#define GW_PROTO_KIND_DUALSHOCK4 3
#define GW_PROTO_KIND_OTHER 4

// `power`: stato di carica.
#define GW_PROTO_POWER_UNKNOWN 0
#define GW_PROTO_POWER_DISCHARGING 1
#define GW_PROTO_POWER_CHARGING 2
#define GW_PROTO_POWER_FULL 3

#define GW_PROTO_BATTERY_UNKNOWN 0xFF

// `ext_buttons`.
#define GW_PROTO_EXT_TOUCHPAD 0x01 // clic del touchpad

// `touch_active`.
#define GW_PROTO_TOUCH1_ACTIVE 0x01
#define GW_PROTO_TOUCH2_ACTIVE 0x02

// ESP32 -> PC, 52 byte. I primi 24 byte hanno lo stesso layout della v1.
typedef struct __attribute__((packed)) {
    gw_proto_input_t base;  // hdr.version = 2
    uint8_t kind;           // GW_PROTO_KIND_*
    uint8_t battery;        // 0..100, GW_PROTO_BATTERY_UNKNOWN se non nota
    uint8_t power;          // GW_PROTO_POWER_*
    uint8_t ext_buttons;    // GW_PROTO_EXT_*
    struct __attribute__((packed)) {
        uint16_t x; // 0..1919
        uint16_t y; // 0..1079
    } touch[2];
    uint8_t touch_active;   // GW_PROTO_TOUCH*_ACTIVE
    uint8_t reserved0;
    int16_t gyro[3];        // X, Y, Z del DualSense, 1 LSB = 1/16,384 °/s
    int16_t accel[3];       // X, Y, Z del DualSense, 1 LSB = 1/8192 g
    uint16_t reserved1;
} gw_proto_input_v2_t;

// Bit di `flags` del FEEDBACK v2.
#define GW_PROTO_FB_LIGHTBAR 0x01    // led_r/g/b validi
#define GW_PROTO_FB_PLAYER_LEDS 0x02 // player_leds valido

// PC -> ESP32, 20 byte. I primi 12 byte hanno lo stesso layout del RUMBLE v1
// (stesso tipo GW_PROTO_TYPE_RUMBLE) e la stessa semantica per la vibrazione.
typedef struct __attribute__((packed)) {
    gw_proto_rumble_t base; // hdr.version = 2
    uint8_t flags;          // GW_PROTO_FB_*
    uint8_t led_r, led_g, led_b;
    uint8_t player_leds;    // maschera dei 5 LED del DualSense, bit 0 = sinistra
    uint8_t reserved[3];
} gw_proto_feedback_v2_t;

_Static_assert(sizeof(gw_proto_input_v2_t) == 52, "INPUT v2 deve essere di 52 byte");
_Static_assert(sizeof(gw_proto_feedback_v2_t) == 20, "FEEDBACK v2 deve essere di 20 byte");
