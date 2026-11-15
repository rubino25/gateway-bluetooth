"""Protocollo UDP tra dongle ESP32 e PC (little-endian), versioni 1 e 2.

Specifica in docs/protocol.md; deve restare allineato a firmware/main/protocol.h.

- v1: INPUT 24 byte, RUMBLE 12 byte.
- v2: INPUT 52 byte (i primi 24 come la v1, più tipo di controller, batteria,
  touchpad e sensori di movimento), FEEDBACK 20 byte (i primi 12 come il RUMBLE
  v1, più barra luminosa e LED giocatore). Stesso tipo 0x02 del RUMBLE.

Il firmware attuale invia INPUT v2 e accetta sia RUMBLE v1 sia FEEDBACK v2; il
ricevitore risponde nella versione dell'ultimo INPUT ricevuto.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

MAGIC = 0x4758  # "XG": sul filo i byte sono 0x58 0x47
VERSION = 1
VERSION2 = 2
VERSIONS = (VERSION, VERSION2)
TYPE_INPUT = 0x01
TYPE_RUMBLE = 0x02  # in v2 si chiama FEEDBACK, stesso tipo

FLAG_CONNECTED = 0x01
FLAG_MOTION = 0x02  # v2: gyro e accel validi
FLAG_TOUCH = 0x04  # v2: touchpad valido

# v2: tipo di controller
KIND_NONE = 0
KIND_XBOX = 1
KIND_DUALSENSE = 2
KIND_DUALSHOCK4 = 3
KIND_OTHER = 4

# v2: stato di carica
POWER_UNKNOWN = 0
POWER_DISCHARGING = 1
POWER_CHARGING = 2
POWER_FULL = 3

BATTERY_UNKNOWN = 0xFF

EXT_TOUCHPAD = 0x01  # v2: clic del touchpad
TOUCH1_ACTIVE = 0x01
TOUCH2_ACTIVE = 0x02

FB_LIGHTBAR = 0x01  # v2: led_r/g/b validi
FB_PLAYER_LEDS = 0x02  # v2: player_leds valido

MAX_PADS = 4
DEFAULT_PORT = 47900

# Unità dei sensori v2 (come il report USB del DualSense e VIIPER).
GYRO_LSB_PER_DPS = 16.384
ACCEL_LSB_PER_G = 8192

# magic, version, type
_HEADER = struct.Struct("<HBB")
# header + seq, pad_idx, flags, buttons, dpad, misc, lx, ly, rx, ry, lt, rt
_INPUT = struct.Struct("<HBBHBBHBB4h2H")
# v2: kind, battery, power, ext_buttons, t1x, t1y, t2x, t2y, touch_active, riservato,
#     gyro x3, accel x3, riservato
_INPUT_EXT = struct.Struct("<BBBB4HBx3h3hH")
# header + pad_idx, reserved, duration_ms, weak, strong, reserved
_RUMBLE = struct.Struct("<HBBBBHBBH")
# v2: flags, r, g, b, player_leds, 3 riservati
_FEEDBACK_EXT = struct.Struct("<BBBBB3x")

HEADER_SIZE = _HEADER.size
INPUT_SIZE = _INPUT.size
INPUT_V2_SIZE = _INPUT.size + _INPUT_EXT.size
RUMBLE_SIZE = _RUMBLE.size
FEEDBACK_V2_SIZE = _RUMBLE.size + _FEEDBACK_EXT.size

assert HEADER_SIZE == 4 and INPUT_SIZE == 24 and RUMBLE_SIZE == 12
assert INPUT_V2_SIZE == 52 and FEEDBACK_V2_SIZE == 20


class ProtocolError(ValueError):
    """Pacchetto non valido: magic, versione, tipo, lunghezza o campi fuori range."""


@dataclass(frozen=True)
class InputPacket:
    """Stato del controller, valori nei range di Bluepad32.

    I campi dopo `rt` esistono solo in v2; in un pacchetto v1 restano ai valori
    di default.
    """

    seq: int = 0
    pad_idx: int = 0
    flags: int = FLAG_CONNECTED
    buttons: int = 0  # maschera BUTTON_*
    dpad: int = 0  # maschera DPAD_*
    misc: int = 0  # maschera MISC_BUTTON_*
    lx: int = 0  # -512..511
    ly: int = 0
    rx: int = 0
    ry: int = 0
    lt: int = 0  # 0..1023
    rt: int = 0
    # --- v2 ---
    version: int = VERSION
    kind: int = KIND_NONE
    battery: int = BATTERY_UNKNOWN  # 0..100
    power: int = POWER_UNKNOWN
    ext_buttons: int = 0
    touch: tuple[tuple[int, int, bool], tuple[int, int, bool]] = ((0, 0, False), (0, 0, False))
    gyro: tuple[int, int, int] = (0, 0, 0)  # 1 LSB = 1/16,384 °/s
    accel: tuple[int, int, int] = (0, 0, 0)  # 1 LSB = 1/8192 g

    @property
    def connected(self) -> bool:
        return bool(self.flags & FLAG_CONNECTED)

    @property
    def has_motion(self) -> bool:
        return bool(self.flags & FLAG_MOTION)

    @property
    def has_touch(self) -> bool:
        return bool(self.flags & FLAG_TOUCH)

    @property
    def touchpad_click(self) -> bool:
        return bool(self.ext_buttons & EXT_TOUCHPAD)


@dataclass(frozen=True)
class RumblePacket:
    """RUMBLE (v1) o FEEDBACK (v2). Barra luminosa e LED giocatore solo in v2."""

    pad_idx: int = 0
    duration_ms: int = 0
    weak: int = 0  # 0..255, motore piccolo (alta frequenza)
    strong: int = 0  # 0..255, motore grande (bassa frequenza)
    # --- v2 ---
    version: int = VERSION
    lightbar: tuple[int, int, int] | None = None  # None = non inviare
    player_leds: int | None = None  # maschera dei 5 LED del DualSense, None = non inviare


def parse_header(data: bytes) -> int:
    """Controlla magic e versione e restituisce il tipo del pacchetto."""
    if len(data) < HEADER_SIZE:
        raise ProtocolError(f"pacchetto troppo corto: {len(data)} byte")
    magic, version, ptype = _HEADER.unpack_from(data)
    if magic != MAGIC:
        raise ProtocolError(f"magic errato: 0x{magic:04X}")
    if version not in VERSIONS:
        raise ProtocolError(f"versione non supportata: {version}")
    if ptype not in (TYPE_INPUT, TYPE_RUMBLE):
        raise ProtocolError(f"tipo sconosciuto: 0x{ptype:02X}")
    return ptype


def packet_version(data: bytes) -> int:
    parse_header(data)
    return data[2]


def _check(data: bytes, expected_type: int, sizes: dict[int, int], name: str) -> int:
    ptype = parse_header(data)
    if ptype != expected_type:
        raise ProtocolError(f"tipo 0x{ptype:02X}, atteso {name}")
    version = data[2]
    expected_size = sizes[version]
    if len(data) != expected_size:
        raise ProtocolError(f"{name} v{version} di {len(data)} byte, attesi {expected_size}")
    return version


def parse_input(data: bytes) -> InputPacket:
    version = _check(data, TYPE_INPUT, {VERSION: INPUT_SIZE, VERSION2: INPUT_V2_SIZE}, "INPUT")
    (_, _, _, seq, pad_idx, flags, buttons, dpad, misc, lx, ly, rx, ry, lt, rt) = _INPUT.unpack_from(data)
    if pad_idx >= MAX_PADS:
        raise ProtocolError(f"pad_idx fuori range: {pad_idx}")
    if version == VERSION:
        return InputPacket(seq, pad_idx, flags, buttons, dpad, misc, lx, ly, rx, ry, lt, rt)
    (kind, battery, power, ext, t1x, t1y, t2x, t2y, active,
     gx, gy, gz, ax, ay, az, _) = _INPUT_EXT.unpack_from(data, INPUT_SIZE)  # fmt: skip
    return InputPacket(
        seq, pad_idx, flags, buttons, dpad, misc, lx, ly, rx, ry, lt, rt,
        version=VERSION2,
        kind=kind,
        battery=battery,
        power=power,
        ext_buttons=ext,
        touch=((t1x, t1y, bool(active & TOUCH1_ACTIVE)), (t2x, t2y, bool(active & TOUCH2_ACTIVE))),
        gyro=(gx, gy, gz),
        accel=(ax, ay, az),
    )  # fmt: skip


def build_input(pkt: InputPacket) -> bytes:
    try:
        base = _INPUT.pack(
            MAGIC, pkt.version, TYPE_INPUT,
            pkt.seq & 0xFFFF, pkt.pad_idx, pkt.flags, pkt.buttons, pkt.dpad, pkt.misc,
            pkt.lx, pkt.ly, pkt.rx, pkt.ry, pkt.lt, pkt.rt,
        )  # fmt: skip
        if pkt.version == VERSION:
            return base
        if pkt.version != VERSION2:
            raise ProtocolError(f"versione non supportata: {pkt.version}")
        (t1x, t1y, t1a), (t2x, t2y, t2a) = pkt.touch
        active = (TOUCH1_ACTIVE if t1a else 0) | (TOUCH2_ACTIVE if t2a else 0)
        return base + _INPUT_EXT.pack(
            pkt.kind, pkt.battery, pkt.power, pkt.ext_buttons,
            t1x, t1y, t2x, t2y, active, *pkt.gyro, *pkt.accel, 0,
        )  # fmt: skip
    except struct.error as exc:
        raise ProtocolError(f"campo INPUT fuori range: {exc}") from exc


def parse_rumble(data: bytes) -> RumblePacket:
    """Legge un RUMBLE v1 o un FEEDBACK v2."""
    version = _check(data, TYPE_RUMBLE, {VERSION: RUMBLE_SIZE, VERSION2: FEEDBACK_V2_SIZE}, "RUMBLE")
    (_, _, _, pad_idx, _, duration_ms, weak, strong, _) = _RUMBLE.unpack_from(data)
    if version == VERSION:
        return RumblePacket(pad_idx, duration_ms, weak, strong)
    flags, r, g, b, leds = _FEEDBACK_EXT.unpack_from(data, RUMBLE_SIZE)
    return RumblePacket(
        pad_idx, duration_ms, weak, strong,
        version=VERSION2,
        lightbar=(r, g, b) if flags & FB_LIGHTBAR else None,
        player_leds=leds if flags & FB_PLAYER_LEDS else None,
    )  # fmt: skip


def build_rumble(pkt: RumblePacket) -> bytes:
    """Costruisce un RUMBLE v1 o un FEEDBACK v2 secondo `pkt.version`."""
    try:
        base = _RUMBLE.pack(
            MAGIC, pkt.version, TYPE_RUMBLE, pkt.pad_idx, 0, pkt.duration_ms, pkt.weak, pkt.strong, 0
        )
        if pkt.version == VERSION:
            return base
        if pkt.version != VERSION2:
            raise ProtocolError(f"versione non supportata: {pkt.version}")
        flags = (FB_LIGHTBAR if pkt.lightbar is not None else 0) | (
            FB_PLAYER_LEDS if pkt.player_leds is not None else 0
        )
        r, g, b = pkt.lightbar if pkt.lightbar is not None else (0, 0, 0)
        leds = pkt.player_leds if pkt.player_leds is not None else 0
        return base + _FEEDBACK_EXT.pack(flags, r, g, b, leds)
    except struct.error as exc:
        raise ProtocolError(f"campo RUMBLE fuori range: {exc}") from exc
