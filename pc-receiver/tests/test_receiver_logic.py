"""Logica del ricevitore con tempo e VIIPER finti: seq con wrap, timeout, vibrazione."""

import pytest

import mapping
import protocol
from config import Config
from protocol import InputPacket, RumblePacket
from receiver import PadSession, SeqFilter
from viiper_client import ViiperError

ADDR = ("192.0.2.10", 47900)  # indirizzo di documentazione (RFC 5737)


# --- SeqFilter ---------------------------------------------------------------


def test_seq_accepts_increasing():
    f = SeqFilter()
    assert all(f.accept(s) for s in (10, 11, 12, 20))


def test_seq_rejects_older():
    f = SeqFilter()
    assert f.accept(100)
    assert not f.accept(99)
    assert not f.accept(50)
    assert f.accept(101)


def test_seq_accepts_duplicate():
    # Keepalive con lo stesso seq: non è più vecchio dell'ultimo, quindi passa.
    f = SeqFilter()
    assert f.accept(7)
    assert f.accept(7)


def test_seq_wraps():
    f = SeqFilter()
    assert f.accept(65534)
    assert f.accept(65535)
    assert f.accept(0)  # dopo il wrap è più nuovo
    assert f.accept(1)
    assert not f.accept(65535)  # arrivato in ritardo: più vecchio
    assert f.accept(2)


def test_seq_half_window():
    f = SeqFilter()
    assert f.accept(0)
    assert f.accept(0x7FFF)  # massimo salto in avanti
    f.reset()
    assert f.accept(0)
    assert not f.accept(0x8000)  # da metà finestra in poi è considerato vecchio


def test_seq_resyncs_after_many_rejects():
    # Dongle riavviato senza pause: il seq riparte da zero.
    f = SeqFilter()
    assert f.accept(1000)
    results = [f.accept(s) for s in range(SeqFilter.MAX_REJECTS)]
    assert results == [False] * (SeqFilter.MAX_REJECTS - 1) + [True]
    assert f.accept(SeqFilter.MAX_REJECTS)


# --- PadSession --------------------------------------------------------------


class FakePad:
    """Sostituto di viiper_client.VirtualPad che registra tutto.

    `rumble_queue` accetta tuple (sinistro, destro) del pad Xbox 360 oppure
    oggetti mapping.Feedback già pronti.
    """

    def __init__(self, world, device_type="xbox360", meta=None):
        self.world = world
        self.device_type = device_type
        self.meta = meta
        self.reports = []
        self.rumble_queue = []
        self.connected = False
        self.closed = False
        self.reconnects = 0
        self.bus_id, self.dev_id = 1, str(len(world.pads) + 1)

    def connect(self):
        if self.world.viiper_down:
            raise ViiperError("VIIPER non raggiungibile")
        self.connected = True

    def reconnect(self):
        self.reconnects += 1
        self.connect()

    def send_report(self, report):
        if self.world.stream_broken:
            self.world.stream_broken = False  # si rompe una volta sola
            raise ViiperError("stream interrotto")
        self.reports.append(report)

    def read_feedback(self):
        if self.world.stream_broken:
            self.world.stream_broken = False
            raise ViiperError("stream interrotto")
        events, self.rumble_queue = self.rumble_queue, []
        return [e if isinstance(e, mapping.Feedback) else mapping.Feedback.from_xbox360(bytes(e))
                for e in events]  # fmt: skip

    def close(self):
        self.connected = False
        self.closed = True


class World:
    def __init__(self, **overrides):
        self.cfg = Config(**overrides)
        self.pads = []
        self.rumbles = []  # (indirizzo, RumblePacket)
        self.viiper_down = False
        self.stream_broken = False
        self.session = PadSession(0, self.cfg, self._factory, self._send_rumble)
        self.seq = 0

    def _factory(self, device_type, meta):
        pad = FakePad(self, device_type, meta)
        self.pads.append(pad)
        return pad

    def _send_rumble(self, addr, pkt):
        self.rumbles.append((addr, pkt))

    def input(self, now, seq=None, **fields):
        if seq is None:
            self.seq += 1
            seq = self.seq
        return self.session.on_input(InputPacket(seq=seq, **fields), ADDR, now)

    @property
    def pad(self):
        return self.pads[-1]


@pytest.fixture
def world():
    return World()


def test_first_input_creates_pad_and_sends_report(world):
    assert world.input(0.0, buttons=mapping.BUTTON_A, lx=100)
    assert len(world.pads) == 1 and world.pad.connected
    assert world.pad.reports == [
        mapping.to_xbox360_report(InputPacket(buttons=mapping.BUTTON_A, lx=100))
    ]


def test_invert_y_comes_from_config():
    for invert in (True, False):
        w = World(invert_y=invert)
        w.input(0.0, ly=-100)
        assert w.pad.reports[-1] == mapping.to_xbox360_report(InputPacket(ly=-100), invert)


def test_old_seq_is_dropped(world):
    world.input(0.0, seq=10, buttons=mapping.BUTTON_A)
    assert not world.input(0.01, seq=9, buttons=mapping.BUTTON_B)
    assert len(world.pad.reports) == 1
    assert world.session.dropped == 1


def test_seq_wrap_through_session(world):
    for i, seq in enumerate((65534, 65535, 0, 1)):
        assert world.input(i * 0.01, seq=seq)
    assert not world.input(0.05, seq=65535)
    assert len(world.pad.reports) == 4


def test_release_after_one_second(world):
    world.input(0.0, buttons=mapping.BUTTON_A)
    world.session.on_tick(0.99)
    assert len(world.pad.reports) == 1  # ancora niente
    world.session.on_tick(1.0)
    assert world.pad.reports[-1] == mapping.NEUTRAL_REPORT
    world.session.on_tick(1.5)
    assert len(world.pad.reports) == 2  # il rilascio parte una volta sola
    assert not world.pad.closed


def test_remove_after_five_seconds(world):
    world.input(0.0, buttons=mapping.BUTTON_A)
    world.session.on_tick(1.0)
    world.session.on_tick(4.99)
    assert not world.pad.closed
    world.session.on_tick(5.0)
    assert world.pad.closed
    assert world.session.pad is None


def test_keepalive_prevents_timeouts(world):
    for i in range(100):  # un pacchetto ogni 100 ms per 10 secondi
        world.input(i * 0.1, buttons=mapping.BUTTON_A)
        world.session.on_tick(i * 0.1 + 0.05)
    assert len(world.pads) == 1 and not world.pad.closed
    assert mapping.NEUTRAL_REPORT not in world.pad.reports


def test_dropped_packets_do_not_refresh_timeouts(world):
    world.input(0.0, seq=100, buttons=mapping.BUTTON_A)
    for i in range(1, 10):
        world.input(i * 0.1, seq=50)  # tutti vecchi
    world.session.on_tick(1.0)
    assert world.pad.reports[-1] == mapping.NEUTRAL_REPORT


def test_pad_recreated_when_inputs_resume(world):
    world.input(0.0)
    world.session.on_tick(5.0)
    assert world.pad.closed
    world.input(6.0, buttons=mapping.BUTTON_B)
    assert len(world.pads) == 2 and world.pad.connected
    assert world.pad.reports == [mapping.to_xbox360_report(InputPacket(buttons=mapping.BUTTON_B))]


def test_seq_restarts_after_silence(world):
    # Dongle riavviato: dopo un silenzio più lungo di release_s il seq riparte.
    world.input(0.0, seq=100, buttons=mapping.BUTTON_A)
    world.session.on_tick(1.0)
    assert world.input(3.0, seq=0, buttons=mapping.BUTTON_B)
    assert world.pad.reports[-1] == mapping.to_xbox360_report(InputPacket(buttons=mapping.BUTTON_B))


def test_disconnected_flag_releases_immediately(world):
    world.input(0.0, buttons=mapping.BUTTON_A)
    world.input(0.1, flags=0)
    assert world.pad.reports[-1] == mapping.NEUTRAL_REPORT
    # I keepalive con connected=0 non tengono in vita il pad virtuale.
    for i in range(2, 60):
        world.input(i * 0.1, flags=0)
        world.session.on_tick(i * 0.1)
    assert world.pad.closed
    assert len(world.pads) == 1


def test_disconnected_flag_does_not_create_pad(world):
    world.input(0.0, flags=0)
    assert world.pads == []


def test_rumble_forwarded_to_input_source(world):
    world.input(0.0)
    world.pad.rumble_queue = [(200, 50)]  # sinistro (grande), destro (piccolo)
    world.session.on_viiper_readable(0.01)
    assert world.rumbles == [(ADDR, RumblePacket(pad_idx=0, duration_ms=500, weak=50, strong=200))]


def test_rumble_uses_last_value_and_skips_unchanged(world):
    world.input(0.0)
    world.pad.rumble_queue = [(10, 10), (30, 40)]
    world.session.on_viiper_readable(0.01)
    world.pad.rumble_queue = [(30, 40)]
    world.session.on_viiper_readable(0.02)
    assert [p for _, p in world.rumbles] == [RumblePacket(0, 500, 40, 30)]


def test_rumble_refreshed_while_active_then_stopped(world):
    world.input(0.0)
    world.pad.rumble_queue = [(255, 0)]
    world.session.on_viiper_readable(0.0)
    for i in range(1, 11):  # 1 secondo a passi di 100 ms, con keepalive
        world.input(i * 0.1)
        world.session.on_tick(i * 0.1)
    active = [p for _, p in world.rumbles]
    assert len(active) == 4  # invio iniziale + rinnovi a 0.3, 0.6 e 0.9 (tick ogni 100 ms)
    assert all(p == RumblePacket(0, 500, 0, 255) for p in active)

    world.pad.rumble_queue = [(0, 0)]
    world.session.on_viiper_readable(1.05)
    assert world.rumbles[-1][1] == RumblePacket(0, 0, 0, 0)


def test_heartbeat_when_idle(world):
    for i in range(31):  # 3 secondi di soli keepalive
        world.input(i * 0.1)
        world.session.on_tick(i * 0.1)
    beats = [p for _, p in world.rumbles]
    assert len(beats) == 4  # a 0, 1, 2 e 3 secondi
    assert all(p == RumblePacket(0, 0, 0, 0) for p in beats)


def test_heartbeat_can_be_disabled():
    w = World(heartbeat_ms=0)
    for i in range(31):
        w.input(i * 0.1)
        w.session.on_tick(i * 0.1)
    assert w.rumbles == []


def test_heartbeat_stops_when_dongle_is_silent(world):
    world.input(0.0)
    for i in range(1, 200):
        world.session.on_tick(i * 0.1)
    # Un RUMBLE al secondo finché il dongle è stato sentito negli ultimi 5 s, poi basta.
    assert len(world.rumbles) == 5
    assert world.session.rumble_sent_at < 5.0


def test_rumble_stopped_when_pad_removed(world):
    world.input(0.0)
    world.pad.rumble_queue = [(100, 100)]
    world.session.on_viiper_readable(0.0)
    world.session.on_tick(5.0)
    assert world.rumbles[-1][1] == RumblePacket(0, 0, 0, 0)
    assert world.session.rumble == (0, 0)


def test_viiper_down_is_retried_with_backoff(world):
    world.viiper_down = True
    world.input(0.0)
    world.input(0.1)
    world.input(2.9)
    assert len(world.pads) == 1  # un solo tentativo entro retry_s (3 s)
    assert world.session.pad is None
    world.viiper_down = False
    world.input(3.0, buttons=mapping.BUTTON_A)
    assert len(world.pads) == 2 and world.session.pad is world.pad
    assert len(world.pad.reports) == 1


def test_same_viiper_error_is_logged_once(world, caplog):
    world.viiper_down = True
    with caplog.at_level("WARNING", logger="receiver"):
        for t in (0.0, 3.0, 6.0, 9.0):
            world.input(t)
    assert len(world.pads) == 4  # ritenta ogni retry_s...
    assert len([r for r in caplog.records if "non creato" in r.getMessage()]) == 1  # ...ma avvisa una volta
    caplog.clear()
    world.viiper_down = False
    with caplog.at_level("INFO", logger="receiver"):
        world.input(12.0)
    assert any("di nuovo disponibile" in r.getMessage() for r in caplog.records)


def test_two_sources_warn_once(world, caplog):
    other = ("192.0.2.20", 50000)
    with caplog.at_level("INFO", logger="receiver"):
        for i in range(20):
            addr = ADDR if i % 2 else other
            world.session.on_input(InputPacket(seq=i), addr, i * 0.01)
    messages = [r.getMessage() for r in caplog.records]
    assert sum("ricevo da" in m for m in messages) == 1
    assert sum("più sorgenti" in m for m in messages) == 1


def test_missing_usbip_gives_install_hint(world, caplog, monkeypatch):
    def fail():
        raise ViiperError("'bus/1/add': 409 Failed to auto-attach device: exec: \"usbip\": "
                          "executable file not found in %PATH%", 409)  # fmt: skip


    monkeypatch.setattr(FakePad, "connect", lambda self: fail())
    with caplog.at_level("WARNING", logger="receiver"):
        world.input(0.0)
    assert any("usbip-win2" in r.getMessage() for r in caplog.records)


def test_stream_error_on_send_reconnects_and_resends(world):
    world.input(0.0)
    world.stream_broken = True
    world.input(0.1, buttons=mapping.BUTTON_A)
    assert world.pad.reconnects == 1
    assert world.pad.reports[-1] == mapping.to_xbox360_report(InputPacket(buttons=mapping.BUTTON_A))
    assert len(world.pads) == 1


def test_stream_error_on_read_reconnects(world):
    world.input(0.0)
    world.stream_broken = True
    world.session.on_viiper_readable(0.1)
    assert world.pad.reconnects == 1 and world.session.pad is world.pad


def test_failed_reconnect_drops_pad_and_retries_later(world):
    world.input(0.0)
    world.stream_broken = True
    world.viiper_down = True
    world.input(0.1)
    assert world.session.pad is None and world.pad.closed
    world.viiper_down = False
    world.input(1.0)
    assert world.session.pad is None  # ancora dentro retry_s
    world.input(3.2)
    assert world.session.pad is not None and len(world.pads) == 2


def test_close_removes_pad_and_stops_rumble(world):
    world.input(0.0)
    world.pad.rumble_queue = [(9, 9)]
    world.session.on_viiper_readable(0.0)
    world.session.close()
    assert world.pad.closed
    assert world.rumbles[-1][1] == RumblePacket(0, 0, 0, 0)
