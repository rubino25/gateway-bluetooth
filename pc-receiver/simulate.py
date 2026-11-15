"""Finto dongle: invia pacchetti INPUT di prova e stampa i RUMBLE/FEEDBACK che riceve.

Serve a provare receiver.py (o dump.py) senza hardware:
    python simulate.py              # 10 secondi verso [simulate].target di config.toml
    python simulate.py --seconds 0  # finché non si preme Ctrl+C
    python simulate.py --dualsense  # DualSense (protocollo v2): touchpad e sensori
    python simulate.py --v1         # vecchio formato v1, come il firmware precedente
"""

from __future__ import annotations

import argparse
import math
import signal
import socket
import sys
import time

import mapping
import protocol
from config import load_config

# Un pulsante alla volta, a rotazione: (buttons, dpad, misc)
_BUTTON_CYCLE = (
    (mapping.BUTTON_A, 0, 0),
    (mapping.BUTTON_B, 0, 0),
    (mapping.BUTTON_X, 0, 0),
    (mapping.BUTTON_Y, 0, 0),
    (mapping.BUTTON_SHOULDER_L, 0, 0),
    (mapping.BUTTON_SHOULDER_R, 0, 0),
    (mapping.BUTTON_THUMB_L, 0, 0),
    (mapping.BUTTON_THUMB_R, 0, 0),
    (0, mapping.DPAD_UP, 0),
    (0, mapping.DPAD_RIGHT, 0),
    (0, mapping.DPAD_DOWN, 0),
    (0, mapping.DPAD_LEFT, 0),
    (0, 0, mapping.MISC_BUTTON_SELECT),
    (0, 0, mapping.MISC_BUTTON_START),
)


def make_state(t: float, seq: int, pad_idx: int = 0, kind: int | None = None) -> protocol.InputPacket:
    """Stato di prova all'istante `t`: stick che ruotano, grilletti a rampa, tasti a turno.

    `kind` None = pacchetto v1; altrimenti v2 con quel tipo di controller. Il
    DualSense ha in più un dito che gira sul touchpad (clic ogni 4 s) e i sensori
    che oscillano.
    """
    angle = t * math.pi  # un giro ogni 2 secondi
    buttons, dpad, misc = _BUTTON_CYCLE[int(t * 2) % len(_BUTTON_CYCLE)]
    ramp = int(t * 512) % 1024
    base = dict(
        seq=seq & 0xFFFF,
        pad_idx=pad_idx,
        flags=protocol.FLAG_CONNECTED,
        buttons=buttons,
        dpad=dpad,
        misc=misc,
        lx=int(511 * math.cos(angle)),
        ly=int(511 * math.sin(angle)),
        rx=int(511 * math.cos(-angle)),
        ry=int(511 * math.sin(-angle)),
        lt=ramp,
        rt=1023 - ramp,
    )
    if kind is None:
        return protocol.InputPacket(**base)
    base.update(version=protocol.VERSION2, kind=kind, battery=80, power=protocol.POWER_DISCHARGING)
    if kind == protocol.KIND_DUALSENSE:
        base["flags"] |= protocol.FLAG_MOTION | protocol.FLAG_TOUCH
        base["touch"] = (
            (int(960 + 600 * math.cos(angle)), int(540 + 300 * math.sin(angle)), True),
            (0, 0, False),
        )
        base["ext_buttons"] = protocol.EXT_TOUCHPAD if int(t) % 4 == 3 else 0
        base["gyro"] = tuple(int(90 * protocol.GYRO_LSB_PER_DPS * math.sin(angle + k)) for k in range(3))
        base["accel"] = (0, protocol.ACCEL_LSB_PER_G, int(2000 * math.sin(angle)))
    return protocol.InputPacket(**base)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Finto dongle per provare il ricevitore")
    parser.add_argument("-c", "--config", help="file di configurazione (default: config.toml)")
    parser.add_argument("-t", "--target", help="indirizzo del ricevitore (default: da config.toml)")
    parser.add_argument("-p", "--port", type=int, help="porta UDP (default: da config.toml)")
    parser.add_argument("--pad", type=int, default=0, help="indice del controller, 0..3")
    parser.add_argument("--rate", type=float, default=100.0, help="pacchetti al secondo")
    parser.add_argument("--seconds", type=float, default=10.0, help="durata, 0 = senza fine")
    parser.add_argument("--seq-start", type=int, default=65000,
                        help="primo seq (default vicino al wrap, per provarlo)")  # fmt: skip
    parser.add_argument("--no-disconnect", action="store_true",
                        help="alla fine non inviare il pacchetto con connected=0")  # fmt: skip
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--dualsense", action="store_true",
                       help="simula un DualSense (protocollo v2, touchpad e sensori)")  # fmt: skip
    group.add_argument("--v1", action="store_true",
                       help="pacchetti v1 (firmware precedente) invece di v2 con un controller Xbox")  # fmt: skip
    args = parser.parse_args(argv)
    kind = None if args.v1 else (protocol.KIND_DUALSENSE if args.dualsense else protocol.KIND_XBOX)

    try:
        cfg = load_config(args.config)
    except (OSError, ValueError) as exc:
        print(f"Configurazione non valida: {exc}", file=sys.stderr)
        return 2
    if not 0 <= args.pad < protocol.MAX_PADS or args.rate <= 0:
        parser.error("--pad deve essere 0..3 e --rate maggiore di zero")
    target = (args.target or cfg.simulate_target, args.port or cfg.port)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setblocking(False)
    print(f"Invio INPUT a {target[0]}:{target[1]} a {args.rate:g} pacchetti/s (Ctrl+C per uscire)")

    # Ctrl+C e Ctrl+Break alzano un flag: il ciclo esce al giro successivo.
    stop: list[bool] = []
    signal.signal(signal.SIGINT, lambda *_: stop.append(True))
    if hasattr(signal, "SIGBREAK"):
        signal.signal(signal.SIGBREAK, lambda *_: stop.append(True))

    seq = args.seq_start
    sent = rumbles = 0
    last_lights = (None, None)
    period = 1.0 / args.rate
    start = time.monotonic()
    next_send = start
    try:
        while not stop:
            now = time.monotonic()
            elapsed = now - start
            if args.seconds > 0 and elapsed >= args.seconds:
                break
            if now >= next_send:
                sock.sendto(protocol.build_input(make_state(elapsed, seq, args.pad, kind)), target)
                seq += 1
                sent += 1
                next_send += period
            # Legge i RUMBLE arrivati nel frattempo.
            while True:
                try:
                    data, addr = sock.recvfrom(2048)
                except (BlockingIOError, ConnectionResetError):
                    break
                try:
                    rumble = protocol.parse_rumble(data)
                except protocol.ProtocolError as exc:
                    print(f"  pacchetto non valido da {addr[0]}: {exc}")
                    continue
                rumbles += 1
                lights = (rumble.lightbar, rumble.player_leds)
                if rumble.weak or rumble.strong or lights != last_lights:
                    last_lights = lights
                    extra = ""
                    if rumble.lightbar is not None:
                        extra += f" barra={rumble.lightbar}"
                    if rumble.player_leds is not None:
                        extra += f" LED=0x{rumble.player_leds:02X}"
                    print(f"  RUMBLE v{rumble.version} pad={rumble.pad_idx} weak={rumble.weak} "
                          f"strong={rumble.strong} durata={rumble.duration_ms} ms{extra}")  # fmt: skip
            time.sleep(max(0.0, min(period, next_send - time.monotonic())))
    finally:
        if not args.no_disconnect:
            # Come il firmware quando il controller si scollega.
            bye = protocol.InputPacket(seq=seq & 0xFFFF, pad_idx=args.pad, flags=0,
                                       version=protocol.VERSION if kind is None else protocol.VERSION2)  # fmt: skip
            sock.sendto(protocol.build_input(bye), target)
        sock.close()
    print(f"Inviati {sent} INPUT, ricevuti {rumbles} RUMBLE (compresi quelli a zero)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
