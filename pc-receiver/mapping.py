"""Conversione dallo stato Bluepad32 ai report di VIIPER (Xbox 360 e DualSense).

Maschere Bluepad32 verificate su uni_gamepad.h / uni_common.h (branch main,
2026-11-14): ogni costante è BIT(n) = 1 << n, nell'ordine degli enum.
Maschere e layout VIIPER da docs/devices/xbox360.md e, per il DualSense, da
device/dualsense/{state,const,device}.go del tag v0.9.0.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

import protocol
from protocol import InputPacket

# Tipi di dispositivo di VIIPER usati dal ricevitore.
DEVICE_XBOX360 = "xbox360"
DEVICE_DUALSENSE = "dualsense"

# --- Bluepad32 -------------------------------------------------------------
# Nel D-pad RIGHT viene prima di LEFT (al contrario di XInput).
DPAD_UP = 0x01
DPAD_DOWN = 0x02
DPAD_RIGHT = 0x04
DPAD_LEFT = 0x08

BUTTON_A = 0x0001
BUTTON_B = 0x0002
BUTTON_X = 0x0004
BUTTON_Y = 0x0008
BUTTON_SHOULDER_L = 0x0010
BUTTON_SHOULDER_R = 0x0020
BUTTON_TRIGGER_L = 0x0040
BUTTON_TRIGGER_R = 0x0080
BUTTON_THUMB_L = 0x0100
BUTTON_THUMB_R = 0x0200

MISC_BUTTON_SYSTEM = 0x01  # tasto Xbox
MISC_BUTTON_SELECT = 0x02  # View / Back
MISC_BUTTON_START = 0x04  # Menu / Start
MISC_BUTTON_CAPTURE = 0x08  # Share: nessun equivalente sul pad 360

# --- Xbox 360 (VIIPER) -----------------------------------------------------
X360_DPAD_UP = 0x0001
X360_DPAD_DOWN = 0x0002
X360_DPAD_LEFT = 0x0004
X360_DPAD_RIGHT = 0x0008
X360_START = 0x0010
X360_BACK = 0x0020
X360_LEFT_THUMB = 0x0040
X360_RIGHT_THUMB = 0x0080
X360_LEFT_SHOULDER = 0x0100
X360_RIGHT_SHOULDER = 0x0200
X360_GUIDE = 0x0400
X360_A = 0x1000
X360_B = 0x2000
X360_X = 0x4000
X360_Y = 0x8000

# buttons u32, LT u8, RT u8, LX LY RX RY i16, 6 byte riservati a zero
_REPORT = struct.Struct("<IBBhhhh6x")
REPORT_SIZE = _REPORT.size
RUMBLE_SIZE = 2  # motore sinistro u8, motore destro u8

assert REPORT_SIZE == 20

NEUTRAL_REPORT = bytes(REPORT_SIZE)

_BUTTON_MAP = (
    (BUTTON_A, X360_A),
    (BUTTON_B, X360_B),
    (BUTTON_X, X360_X),
    (BUTTON_Y, X360_Y),
    (BUTTON_SHOULDER_L, X360_LEFT_SHOULDER),
    (BUTTON_SHOULDER_R, X360_RIGHT_SHOULDER),
    (BUTTON_THUMB_L, X360_LEFT_THUMB),
    (BUTTON_THUMB_R, X360_RIGHT_THUMB),
)
_DPAD_MAP = (
    (DPAD_UP, X360_DPAD_UP),
    (DPAD_DOWN, X360_DPAD_DOWN),
    (DPAD_LEFT, X360_DPAD_LEFT),
    (DPAD_RIGHT, X360_DPAD_RIGHT),
)
_MISC_MAP = (
    (MISC_BUTTON_SYSTEM, X360_GUIDE),
    (MISC_BUTTON_SELECT, X360_BACK),
    (MISC_BUTTON_START, X360_START),
)


def map_buttons(buttons: int, dpad: int, misc: int) -> int:
    """Maschera pulsanti Xbox 360 a partire dalle tre maschere di Bluepad32."""
    out = 0
    for source, table in ((buttons, _BUTTON_MAP), (dpad, _DPAD_MAP), (misc, _MISC_MAP)):
        for src_bit, dst_bit in table:
            if source & src_bit:
                out |= dst_bit
    return out


def scale_stick(value: int, invert: bool = False) -> int:
    """Da -512..511 a -32768..32767: valore × 64 con clamp."""
    scaled = value * 64
    if invert:
        scaled = -scaled
    return max(-32768, min(32767, scaled))


def scale_trigger(value: int) -> int:
    """Da 0..1023 a 0..255: valore >> 2 con clamp."""
    return max(0, min(255, value >> 2))


def to_xbox360_report(pkt: InputPacket, invert_y: bool = True) -> bytes:
    """Report di 20 byte per lo stream Xbox 360 di VIIPER."""
    lt = scale_trigger(pkt.lt)
    rt = scale_trigger(pkt.rt)
    # Ripiego per i controller che danno il grilletto solo come pulsante.
    if lt == 0 and pkt.buttons & BUTTON_TRIGGER_L:
        lt = 255
    if rt == 0 and pkt.buttons & BUTTON_TRIGGER_R:
        rt = 255
    return _REPORT.pack(
        map_buttons(pkt.buttons, pkt.dpad, pkt.misc),
        lt,
        rt,
        scale_stick(pkt.lx),
        scale_stick(pkt.ly, invert_y),
        scale_stick(pkt.rx),
        scale_stick(pkt.ry, invert_y),
    )


def rumble_from_viiper(left: int, right: int) -> tuple[int, int]:
    """Da (sinistro, destro) di VIIPER a (weak, strong) del pacchetto RUMBLE.

    Il motore sinistro è quello grande a bassa frequenza (strong), il destro
    quello piccolo ad alta frequenza (weak).
    """
    return right, left


# --- DualSense (VIIPER) ----------------------------------------------------
# Stream: InputState di 33 byte verso VIIPER, OutputState di 6 byte in ritorno.
DS_SQUARE = 0x00000010
DS_CROSS = 0x00000020
DS_CIRCLE = 0x00000040
DS_TRIANGLE = 0x00000080
DS_L1 = 0x00000100
DS_R1 = 0x00000200
DS_L2 = 0x00000400
DS_R2 = 0x00000800
DS_CREATE = 0x00001000
DS_OPTIONS = 0x00002000
DS_L3 = 0x00004000
DS_R3 = 0x00008000
DS_PS = 0x00010000
DS_TOUCHPAD = 0x00020000
DS_MIC_MUTE = 0x00040000

DS_DPAD_UP = 0x01
DS_DPAD_DOWN = 0x02
DS_DPAD_LEFT = 0x04
DS_DPAD_RIGHT = 0x08

DS_TOUCH_MAX_X = 1920
DS_TOUCH_MAX_Y = 1080
# Accelerometro a riposo come lo crea VIIPER (controller in piano, -1 g su Z).
DS_DEFAULT_ACCEL = (0, 0, -8192)

# LX LY RX RY i8, buttons u32, dpad u8, L2 u8, R2 u8,
# touch1 x u16 y u16 active u8, touch2 x u16 y u16 active u8, gyro 3×i16, accel 3×i16
_DS_STATE = struct.Struct("<4bIBBBHHBHHB3h3h")
DS_STATE_SIZE = _DS_STATE.size
DS_FEEDBACK_SIZE = 6  # rumbleSmall, rumbleLarge, R, G, B, playerLeds

assert DS_STATE_SIZE == 33

_DS_BUTTON_MAP = (
    (BUTTON_A, DS_CROSS),
    (BUTTON_B, DS_CIRCLE),
    (BUTTON_X, DS_SQUARE),
    (BUTTON_Y, DS_TRIANGLE),
    (BUTTON_SHOULDER_L, DS_L1),
    (BUTTON_SHOULDER_R, DS_R1),
    (BUTTON_TRIGGER_L, DS_L2),
    (BUTTON_TRIGGER_R, DS_R2),
    (BUTTON_THUMB_L, DS_L3),
    (BUTTON_THUMB_R, DS_R3),
)
_DS_MISC_MAP = (
    (MISC_BUTTON_SYSTEM, DS_PS),
    (MISC_BUTTON_SELECT, DS_CREATE),
    (MISC_BUTTON_START, DS_OPTIONS),
    (MISC_BUTTON_CAPTURE, DS_MIC_MUTE),  # sul DualSense Bluepad32 vi mette il tasto microfono
)
_DS_DPAD_MAP = (
    (DPAD_UP, DS_DPAD_UP),
    (DPAD_DOWN, DS_DPAD_DOWN),
    (DPAD_LEFT, DS_DPAD_LEFT),
    (DPAD_RIGHT, DS_DPAD_RIGHT),
)


def _ds_stick(value: int) -> int:
    """Da -512..511 a -128..127. Il verso resta quello di Bluepad32 e del
    DualSense ("su" negativo): niente inversione dell'asse Y."""
    return max(-128, min(127, value >> 2))


def _ds_touch(point: tuple[int, int, bool]) -> tuple[int, int, int]:
    x, y, active = point
    return min(x, DS_TOUCH_MAX_X), min(y, DS_TOUCH_MAX_Y), 1 if active else 0


def to_dualsense_state(pkt: InputPacket) -> bytes:
    """InputState di 33 byte per lo stream DualSense di VIIPER."""
    buttons = 0
    for source, table in ((pkt.buttons, _DS_BUTTON_MAP), (pkt.misc, _DS_MISC_MAP)):
        for src_bit, dst_bit in table:
            if source & src_bit:
                buttons |= dst_bit
    if pkt.touchpad_click:
        buttons |= DS_TOUCHPAD
    dpad = 0
    for src_bit, dst_bit in _DS_DPAD_MAP:
        if pkt.dpad & src_bit:
            dpad |= dst_bit
    lt = scale_trigger(pkt.lt)
    rt = scale_trigger(pkt.rt)
    if lt == 0 and pkt.buttons & BUTTON_TRIGGER_L:
        lt = 255
    if rt == 0 and pkt.buttons & BUTTON_TRIGGER_R:
        rt = 255
    if pkt.has_touch:
        t1, t2 = _ds_touch(pkt.touch[0]), _ds_touch(pkt.touch[1])
    else:
        t1 = t2 = (0, 0, 0)
    gyro = pkt.gyro if pkt.has_motion else (0, 0, 0)
    accel = pkt.accel if pkt.has_motion else DS_DEFAULT_ACCEL
    return _DS_STATE.pack(
        _ds_stick(pkt.lx), _ds_stick(pkt.ly), _ds_stick(pkt.rx), _ds_stick(pkt.ry),
        buttons, dpad, lt, rt, *t1, *t2, *gyro, *accel,
    )  # fmt: skip


DS_NEUTRAL_STATE = _DS_STATE.pack(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, *DS_DEFAULT_ACCEL)


def battery_status_byte(pkt: InputPacket) -> int | None:
    """Byte di stato batteria del report USB del DualSense (MetaState.battery_status
    di VIIPER): bit 0..3 livello 0..10, bit 4..7 0 = in scarica, 1 = in carica,
    2 = carica. None se il controller non l'ha comunicato."""
    if pkt.battery == protocol.BATTERY_UNKNOWN or pkt.battery > 100:
        return None
    level = min(10, (pkt.battery + 5) // 10)
    state = {protocol.POWER_CHARGING: 0x1, protocol.POWER_FULL: 0x2}.get(pkt.power, 0x0)
    if state == 0x2:
        level = 10
    return (state << 4) | level


@dataclass(frozen=True)
class Feedback:
    """Ritorno dal gioco: vibrazione e, per il DualSense, barra luminosa e LED giocatore."""

    weak: int = 0
    strong: int = 0
    lightbar: tuple[int, int, int] | None = None  # None = il gioco non l'ha impostata
    player_leds: int | None = None

    @classmethod
    def from_xbox360(cls, data: bytes) -> "Feedback":
        weak, strong = rumble_from_viiper(data[0], data[1])
        return cls(weak, strong)

    @classmethod
    def from_dualsense(cls, data: bytes) -> "Feedback":
        # VIIPER 0.9.0 (parseOutputReport) mette a zero colore e LED quando il
        # gioco manda un report che non li tocca: lo zero vale "non impostato".
        # Conseguenza: un gioco non può spegnere del tutto la barra o i LED.
        small, large, r, g, b, leds = data[:DS_FEEDBACK_SIZE]
        return cls(
            weak=small,
            strong=large,
            lightbar=(r, g, b) if (r or g or b) else None,
            player_leds=leds if leds else None,
        )


# Dimensioni degli stream di VIIPER per tipo di dispositivo:
# (report verso VIIPER, pacchetto di ritorno, report a riposo, lettura del ritorno).
DEVICE_FORMATS = {
    DEVICE_XBOX360: (REPORT_SIZE, RUMBLE_SIZE, NEUTRAL_REPORT, Feedback.from_xbox360),
    DEVICE_DUALSENSE: (DS_STATE_SIZE, DS_FEEDBACK_SIZE, DS_NEUTRAL_STATE, Feedback.from_dualsense),
}


def device_type_for(pkt: InputPacket, dualsense_device: str) -> str:
    """Dispositivo virtuale da creare per questo controller."""
    if pkt.kind == protocol.KIND_DUALSENSE and dualsense_device == DEVICE_DUALSENSE:
        return DEVICE_DUALSENSE
    return DEVICE_XBOX360


def to_report(device_type: str, pkt: InputPacket, invert_y: bool = True) -> bytes:
    if device_type == DEVICE_DUALSENSE:
        return to_dualsense_state(pkt)
    return to_xbox360_report(pkt, invert_y)
