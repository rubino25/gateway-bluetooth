"""Finto server VIIPER, per i test e per provare il ricevitore senza driver installati.

Imita l'API descritta in docs/api/overview.md di VIIPER: richieste terminate da
`\\0`, una per connessione, risposta JSON e chiusura; stream binario del
dispositivo su una seconda connessione; rimozione del dispositivo se lo stream
non viene aperto (o riaperto) entro il timeout. NON crea alcun controller in
Windows: registra soltanto i report ricevuti.

Uso manuale:
    python fake_viiper.py                # porta 3242, stampa i report che cambiano
    python fake_viiper.py --rumble 2     # in più manda una vibrazione ogni 2 secondi
                                         # (ai DualSense anche barra luminosa e LED)

Tipi di dispositivo: "xbox360" (report 20 byte, ritorno 2) e "dualsense"
(report 33 byte, ritorno 6), come VIIPER 0.9.0.
"""

from __future__ import annotations

import argparse
import json
import re
import socket
import struct
import threading
import time

REPORT_SIZE = 20  # Xbox 360
# tipo -> (dimensione del report in ingresso, PID mostrato in `list`)
DEVICE_TYPES = {"xbox360": (20, "0x028e"), "dualsense": (33, "0x0ce6")}
_DEVICE_PATH = re.compile(r"^bus/(\d+)/(list|add|remove|\d+)$")


class FakeDevice:
    def __init__(self, bus_id: int, dev_id: str, deadline: float, dev_type: str = "xbox360",
                 device_specific: dict | None = None):  # fmt: skip
        self.bus_id = bus_id
        self.dev_id = dev_id
        self.dev_type = dev_type
        self.device_specific = device_specific or {}
        self.report_size = DEVICE_TYPES[dev_type][0]
        self.reports: list[bytes] = []
        self.stream: socket.socket | None = None
        self.stream_opens = 0
        self.deadline: float | None = deadline  # istante di rimozione se manca lo stream


class FakeViiperServer:
    def __init__(self, host: str = "127.0.0.1", port: int = 0, connect_timeout: float = 5.0,
                 on_report=None):  # fmt: skip
        self.connect_timeout = connect_timeout
        self.on_report = on_report  # callback(device, report) facoltativa
        self.buses: dict[int, dict[str, FakeDevice]] = {}
        self.requests: list[str] = []
        self.removed: list[tuple[int, str]] = []  # dispositivi rimossi (da API o per timeout)
        # Se impostato, `add` crea il dispositivo ma risponde 409 con questo testo e non
        # lo rimuove più, come VIIPER 0.9.0 quando manca usbip.exe.
        self.attach_error: str | None = None
        self.lock = threading.RLock()
        self._next_dev = 1
        self._running = False
        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener.bind((host, port))
        self._listener.listen(16)
        self._listener.settimeout(0.1)
        self.host = host
        self.port = self._listener.getsockname()[1]
        self._threads: list[threading.Thread] = []

    # --- ciclo di vita -----------------------------------------------------
    def start(self) -> "FakeViiperServer":
        self._running = True
        for target in (self._accept_loop, self._reaper_loop):
            thread = threading.Thread(target=target, daemon=True)
            thread.start()
            self._threads.append(thread)
        return self

    def stop(self) -> None:
        self._running = False
        for thread in self._threads:
            thread.join(timeout=2)
        self._listener.close()
        with self.lock:
            for dev in self.devices():
                self._close_stream(dev)

    def __enter__(self) -> "FakeViiperServer":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()

    # --- aiuti per i test --------------------------------------------------
    def devices(self) -> list[FakeDevice]:
        with self.lock:
            return [d for bus in self.buses.values() for d in bus.values()]

    def send_rumble(self, dev: FakeDevice, left: int, right: int) -> None:
        """Xbox 360: motore sinistro (grande), destro (piccolo)."""
        self.send_feedback(dev, bytes((left, right)))

    def send_feedback(self, dev: FakeDevice, data: bytes) -> None:
        """Pacchetto di ritorno grezzo (DualSense: piccolo, grande, R, G, B, LED)."""
        with self.lock:
            if dev.stream is not None:
                dev.stream.sendall(data)

    def drop_stream(self, dev: FakeDevice) -> None:
        """Chiude lo stream dal lato server, come farebbe un errore di rete."""
        with self.lock:
            self._close_stream(dev)
            dev.deadline = time.monotonic() + self.connect_timeout

    # --- interni -----------------------------------------------------------
    def _close_stream(self, dev: FakeDevice) -> None:
        if dev.stream is not None:
            try:
                dev.stream.close()
            except OSError:
                pass
            dev.stream = None

    def _accept_loop(self) -> None:
        while self._running:
            try:
                conn, _ = self._listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            threading.Thread(target=self._handle, args=(conn,), daemon=True).start()

    def _reaper_loop(self) -> None:
        while self._running:
            now = time.monotonic()
            with self.lock:
                for bus in self.buses.values():
                    for dev in list(bus.values()):
                        if dev.stream is None and dev.deadline is not None and now >= dev.deadline:
                            del bus[dev.dev_id]
                            self.removed.append((dev.bus_id, dev.dev_id))
            time.sleep(0.02)

    def _handle(self, conn: socket.socket) -> None:
        conn.settimeout(2.0)
        raw = bytearray()
        try:
            while b"\0" not in raw:
                chunk = conn.recv(1024)
                if not chunk:
                    conn.close()
                    return
                raw += chunk
        except OSError:
            conn.close()
            return
        line, _, rest = bytes(raw).partition(b"\0")
        text = line.decode("utf-8", "replace")
        path, _, payload = text.partition(" ")
        payload = payload.strip()
        with self.lock:
            self.requests.append(text)

        match = _DEVICE_PATH.match(path)
        if match and match.group(2).isdigit() and not payload:
            self._stream(conn, int(match.group(1)), match.group(2), rest)
            return
        status, body = self._api(path, payload, match)
        if status != 200:
            body = {"status": status, "title": body, "detail": body}
        try:
            conn.sendall(json.dumps(body).encode() + b"\n")
        except OSError:
            pass
        conn.close()

    def _api(self, path: str, payload: str, match) -> tuple[int, object]:
        with self.lock:
            if path == "ping":
                return 200, {"server": "VIIPER", "version": "fake"}
            if path == "bus/list":
                return 200, {"buses": sorted(self.buses)}
            if path == "bus/create":
                if payload and not payload.isdigit():
                    return 400, "invalid bus id"
                bus_id = int(payload) if payload else max(self.buses, default=0) + 1
                if bus_id in self.buses:
                    return 409, "bus already exists"
                self.buses[bus_id] = {}
                return 200, {"busId": bus_id}
            if path == "bus/remove":
                if not payload.isdigit():
                    return 400, "missing payload"
                bus = self.buses.pop(int(payload), None)
                if bus is None:
                    return 404, "bus not found"
                for dev in bus.values():
                    self._close_stream(dev)
                    self.removed.append((dev.bus_id, dev.dev_id))
                return 200, {"busId": int(payload)}
            if not match:
                return 404, "unknown path"
            bus_id, action = int(match.group(1)), match.group(2)
            bus = self.buses.get(bus_id)
            if bus is None:
                return 404, "bus not found"
            if action == "list":
                return 200, {"devices": [self._describe(d) for d in bus.values()]}
            if action == "add":
                try:
                    spec = json.loads(payload)
                    dev_type = spec["type"]
                    device_specific = spec.get("deviceSpecific") or {}
                except (ValueError, KeyError, TypeError, AttributeError):
                    return 400, "invalid JSON payload"
                if dev_type not in DEVICE_TYPES:
                    return 400, "unsupported device type"
                dev = FakeDevice(bus_id, str(self._next_dev), time.monotonic() + self.connect_timeout,
                                 dev_type, device_specific)  # fmt: skip
                self._next_dev += 1
                bus[dev.dev_id] = dev
                if self.attach_error:
                    dev.deadline = None
                    return 409, f"Failed to auto-attach device: {self.attach_error}"
                return 200, self._describe(dev)
            if action == "remove":
                dev = bus.pop(payload, None)
                if dev is None:
                    return 404, "device not found"
                self._close_stream(dev)
                self.removed.append((bus_id, dev.dev_id))
                return 200, {"busId": bus_id, "devId": dev.dev_id}
            return 404, "unknown path"

    @staticmethod
    def _describe(dev: FakeDevice) -> dict:
        vid = "0x054c" if dev.dev_type == "dualsense" else "0x045e"
        return {"busId": dev.bus_id, "devId": dev.dev_id, "vid": vid,
                "pid": DEVICE_TYPES[dev.dev_type][1], "type": dev.dev_type,
                "deviceSpecific": dev.device_specific}  # fmt: skip

    def _stream(self, conn: socket.socket, bus_id: int, dev_id: str, rest: bytes) -> None:
        with self.lock:
            dev = self.buses.get(bus_id, {}).get(dev_id)
            if dev is None:
                conn.close()
                return
            self._close_stream(dev)
            dev.stream = conn
            dev.stream_opens += 1
            dev.deadline = None
        conn.settimeout(0.1)
        buf = bytearray(rest)
        while self._running:
            while len(buf) >= dev.report_size:
                report, buf = bytes(buf[:dev.report_size]), buf[dev.report_size:]
                with self.lock:
                    dev.reports.append(report)
                if self.on_report:
                    self.on_report(dev, report)
            try:
                chunk = conn.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
        with self.lock:
            if dev.stream is conn:
                self._close_stream(dev)
                dev.deadline = time.monotonic() + self.connect_timeout


def decode_report(report: bytes) -> str:
    if len(report) == DEVICE_TYPES["dualsense"][0]:
        (lx, ly, rx, ry, buttons, dpad, l2, r2, t1x, t1y, t1a, t2x, t2y, t2a,
         gx, gy, gz, ax, ay, az) = struct.unpack("<4bIBBBHHBHHB3h3h", report)  # fmt: skip
        touch = f"T1=({t1x},{t1y})" if t1a else "T1=-"
        touch += f" T2=({t2x},{t2y})" if t2a else " T2=-"
        return (f"DS tasti=0x{buttons:05X} dpad={dpad:X} L2={l2:3d} R2={r2:3d} "
                f"L=({lx:4d},{ly:4d}) R=({rx:4d},{ry:4d}) {touch} "
                f"gyro=({gx},{gy},{gz}) accel=({ax},{ay},{az})")  # fmt: skip
    buttons, lt, rt, lx, ly, rx, ry = struct.unpack("<IBBhhhh6x", report)
    return (f"tasti=0x{buttons:04X} LT={lt:3d} RT={rt:3d} "
            f"L=({lx:6d},{ly:6d}) R=({rx:6d},{ry:6d})")  # fmt: skip


def main() -> int:
    parser = argparse.ArgumentParser(description="Finto server VIIPER (non crea controller veri)")
    parser.add_argument("-p", "--port", type=int, default=3242)
    parser.add_argument("--rumble", type=float, default=0.0, metavar="SECONDI",
                        help="manda una vibrazione di prova a ogni stream con questo intervallo")  # fmt: skip
    parser.add_argument("--quiet", action="store_true", help="non stampare i report")
    args = parser.parse_args()

    last: dict[str, bytes] = {}
    count = [0]

    def on_report(dev: FakeDevice, report: bytes) -> None:
        count[0] += 1
        if not args.quiet and last.get(dev.dev_id) != report:
            last[dev.dev_id] = report
            print(f"bus {dev.bus_id} dev {dev.dev_id}: {decode_report(report)}", flush=True)

    server = FakeViiperServer(port=args.port, on_report=on_report).start()
    print(f"Finto VIIPER in ascolto su 127.0.0.1:{server.port} (Ctrl+C per uscire)", flush=True)
    known: set[tuple[int, str]] = set()
    seen_removed = 0
    step = 0
    next_rumble = time.monotonic() + args.rumble
    try:
        while True:
            time.sleep(0.1)
            current = {(d.bus_id, d.dev_id) for d in server.devices()}
            for bus_id, dev_id in sorted(current - known):
                added = server.buses.get(bus_id, {}).get(dev_id)
                kind = f"{added.dev_type} {added.device_specific or ''}" if added else ""
                print(f"+ dispositivo aggiunto: bus {bus_id} dev {dev_id} {kind}", flush=True)
            known = current
            for bus_id, dev_id in server.removed[seen_removed:]:
                print(f"- dispositivo rimosso: bus {bus_id} dev {dev_id}", flush=True)
            seen_removed = len(server.removed)
            if args.rumble > 0 and time.monotonic() >= next_rumble:
                next_rumble += args.rumble
                step += 1
                left, right = ((200, 50), (0, 0))[step % 2]
                # DualSense: in più barra luminosa a colori alterni e LED del giocatore 1/2.
                color = ((255, 0, 0, 0x04), (0, 0, 255, 0x0A))[step % 2]
                for dev in server.devices():
                    try:
                        if dev.dev_type == "dualsense":
                            server.send_feedback(dev, bytes((right, left, *color)))
                        else:
                            server.send_rumble(dev, left, right)
                    except OSError:
                        pass
                print(f"> vibrazione sinistro={left} destro={right}; DualSense: "
                      f"barra={color[:3]} LED=0x{color[3]:02X}", flush=True)  # fmt: skip
    except KeyboardInterrupt:
        pass
    finally:
        server.stop()
    print(f"Report ricevuti: {count[0]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
