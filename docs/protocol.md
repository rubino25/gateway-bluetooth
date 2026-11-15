# Protocollo ESP32 ↔ ricevitore PC (v1 e v2)

La v1 è descritta per prima; la **v2** (2026-11-14, per il
DualSense) è descritta in fondo a questo file. Allineato a `firmware/main/protocol.h`
(`gw_proto_input_t`, `gw_proto_rumble_t`, `gw_proto_input_v2_t`,
`gw_proto_feedback_v2_t`). Implementazione lato PC: `pc-receiver/protocol.py` e
`pc-receiver/mapping.py`. Se cambia il layout vanno aggiornati insieme:
`protocol.h`, questo file e `pc-receiver`.

**Chi parla quale versione**: il firmware attuale invia solo INPUT v2 e accetta sia
RUMBLE v1 sia FEEDBACK v2. Il ricevitore accetta INPUT v1 e v2 e risponde nella
versione dell'ultimo INPUT ricevuto. Un ricevitore vecchio (solo v1) scarta gli
INPUT v2: dopo aver aggiornato il firmware va aggiornato anche `pc-receiver`.

## Trasporto

UDP, little-endian, porta default **47900**.

Destinazione dei pacchetti INPUT: IP del PC impostato nel firmware, oppure broadcast
se vuoto. Il PC risponde all'indirizzo sorgente da cui riceve gli INPUT.

## Header comune (4 byte)

| off | tipo | campo   | valore                  |
|-----|------|---------|-------------------------|
| 0   | u16  | magic   | 0x4758 ("XG")           |
| 2   | u8   | version | 1                       |
| 3   | u8   | type    | 0x01 INPUT, 0x02 RUMBLE |

Sul filo il magic è la coppia di byte `58 47` (ASCII `X` `G`).

## INPUT (ESP32 → PC), 24 byte totali

| off | tipo   | campo          | note                                |
|-----|--------|----------------|-------------------------------------|
| 4   | u16    | seq            | incrementale, wrap                  |
| 6   | u8     | pad_idx        | indice controller (0..3)            |
| 7   | u8     | flags          | bit0 = connected                    |
| 8   | u16    | buttons        | maschera BUTTON_* di Bluepad32      |
| 10  | u8     | dpad           | maschera DPAD_* di Bluepad32        |
| 11  | u8     | misc           | MISC_BUTTON_* (system/select/start) |
| 12  | i16×4  | lx, ly, rx, ry | range Bluepad32 −512..511           |
| 20  | u16×2  | lt, rt         | range Bluepad32 0..1023             |

- Keepalive: il firmware reinvia l'ultimo stato almeno ogni 100 ms anche se invariato.
- Alla disconnessione del controller il firmware invia un INPUT con `connected = 0`.

## RUMBLE (PC → ESP32), 12 byte totali

| off | tipo | campo       | note     |
|-----|------|-------------|----------|
| 4   | u8   | pad_idx     |          |
| 5   | u8   | reserved    | 0        |
| 6   | u16  | duration_ms |          |
| 8   | u8   | weak        | 0..255   |
| 9   | u8   | strong      | 0..255   |
| 10  | u16  | reserved    | 0        |

## Maschere Bluepad32

Verificate il 2026-11-14 su `uni_gamepad.h` e `uni_common.h` (branch `main`):
ogni costante è `BIT(n)` con `BIT(nr) = 1UL << nr`.

| Campo     | Costante           | Valore |
|-----------|--------------------|--------|
| `dpad`    | DPAD_UP            | 0x01   |
| `dpad`    | DPAD_DOWN          | 0x02   |
| `dpad`    | DPAD_RIGHT         | 0x04   |
| `dpad`    | DPAD_LEFT          | 0x08   |
| `buttons` | BUTTON_A           | 0x0001 |
| `buttons` | BUTTON_B           | 0x0002 |
| `buttons` | BUTTON_X           | 0x0004 |
| `buttons` | BUTTON_Y           | 0x0008 |
| `buttons` | BUTTON_SHOULDER_L  | 0x0010 |
| `buttons` | BUTTON_SHOULDER_R  | 0x0020 |
| `buttons` | BUTTON_TRIGGER_L   | 0x0040 |
| `buttons` | BUTTON_TRIGGER_R   | 0x0080 |
| `buttons` | BUTTON_THUMB_L     | 0x0100 |
| `buttons` | BUTTON_THUMB_R     | 0x0200 |
| `misc`    | MISC_BUTTON_SYSTEM | 0x01   |
| `misc`    | MISC_BUTTON_SELECT | 0x02   |
| `misc`    | MISC_BUTTON_START  | 0x04   |
| `misc`    | MISC_BUTTON_CAPTURE| 0x08   |

Attenzione: nel D-pad di Bluepad32 RIGHT viene prima di LEFT, al contrario di XInput.

## Mappatura verso XInput (lato PC)

- Stick: valore × 64, clamp a −32768..32767. L'asse Y va **invertito**
  (in Bluepad32 "su" è negativo, in XInput è positivo) — verificare sul campo;
  nel ricevitore è l'opzione `invert_y` di `config.toml`.
- Grilletti: valore >> 2 → 0..255.
- Pacchetti con `seq` più vecchio dell'ultimo ricevuto vanno scartati (gestire il wrap).
- Nessun pacchetto per 1 s → rilasciare tutti i tasti; per 5 s → scollegare il pad virtuale.

| Bluepad32                       | Xbox 360 (VIIPER)       |
|---------------------------------|-------------------------|
| DPAD_UP / DOWN / LEFT / RIGHT   | 0x0001 / 0x0002 / 0x0004 / 0x0008 |
| MISC_BUTTON_START               | Start 0x0010            |
| MISC_BUTTON_SELECT              | Back 0x0020             |
| BUTTON_THUMB_L / THUMB_R        | LS 0x0040 / RS 0x0080   |
| BUTTON_SHOULDER_L / SHOULDER_R  | LB 0x0100 / RB 0x0200   |
| MISC_BUTTON_SYSTEM              | Guide 0x0400            |
| BUTTON_A / B / X / Y            | 0x1000 / 0x2000 / 0x4000 / 0x8000 |
| MISC_BUTTON_CAPTURE             | nessun equivalente, ignorato |
| BUTTON_TRIGGER_L / TRIGGER_R    | LT / RT = 255 solo se il valore analogico è 0 |

Vibrazione: VIIPER manda 2 byte, motore sinistro (grande, bassa frequenza) e destro
(piccolo, alta frequenza). Nel RUMBLE: `strong` = sinistro, `weak` = destro.

## Scelte del ricevitore non scritte nella specifica

Non cambiano il formato dei pacchetti; servono a chi scrive il firmware.

- **`seq` uguale all'ultimo**: accettato (non è "più vecchio"). Così il keepalive
  funziona anche se il firmware non incrementa `seq` quando reinvia lo stesso stato.
- **Riavvio del dongle**: dopo 1 s senza pacchetti validi, o dopo 16 pacchetti
  scartati di fila, il ricevitore riparte dal `seq` che arriva.
- **`connected = 0`**: tutti i comandi vengono rilasciati subito; il pad virtuale
  viene rimosso se non arrivano INPUT con `connected = 1` per 5 s.
- **Durata della vibrazione**: VIIPER comunica solo i livelli. Finché sono diversi
  da zero il ricevitore reinvia il RUMBLE ogni 250 ms con `duration_ms = 500`;
  quando tornano a zero invia un RUMBLE con `weak = strong = 0` e `duration_ms = 0`,
  che vale come "ferma".
- **Segnale di presenza del PC**: il protocollo non ha un keepalive PC → ESP32,
  ma il firmware mostra "PC visto" solo se riceve pacchetti. A riposo il ricevitore
  invia quindi ogni secondo un RUMBLE tutto a zero (disattivabile con
  `heartbeat_ms = 0`). Il firmware dovrebbe contarlo come pacchetto ricevuto senza
  interrompere una vibrazione avviata da altri (es. il test della pagina web).

---

## Versione 2

Header uguale alla v1 con `version = 2`. Stessi tipi: 0x01 INPUT, 0x02 RUMBLE (in v2
chiamato FEEDBACK). Stesse regole di keepalive, `seq`, presenza del PC e durata della
vibrazione.

### INPUT v2 (ESP32 → PC), 52 byte totali

I primi 24 byte hanno lo stesso layout della v1.

| off | tipo  | campo        | note |
|-----|-------|--------------|------|
| 0   | —     | come v1      | header, seq, pad_idx, flags, buttons, dpad, misc, stick, grilletti |
| 7   | u8    | flags        | bit0 connected, **bit1 motion** (gyro/accel validi), **bit2 touch** (touchpad valido) |
| 24  | u8    | kind         | 0 nessuno, 1 Xbox, 2 DualSense, 3 DualShock 4, 4 altro |
| 25  | u8    | battery      | 0..100 %, 255 = sconosciuta |
| 26  | u8    | power        | 0 sconosciuto, 1 in scarica, 2 in carica, 3 carica completa |
| 27  | u8    | ext_buttons  | bit0 = clic del touchpad |
| 28  | u16×2 | touch1 x, y  | x 0..1919, y 0..1079 |
| 32  | u16×2 | touch2 x, y  | |
| 36  | u8    | touch_active | bit0 dito 1, bit1 dito 2 |
| 37  | u8    | reserved     | 0 |
| 38  | i16×3 | gyro x, y, z | assi del DualSense, 1 LSB = 1/16,384 °/s |
| 44  | i16×3 | accel x, y, z| assi del DualSense, 1 LSB = 1/8192 g |
| 50  | u16   | reserved     | 0 |

- Con `connected = 0` i campi v2 sono a zero, `battery = 255`.
- `kind`, `battery` e `power` sono validi per ogni controller (`battery = 255` se il
  controller non la comunica). Touchpad e sensori solo con i rispettivi bit di
  `flags`: oggi solo per il DualSense.
- Sensori: Bluepad32 li fornisce già calibrati (rad/s, m/s²); il firmware li riporta
  alle unità del report USB del DualSense, le stesse di VIIPER (16,384 LSB per °/s,
  8192 LSB per g).
- Sul DualSense `misc` bit 3 (`MISC_BUTTON_CAPTURE`) è il tasto **microfono**.
- Il firmware manda un INPUT a ogni cambio. Se cambiano solo sensori o posizione
  delle dita, al massimo uno ogni 4 ms.

### FEEDBACK v2 (PC → ESP32), 20 byte totali

I primi 12 byte hanno lo stesso layout e significato del RUMBLE v1.

| off | tipo | campo       | note |
|-----|------|-------------|------|
| 0   | —    | come RUMBLE v1 | header, pad_idx, duration_ms, weak, strong |
| 12  | u8   | flags       | bit0 = barra luminosa valida, bit1 = LED giocatore validi |
| 13  | u8×3 | led r, g, b | 0..255 |
| 16  | u8   | player_leds | maschera dei 5 LED del DualSense, bit0 = il più a sinistra |
| 17  | u8×3 | reserved    | 0 |

- Il ricevitore mette nelle luci l'ultimo valore chiesto dal gioco e le ripete in
  ogni FEEDBACK, anche nel segnale di presenza; il firmware le inoltra al controller
  solo quando cambiano. Senza i bit di `flags` il controller tiene le luci che ha.
- LED giocatore: Bluepad32 accetta 4 "posti". Gli schemi standard di Sony (0x04,
  0x0A, 0x15, 0x1B = giocatore 1..4) sono riprodotti esatti; gli altri in modo
  approssimato (il LED centrale si accende solo nello schema del giocatore 1).

### DualSense virtuale (VIIPER 0.9.0, tipo `dualsense`)

Verificato sul sorgente del tag `v0.9.0` (`device/dualsense/*.go`).

- Stream verso VIIPER: 33 byte — `LX LY RX RY` i8 (−128..127, "su" negativo come in
  Bluepad32: **niente inversione**), `buttons` u32, `dpad` u8 (su 1, giù 2,
  sinistra 4, destra 8), `L2` `R2` u8, dito 1 (`x` u16, `y` u16, attivo u8), dito 2,
  `gyro` 3×i16, `accel` 3×i16.
- Ritorno: 6 byte — vibrazione piccola, grande, `R G B`, LED giocatore.
- Pulsanti: Quadrato 0x10, Croce 0x20, Cerchio 0x40, Triangolo 0x80, L1 0x100,
  R1 0x200, L2 0x400, R2 0x800, Create 0x1000, Options 0x2000, L3 0x4000,
  R3 0x8000, PS 0x10000, touchpad 0x20000, microfono 0x40000.
- Batteria: VIIPER 0.9.0 la accetta solo alla creazione del dispositivo
  (`deviceSpecific.battery_status`, byte di stato del report USB: bit 0..3 livello
  0..10, bit 4..7 0 in scarica / 1 in carica / 2 carica). Nessuna API per
  aggiornarla dopo: il PC vede il livello del momento in cui il pad virtuale nasce.
- VIIPER manda colore e LED a zero quando il gioco invia un report che non li
  imposta: il ricevitore tratta lo zero come "non impostato" (un gioco non può
  spegnere del tutto la barra luminosa).
- **Non gestiti da VIIPER 0.9.0**: grilletti adattivi, aptica audio, altoparlante,
  microfono e jack audio del controller.

| Bluepad32 | DualSense (VIIPER) |
|-----------|--------------------|
| BUTTON_A / B / X / Y | Croce / Cerchio / Quadrato / Triangolo |
| BUTTON_SHOULDER_L / R | L1 / R1 |
| BUTTON_TRIGGER_L / R | L2 / R2 digitali; analogici da `lt`/`rt` >> 2 |
| BUTTON_THUMB_L / R | L3 / R3 |
| MISC_BUTTON_SYSTEM / SELECT / START / CAPTURE | PS / Create / Options / microfono |
| `ext_buttons` bit0 | clic del touchpad |
