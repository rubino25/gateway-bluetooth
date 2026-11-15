"""Client TCP per VIIPER (https://github.com/Alia5/VIIPER) su localhost, senza autenticazione.

API (docs/api/overview.md del repository, verificata il 2026-11-14):
- una richiesta per connessione: `percorso[ payload]\\0`; la risposta è una riga
  JSON (o vuota) e termina con la chiusura della connessione;
- gli errori sono JSON con `status`, `title`, `detail`;
- lo stream del dispositivo è una seconda connessione: si invia `bus/{bus}/{dev}\\0`
  e da lì in poi viaggiano i dati binari (il server non risponde nulla);
- dopo `add`, e dopo ogni caduta dello stream, il server aspetta 5 s che lo stream
  venga (ri)aperto, poi rimuove il dispositivo.
"""

from __future__ import annotations

import json
import select
import socket

import mapping

_MAX_RESPONSE = 64 * 1024


class ViiperError(Exception):
    """Errore di rete verso VIIPER o risposta di errore del server."""

    def __init__(self, message: str, status: int | None = None):
        super().__init__(message)
        self.status = status  # codice restituito dal server, None per errori di rete


class ViiperClient:
    """Richieste all'API di gestione di VIIPER."""

    def __init__(self, host: str = "127.0.0.1", port: int = 3242, timeout: float = 1.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        # Bus creati da questo processo: vanno rimossi all'uscita, quando sono vuoti.
        self.owned_buses: set[int] = set()

    def _connect(self) -> socket.socket:
        try:
            sock = socket.create_connection((self.host, self.port), self.timeout)
        except OSError as exc:
            raise ViiperError(f"VIIPER non raggiungibile su {self.host}:{self.port}: {exc}") from exc
        sock.settimeout(self.timeout)
        return sock

    def request(self, path: str, payload: str | None = None) -> dict:
        line = path if payload is None else f"{path} {payload}"
        sock = self._connect()
        try:
            sock.sendall(line.encode("utf-8") + b"\0")
            raw = bytearray()
            while len(raw) < _MAX_RESPONSE:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                raw += chunk
        except OSError as exc:
            raise ViiperError(f"richiesta '{path}' fallita: {exc}") from exc
        finally:
            sock.close()

        text = raw.decode("utf-8", "replace").strip("\r\n\t \0")
        if not text:
            return {}
        try:
            obj = json.loads(text)
        except json.JSONDecodeError as exc:
            raise ViiperError(f"risposta non valida a '{path}': {text[:80]!r}") from exc
        if not isinstance(obj, dict):
            raise ViiperError(f"risposta inattesa a '{path}': {text[:80]!r}")
        status = obj.get("status")
        if isinstance(status, int) and "title" in obj:
            detail = obj.get("detail") or obj["title"]
            raise ViiperError(f"'{path}': {status} {detail}", status)
        return obj

    def ping(self) -> dict:
        return self.request("ping")

    def bus_list(self) -> list[int]:
        return [int(b) for b in self.request("bus/list").get("buses") or []]

    def bus_create(self, bus_id: int | None = None) -> int:
        reply = self.request("bus/create", None if bus_id is None else str(bus_id))
        return int(self._field(reply, "busId", "bus/create"))

    def bus_remove(self, bus_id: int) -> None:
        self.request("bus/remove", str(bus_id))

    def device_list(self, bus_id: int) -> list[dict]:
        return list(self.request(f"bus/{bus_id}/list").get("devices") or [])

    def device_add(self, bus_id: int, device_type: str = "xbox360",
                   device_specific: dict | None = None) -> str:  # fmt: skip
        spec: dict = {"type": device_type}
        if device_specific:
            spec["deviceSpecific"] = device_specific
        reply = self.request(f"bus/{bus_id}/add", json.dumps(spec))
        return str(self._field(reply, "devId", "add"))

    def device_remove(self, bus_id: int, dev_id: str) -> None:
        self.request(f"bus/{bus_id}/remove", str(dev_id))

    def open_stream(self, bus_id: int, dev_id: str) -> socket.socket:
        """Apre lo stream binario del dispositivo."""
        sock = self._connect()
        try:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.sendall(f"bus/{bus_id}/{dev_id}".encode("ascii") + b"\0")
        except OSError as exc:
            sock.close()
            raise ViiperError(f"apertura dello stream fallita: {exc}") from exc
        return sock

    @staticmethod
    def _field(reply: dict, name: str, what: str):
        if name not in reply:
            raise ViiperError(f"risposta a '{what}' senza '{name}': {reply!r}")
        return reply[name]


class VirtualPad:
    """Un controller virtuale di VIIPER (Xbox 360 o DualSense): dispositivo su un
    bus più il suo stream. Formati dello stream in mapping.DEVICE_FORMATS."""

    def __init__(self, client: ViiperClient, bus_id: int = 0,
                 device_type: str = mapping.DEVICE_XBOX360,
                 device_specific: dict | None = None):  # fmt: skip
        if device_type not in mapping.DEVICE_FORMATS:
            raise ValueError(f"tipo di dispositivo non gestito: {device_type}")
        self.client = client
        self.device_type = device_type
        self.device_specific = device_specific  # facoltativo, solo alla creazione
        self.report_size, self.feedback_size, self.neutral_report, self._parse_feedback = (
            mapping.DEVICE_FORMATS[device_type]
        )
        self._wanted_bus = bus_id  # 0 = riusa un bus esistente o ne crea uno
        self.bus_id: int | None = None
        self.dev_id: str | None = None
        self._sock: socket.socket | None = None
        self._rx = b""

    @property
    def connected(self) -> bool:
        return self._sock is not None

    def fileno(self) -> int:
        """Permette di passare l'oggetto a select() per attendere la vibrazione."""
        if self._sock is None:
            raise ViiperError("stream non aperto")
        return self._sock.fileno()

    def connect(self) -> None:
        """Crea il dispositivo e apre subito lo stream (il server concede 5 s)."""
        self._close_stream()
        self._ensure_bus()
        before = self._device_ids()
        try:
            self.dev_id = self.client.device_add(self.bus_id, self.device_type, self.device_specific)
        except ViiperError as exc:
            if exc.status is not None:
                # VIIPER 0.9.0: se il collegamento automatico fallisce (es. usbip.exe
                # mancante) risponde 409 ma il dispositivo resta creato e non viene
                # più rimosso. Si tolgono quelli comparsi con questa richiesta.
                self._remove_new_devices(before)
            raise
        try:
            self._sock = self.client.open_stream(self.bus_id, self.dev_id)
        except ViiperError:
            self._remove_device()
            raise

    def reconnect(self) -> None:
        """Dopo la caduta dello stream: lo riapre, o ricrea il dispositivo se è sparito."""
        self._close_stream()
        if self.bus_id is not None and self.dev_id is not None:
            try:
                devices = self.client.device_list(self.bus_id)
            except ViiperError as exc:
                if exc.status is None:
                    raise  # server non raggiungibile: inutile insistere adesso
                devices = []  # il bus non esiste più (server riavviato)
            if any(str(d.get("devId")) == self.dev_id for d in devices):
                self._sock = self.client.open_stream(self.bus_id, self.dev_id)
                return
        self.dev_id = None
        self.connect()

    def send_report(self, report: bytes) -> None:
        if len(report) != self.report_size:
            raise ValueError(f"report di {len(report)} byte, attesi {self.report_size}")
        if self._sock is None:
            raise ViiperError("stream non aperto")
        try:
            self._sock.sendall(report)
        except OSError as exc:
            self._close_stream()
            raise ViiperError(f"stream interrotto: {exc}") from exc

    def read_feedback(self) -> list[mapping.Feedback]:
        """Legge senza bloccare i pacchetti di ritorno arrivati (vibrazione, luci)."""
        if self._sock is None:
            raise ViiperError("stream non aperto")
        try:
            while select.select([self._sock], [], [], 0)[0]:
                chunk = self._sock.recv(256)
                if not chunk:
                    raise ConnectionResetError("chiuso dal server")
                self._rx += chunk
        except (OSError, ValueError) as exc:
            self._close_stream()
            raise ViiperError(f"stream interrotto: {exc}") from exc
        size = self.feedback_size
        whole = len(self._rx) - len(self._rx) % size
        data, self._rx = self._rx[:whole], self._rx[whole:]
        return [self._parse_feedback(data[i:i + size]) for i in range(0, whole, size)]

    def read_rumble(self) -> list[tuple[int, int]]:
        """Solo vibrazione, come (sinistro, destro) del pad Xbox 360."""
        return [(f.strong, f.weak) for f in self.read_feedback()]

    def close(self) -> None:
        """Chiude lo stream, rimuove il dispositivo e il bus se creato da noi e vuoto."""
        self._close_stream()
        self._remove_device()
        bus = self.bus_id
        if bus is not None and bus in self.client.owned_buses:
            try:
                if not self.client.device_list(bus):
                    self.client.bus_remove(bus)
                    self.client.owned_buses.discard(bus)
            except ViiperError:
                pass
        self.bus_id = None

    def _ensure_bus(self) -> None:
        buses = self.client.bus_list()
        if self._wanted_bus:
            if self._wanted_bus not in buses:
                self.client.bus_create(self._wanted_bus)
                self.client.owned_buses.add(self._wanted_bus)
            self.bus_id = self._wanted_bus
        elif self.bus_id is not None and self.bus_id in buses:
            pass
        elif buses:
            self.bus_id = min(buses)
        else:
            self.bus_id = self.client.bus_create()
            self.client.owned_buses.add(self.bus_id)

    def _device_ids(self) -> set[str] | None:
        try:
            return {str(d.get("devId")) for d in self.client.device_list(self.bus_id)}
        except ViiperError:
            return None

    def _remove_new_devices(self, before: set[str] | None) -> None:
        if before is None:
            return  # elenco iniziale non disponibile: meglio non toccare nulla
        after = self._device_ids() or set()
        for dev_id in sorted(after - before):
            try:
                self.client.device_remove(self.bus_id, dev_id)
            except ViiperError:
                pass

    def _remove_device(self) -> None:
        if self.bus_id is not None and self.dev_id is not None:
            try:
                self.client.device_remove(self.bus_id, self.dev_id)
            except ViiperError:
                pass  # già rimosso dal server, o server non raggiungibile
        self.dev_id = None

    def _close_stream(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
        self._sock = None
        self._rx = b""


class Xbox360Pad(VirtualPad):
    """Controller Xbox 360 virtuale (nome storico, usato dai test)."""

    def __init__(self, client: ViiperClient, bus_id: int = 0):
        super().__init__(client, bus_id, mapping.DEVICE_XBOX360)


class DualSensePad(VirtualPad):
    """Controller DualSense virtuale."""

    def __init__(self, client: ViiperClient, bus_id: int = 0, device_specific: dict | None = None):
        super().__init__(client, bus_id, mapping.DEVICE_DUALSENSE, device_specific)
