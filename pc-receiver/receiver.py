"""Ricevitore PC: pacchetti UDP dal dongle -> controller virtuale di VIIPER.

Avvio: `python receiver.py` (legge config.toml nella stessa cartella).
Un controller Xbox diventa un pad Xbox 360 virtuale; un DualSense (protocollo v2)
diventa un DualSense virtuale, con touchpad e sensori di movimento.
Vibrazione, barra luminosa e LED giocatore chiesti dai giochi tornano al dongle
come pacchetto RUMBLE/FEEDBACK, all'indirizzo da cui arrivano gli INPUT.
"""

from __future__ import annotations

import argparse
import logging
import select
import signal
import socket
import sys
import time
from typing import Callable

import mapping
import protocol
from config import Config, load_config
from viiper_client import ViiperClient, ViiperError, VirtualPad

log = logging.getLogger("receiver")

Address = tuple[str, int]
# (tipo di dispositivo VIIPER, deviceSpecific facoltativo) -> controller virtuale
PadFactory = Callable[[str, "dict | None"], VirtualPad]


class SeqFilter:
    """Scarta i pacchetti con `seq` più vecchio dell'ultimo accettato (16 bit con wrap)."""

    MAX_REJECTS = 16  # scarti consecutivi dopo i quali ci si riallinea

    def __init__(self) -> None:
        self.last: int | None = None
        self._rejects = 0

    def accept(self, seq: int) -> bool:
        if self.last is not None:
            # Distanza in avanti modulo 2^16: metà dello spazio è "nuovo", metà "vecchio".
            # Distanza 0 (stesso seq) è accettata: non è più vecchio dell'ultimo.
            if (seq - self.last) & 0xFFFF >= 0x8000:
                self._rejects += 1
                if self._rejects < self.MAX_REJECTS:
                    return False
        self.last = seq
        self._rejects = 0
        return True

    def reset(self) -> None:
        self.last = None
        self._rejects = 0


class PadSession:
    """Stato di un controller (un `pad_idx`): filtro seq, timeout, pad virtuale, vibrazione.

    Il tempo arriva sempre come parametro `now` (secondi monotoni), così la logica
    si prova senza attese reali.
    """

    def __init__(
        self,
        pad_idx: int,
        cfg: Config,
        pad_factory: PadFactory,
        send_rumble: Callable[[Address, protocol.RumblePacket], None],
    ) -> None:
        self.pad_idx = pad_idx
        self.cfg = cfg
        self._pad_factory = pad_factory
        self._send_rumble_packet = send_rumble
        self.pad: VirtualPad | None = None
        self.device_type: str | None = None  # tipo del pad virtuale aperto
        self.version = protocol.VERSION  # versione dell'ultimo INPUT: si risponde nella stessa
        self.lightbar: tuple[int, int, int] | None = None  # chiesta dal gioco (solo DualSense)
        self.player_leds: int | None = None
        self.addr: Address | None = None
        self.seq = SeqFilter()
        self.last_rx: float | None = None  # ultimo INPUT accettato
        self.last_active: float | None = None  # ultimo INPUT accettato con connected=1
        self.released = True  # il pad virtuale è a riposo
        self.rumble = (0, 0)  # (weak, strong) richiesti dal gioco
        self.rumble_sent_at: float | None = None
        self.retry_at = 0.0  # non riprovare a creare il pad prima di questo istante
        self._last_error: str | None = None  # per non ripetere lo stesso avviso
        self._multi_source_warned = False
        self.accepted = 0
        self.dropped = 0

    # --- ingresso dal dongle ---------------------------------------------
    def on_input(self, pkt: protocol.InputPacket, addr: Address, now: float) -> bool:
        """Elabora un INPUT; restituisce False se è stato scartato per `seq` vecchio."""
        resumed = self.last_rx is None or now - self.last_rx >= self.cfg.release_s
        if resumed:
            # Primo pacchetto o ripresa dopo un silenzio (es. dongle riavviato):
            # il seq riparte da quello che arriva.
            self.seq.reset()
        if addr != self.addr:
            if resumed:
                log.info("pad %d: ricevo da %s:%d", self.pad_idx, addr[0], addr[1])
            elif not self._multi_source_warned:
                self._multi_source_warned = True
                log.warning(
                    "pad %d: INPUT da più sorgenti insieme (%s:%d e %s:%d): limitare con "
                    "[udp] allowed_sources in config.toml. Avviso mostrato una volta sola.",
                    self.pad_idx, self.addr[0], self.addr[1], addr[0], addr[1],
                )  # fmt: skip
        if not self.seq.accept(pkt.seq):
            self.dropped += 1
            return False
        self.accepted += 1
        self.last_rx = now
        self.addr = addr
        self.version = pkt.version

        if not pkt.connected:
            self._release("controller scollegato dal dongle")
            return True

        self.last_active = now
        wanted = mapping.device_type_for(pkt, self.cfg.dualsense_device)
        if self.pad is not None and self.device_type != wanted:
            # Sul dongle è cambiato controller (es. da Xbox a DualSense).
            log.info("pad %d: il controller ora è %s, sostituisco il pad virtuale",
                     self.pad_idx, wanted)  # fmt: skip
            self._stop_rumble(now)
            self._drop_pad()
        if self.pad is None and not self._open_pad(now, wanted, pkt):
            return True
        self.released = False
        self._send_report(mapping.to_report(self.device_type, pkt, self.cfg.invert_y), now)
        return True

    # --- ingresso da VIIPER ----------------------------------------------
    def on_viiper_readable(self, now: float) -> None:
        """Da chiamare quando lo stream di VIIPER ha dati (o è stato chiuso)."""
        if self.pad is None:
            return
        try:
            events = self.pad.read_feedback()
        except ViiperError as exc:
            log.warning("pad %d: %s", self.pad_idx, exc)
            self._recover_stream(now)
            return
        if not events:
            return
        changed = False
        # Vibrazione: vale l'ultima. Luci: l'ultima impostata (None = non toccata).
        rumble = (events[-1].weak, events[-1].strong)
        if rumble != self.rumble:
            self.rumble = rumble
            changed = True
        for fb in events:
            if fb.lightbar is not None and fb.lightbar != self.lightbar:
                self.lightbar = fb.lightbar
                changed = True
            if fb.player_leds is not None and fb.player_leds != self.player_leds:
                self.player_leds = fb.player_leds
                changed = True
        if changed:
            self._send_rumble(now)

    # --- tempo ------------------------------------------------------------
    def on_tick(self, now: float) -> None:
        if self.pad is not None and self.last_active is not None:
            idle = now - self.last_active
            if idle >= self.cfg.remove_s:
                log.info("pad %d: nessun input da %.0f s, pad virtuale rimosso",
                         self.pad_idx, self.cfg.remove_s)  # fmt: skip
                self._stop_rumble(now)
                self._drop_pad()
            elif idle >= self.cfg.release_s:
                self._release(f"nessun input da {self.cfg.release_s:g} s")

        # Vibrazione: finché è attiva va rinfrescata (il RUMBLE ha una durata);
        # a riposo un RUMBLE a zero fa da segnale di presenza del PC.
        if self.addr is None or self.last_rx is None or now - self.last_rx >= self.cfg.remove_s:
            return
        interval_ms = self.cfg.rumble_refresh_ms if any(self.rumble) else self.cfg.heartbeat_ms
        if interval_ms > 0 and (
            self.rumble_sent_at is None or now - self.rumble_sent_at >= interval_ms / 1000
        ):
            self._send_rumble(now)

    def close(self) -> None:
        if self.addr is not None and any(self.rumble):
            self.rumble = (0, 0)
            self._send_rumble(0.0)
        self._drop_pad()

    # --- interni ----------------------------------------------------------
    def _open_pad(self, now: float, device_type: str, pkt: protocol.InputPacket) -> bool:
        if now < self.retry_at:
            return False
        meta = None
        if device_type == mapping.DEVICE_DUALSENSE:
            # VIIPER 0.9.0 accetta la batteria solo alla creazione del dispositivo.
            status = mapping.battery_status_byte(pkt)
            if status is not None:
                meta = {"battery_status": status}
        pad = self._pad_factory(device_type, meta)
        try:
            pad.connect()
        except ViiperError as exc:
            self.retry_at = now + self.cfg.viiper_retry_s
            message = str(exc)
            if message == self._last_error:
                log.debug("pad %d: pad virtuale non creato: %s", self.pad_idx, message)
                return False
            self._last_error = message
            log.warning("pad %d: pad virtuale non creato: %s", self.pad_idx, message)
            if "usbip" in message.lower():
                log.warning(
                    "VIIPER non trova usbip.exe: installare usbip-win2 (con 'viiper install' "
                    "o da github.com/vadimgrn/usbip-win2), riavviare il PC e poi 'viiper server'. "
                    "Riprovo ogni %g s senza ripetere questo avviso.",
                    self.cfg.viiper_retry_s,
                )
            return False
        if self._last_error is not None:
            log.info("pad %d: VIIPER di nuovo disponibile", self.pad_idx)
            self._last_error = None
        self.pad = pad
        self.device_type = device_type
        log.info("pad %d: %s virtuale creato (bus %s, dispositivo %s)",
                 self.pad_idx, device_type, pad.bus_id, pad.dev_id)  # fmt: skip
        return True

    def _send_report(self, report: bytes, now: float) -> None:
        if self.pad is None:
            return
        try:
            self.pad.send_report(report)
            return
        except ViiperError as exc:
            log.warning("pad %d: %s", self.pad_idx, exc)
        if self._recover_stream(now):
            try:
                self.pad.send_report(report)
            except ViiperError as exc:
                log.warning("pad %d: %s", self.pad_idx, exc)
                self._drop_pad()
                self.retry_at = now + self.cfg.viiper_retry_s

    def _recover_stream(self, now: float) -> bool:
        """Riapre lo stream caduto; se non riesce abbandona il pad e riproverà più tardi."""
        assert self.pad is not None
        try:
            self.pad.reconnect()
        except ViiperError as exc:
            log.warning("pad %d: riconnessione a VIIPER fallita: %s", self.pad_idx, exc)
            self._drop_pad()
            self.retry_at = now + self.cfg.viiper_retry_s
            return False
        log.info("pad %d: stream VIIPER riaperto (bus %s, dispositivo %s)",
                 self.pad_idx, self.pad.bus_id, self.pad.dev_id)  # fmt: skip
        self.rumble = (0, 0)
        return True

    def _release(self, reason: str) -> None:
        if self.released:
            return
        self.released = True
        log.info("pad %d: comandi rilasciati (%s)", self.pad_idx, reason)
        if self.pad is not None:
            try:
                self.pad.send_report(mapping.DEVICE_FORMATS[self.device_type][2])
            except ViiperError as exc:
                log.warning("pad %d: %s", self.pad_idx, exc)

    def _drop_pad(self) -> None:
        if self.pad is not None:
            self.pad.close()
        self.pad = None
        self.device_type = None
        self.released = True
        self.rumble = (0, 0)
        # Il prossimo pad virtuale riparte senza luci impostate: il dongle tiene
        # quelle già applicate al controller.
        self.lightbar = None
        self.player_leds = None

    def _stop_rumble(self, now: float) -> None:
        if any(self.rumble):
            self.rumble = (0, 0)
            self._send_rumble(now)

    def _send_rumble(self, now: float) -> None:
        if self.addr is None:
            return
        weak, strong = self.rumble
        duration = self.cfg.rumble_duration_ms if weak or strong else 0
        if self.version == protocol.VERSION2:
            # FEEDBACK v2: le luci viaggiano in ogni pacchetto, anche nel segnale
            # di presenza; il dongle le inoltra al controller solo quando cambiano.
            pkt = protocol.RumblePacket(
                self.pad_idx, duration, weak, strong,
                version=protocol.VERSION2, lightbar=self.lightbar, player_leds=self.player_leds,
            )  # fmt: skip
        else:
            pkt = protocol.RumblePacket(self.pad_idx, duration, weak, strong)
        self._send_rumble_packet(self.addr, pkt)
        self.rumble_sent_at = now


class Receiver:
    """Socket UDP più una PadSession per ogni `pad_idx` visto."""

    TICK_S = 0.05

    def __init__(
        self,
        cfg: Config,
        pad_factory: PadFactory | None = None,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.cfg = cfg
        self._clock = clock
        if pad_factory is None:
            client = ViiperClient(cfg.viiper_host, cfg.viiper_port, cfg.viiper_timeout_s)

            def pad_factory(device_type: str, meta: dict | None) -> VirtualPad:
                return VirtualPad(client, cfg.viiper_bus_id, device_type, meta)

        self._pad_factory = pad_factory
        self.sessions: dict[int, PadSession] = {}
        self.invalid = 0
        self._stop = False
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.sock.bind((cfg.bind_address, cfg.port))
        except OSError:
            self.sock.close()
            raise
        self.sock.setblocking(False)

    @property
    def port(self) -> int:
        return self.sock.getsockname()[1]

    def step(self, timeout: float | None = None) -> None:
        """Un giro del ciclo: attende dati per al più `timeout` secondi e li elabora."""
        streams = [s.pad for s in self.sessions.values() if s.pad is not None and s.pad.connected]
        try:
            readable, _, _ = select.select(
                [self.sock, *streams], [], [], self.TICK_S if timeout is None else timeout
            )
        except (OSError, ValueError):
            readable = [self.sock, *streams]  # un descrittore è stato chiuso: si verifica sotto
        now = self._clock()
        if self.sock in readable:
            self._drain_udp(now)
        for session in list(self.sessions.values()):
            if session.pad is not None and any(session.pad is r for r in readable):
                session.on_viiper_readable(now)
            session.on_tick(now)

    def run(self, on_idle: Callable[[Receiver], None] | None = None) -> None:
        """Ciclo fino a stop(). `on_idle`, se c'è, viene chiamato dopo ogni giro
        (launcher.py lo usa per sorvegliare VIIPER e la richiesta di arresto)."""
        while not self._stop:
            self.step()
            if on_idle is not None:
                on_idle(self)

    def stop(self) -> None:
        self._stop = True

    def close(self) -> None:
        for session in self.sessions.values():
            session.close()
        self.sock.close()

    def _drain_udp(self, now: float) -> None:
        for _ in range(64):
            try:
                data, addr = self.sock.recvfrom(2048)
            except BlockingIOError:
                return
            except ConnectionResetError:
                # Windows: un RUMBLE inviato a una porta chiusa torna come errore qui.
                continue
            except OSError:
                return
            if self.cfg.allowed_sources and addr[0] not in self.cfg.allowed_sources:
                continue
            try:
                pkt = protocol.parse_input(data)
            except protocol.ProtocolError as exc:
                self.invalid += 1
                log.debug("pacchetto scartato da %s: %s", addr[0], exc)
                continue
            session = self.sessions.get(pkt.pad_idx)
            if session is None:
                session = PadSession(pkt.pad_idx, self.cfg, self._pad_factory, self._send_rumble)
                self.sessions[pkt.pad_idx] = session
            session.on_input(pkt, addr, now)

    def _send_rumble(self, addr: Address, pkt: protocol.RumblePacket) -> None:
        try:
            self.sock.sendto(protocol.build_rumble(pkt), addr)
        except OSError as exc:
            log.debug("invio RUMBLE a %s fallito: %s", addr[0], exc)


def main(argv: list[str] | None = None,
         on_idle: Callable[[Receiver], None] | None = None) -> int:  # fmt: skip
    parser = argparse.ArgumentParser(description="Ricevitore PC di xbox-eth-gateway")
    parser.add_argument("-c", "--config", help="file di configurazione (default: config.toml)")
    parser.add_argument("-v", "--verbose", action="store_true", help="log di debug")
    args = parser.parse_args(argv)

    try:
        cfg = load_config(args.config)
    except (OSError, ValueError) as exc:
        print(f"Configurazione non valida: {exc}", file=sys.stderr)
        return 2
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else cfg.log_level.upper(),
        format="%(asctime)s %(levelname)-7s %(message)s",
        datefmt="%H:%M:%S",
    )

    try:
        receiver = Receiver(cfg)
    except OSError as exc:
        print(f"Impossibile aprire la porta UDP {cfg.bind_address}:{cfg.port}: {exc}",
              file=sys.stderr)  # fmt: skip
        return 1
    log.info("in ascolto su UDP %s:%d", cfg.bind_address, receiver.port)

    try:
        info = ViiperClient(cfg.viiper_host, cfg.viiper_port, cfg.viiper_timeout_s).ping()
        log.info("VIIPER %s su %s:%d", info.get("version", "?"), cfg.viiper_host, cfg.viiper_port)
    except ViiperError as exc:
        log.warning("%s. Riprovo quando arrivano i pacchetti del dongle.", exc)

    # Ctrl+C e Ctrl+Break (o la chiusura da un altro processo) fermano il ciclo
    # e fanno rimuovere il pad virtuale.
    signal.signal(signal.SIGINT, lambda *_: receiver.stop())
    if hasattr(signal, "SIGBREAK"):
        signal.signal(signal.SIGBREAK, lambda *_: receiver.stop())
    signal.signal(signal.SIGTERM, lambda *_: receiver.stop())

    try:
        receiver.run(on_idle)
    finally:
        receiver.close()
        log.info("chiuso, pad virtuali rimossi")
    return 0


if __name__ == "__main__":
    sys.exit(main())
