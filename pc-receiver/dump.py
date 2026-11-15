"""Stampa in forma leggibile i pacchetti INPUT ricevuti dal dongle.

Avvio: `python dump.py` (porta da config.toml). Non usa VIIPER: serve a verificare
che i pacchetti arrivino al PC. Non può girare insieme a receiver.py (stessa porta).
"""

from __future__ import annotations

import argparse
import signal
import socket
import sys
import time

import mapping
import protocol
from config import load_config

_BUTTON_NAMES = (
    (mapping.BUTTON_A, "A"),
    (mapping.BUTTON_B, "B"),
    (mapping.BUTTON_X, "X"),
    (mapping.BUTTON_Y, "Y"),
    (mapping.BUTTON_SHOULDER_L, "LB"),
    (mapping.BUTTON_SHOULDER_R, "RB"),
    (mapping.BUTTON_TRIGGER_L, "LT"),
    (mapping.BUTTON_TRIGGER_R, "RT"),
    (mapping.BUTTON_THUMB_L, "LS"),
    (mapping.BUTTON_THUMB_R, "RS"),
)
_DPAD_NAMES = (
    (mapping.DPAD_UP, "Su"),
    (mapping.DPAD_DOWN, "Giu"),
    (mapping.DPAD_LEFT, "Sx"),
    (mapping.DPAD_RIGHT, "Dx"),
)
_MISC_NAMES = (
    (mapping.MISC_BUTTON_SYSTEM, "Xbox"),
    (mapping.MISC_BUTTON_SELECT, "View"),
    (mapping.MISC_BUTTON_START, "Menu"),
    (mapping.MISC_BUTTON_CAPTURE, "Share"),
)


def _names(mask: int, table: tuple[tuple[int, str], ...]) -> str:
    return "+".join(name for bit, name in table if mask & bit) or "-"


_KIND_NAMES = {
    protocol.KIND_NONE: "-",
    protocol.KIND_XBOX: "Xbox",
    protocol.KIND_DUALSENSE: "DualSense",
    protocol.KIND_DUALSHOCK4: "DualShock4",
    protocol.KIND_OTHER: "altro",
}
_POWER_NAMES = {
    protocol.POWER_DISCHARGING: "in scarica",
    protocol.POWER_CHARGING: "in carica",
    protocol.POWER_FULL: "carico",
}


def format_input(pkt: protocol.InputPacket) -> str:
    """Una riga leggibile per un pacchetto INPUT (v1 o v2)."""
    line = (
        f"v{pkt.version} seq={pkt.seq:5d} pad={pkt.pad_idx} {'ON ' if pkt.connected else 'OFF'} "
        f"L=({pkt.lx:5d},{pkt.ly:5d}) R=({pkt.rx:5d},{pkt.ry:5d}) "
        f"LT={pkt.lt:4d} RT={pkt.rt:4d} "
        f"tasti={_names(pkt.buttons, _BUTTON_NAMES)} "
        f"dpad={_names(pkt.dpad, _DPAD_NAMES)} "
        f"misc={_names(pkt.misc, _MISC_NAMES)}"
    )
    if pkt.version < protocol.VERSION2 or not pkt.connected:
        return line
    battery = "?" if pkt.battery == protocol.BATTERY_UNKNOWN else f"{pkt.battery}%"
    power = _POWER_NAMES.get(pkt.power, "")
    line += f" | {_KIND_NAMES.get(pkt.kind, pkt.kind)} batt={battery} {power}".rstrip()
    if pkt.has_touch:
        fingers = [f"({x},{y})" for x, y, active in pkt.touch if active]
        line += f" touch={'+'.join(fingers) or '-'}{' CLIC' if pkt.touchpad_click else ''}"
    if pkt.has_motion:
        dps = tuple(round(v / protocol.GYRO_LSB_PER_DPS) for v in pkt.gyro)
        g = tuple(round(v / protocol.ACCEL_LSB_PER_G, 2) for v in pkt.accel)
        line += f" gyro={dps}°/s accel={g}g"
    return line


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Stampa i pacchetti INPUT del dongle")
    parser.add_argument("-c", "--config", help="file di configurazione (default: config.toml)")
    parser.add_argument("-p", "--port", type=int, help="porta UDP (default: quella di config.toml)")
    parser.add_argument(
        "-a", "--all", action="store_true", help="stampa anche i pacchetti identici al precedente"
    )
    args = parser.parse_args(argv)

    try:
        cfg = load_config(args.config)
    except (OSError, ValueError) as exc:
        print(f"Configurazione non valida: {exc}", file=sys.stderr)
        return 2
    port = args.port or cfg.port

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind((cfg.bind_address, port))
    except OSError as exc:
        print(f"Impossibile aprire la porta UDP {port}: {exc}", file=sys.stderr)
        return 1
    sock.settimeout(0.5)  # così Ctrl+C viene servito anche senza traffico
    print(f"In ascolto su UDP {cfg.bind_address}:{port} (Ctrl+C per uscire)")

    # Ctrl+C e Ctrl+Break alzano un flag: il ciclo esce al giro successivo.
    stop: list[bool] = []
    signal.signal(signal.SIGINT, lambda *_: stop.append(True))
    if hasattr(signal, "SIGBREAK"):
        signal.signal(signal.SIGBREAK, lambda *_: stop.append(True))

    last_state: dict[tuple[str, int], tuple] = {}
    counts = {"ricevuti": 0, "non validi": 0}
    try:
        while not stop:
            try:
                data, addr = sock.recvfrom(2048)
            except (socket.timeout, ConnectionResetError):
                continue
            counts["ricevuti"] += 1
            stamp = time.strftime("%H:%M:%S")
            try:
                pkt = protocol.parse_input(data)
            except protocol.ProtocolError as exc:
                counts["non validi"] += 1
                print(f"{stamp} {addr[0]} NON VALIDO ({exc}): {data.hex(' ')}")
                continue
            # Il keepalive ripete lo stesso stato ogni 100 ms: si stampa solo quando
            # cambia. Sensori e posizione delle dita cambiano di continuo: contano
            # solo con --all.
            state = (pkt.flags, pkt.buttons, pkt.dpad, pkt.misc, pkt.ext_buttons,
                     pkt.lx, pkt.ly, pkt.rx, pkt.ry, pkt.lt, pkt.rt,
                     pkt.kind, pkt.battery, pkt.power,
                     tuple(active for _, _, active in pkt.touch))  # fmt: skip
            key = (addr[0], pkt.pad_idx)
            if args.all or last_state.get(key) != state:
                last_state[key] = state
                print(f"{stamp} {addr[0]} {format_input(pkt)}")
    finally:
        sock.close()
    print(f"Pacchetti ricevuti: {counts['ricevuti']}, non validi: {counts['non validi']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
