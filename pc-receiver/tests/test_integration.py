"""Ricevitore completo: UDP vero su localhost + finto server VIIPER, tempi accorciati."""

import socket
import threading
import time

import pytest

import mapping
import protocol
from config import Config, load_config
from fake_viiper import FakeViiperServer
from protocol import InputPacket
from receiver import Receiver


def wait_for(condition, timeout=3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if condition():
            return
        time.sleep(0.01)
    assert condition(), "condizione non raggiunta in tempo"


class Dongle:
    """Finto dongle: un socket UDP che invia INPUT e raccoglie i RUMBLE."""

    def __init__(self, port):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.05)
        self.target = ("127.0.0.1", port)
        self.seq = 0

    def send(self, **fields):
        self.seq += 1
        self.sock.sendto(protocol.build_input(InputPacket(seq=self.seq, **fields)), self.target)

    def rumbles(self, seconds, enough=lambda out: False):
        """Raccoglie i RUMBLE per `seconds`, o finché `enough(lista)` diventa vero."""
        out = []
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline and not enough(out):
            try:
                out.append(protocol.parse_rumble(self.sock.recv(64)))
            except (socket.timeout, ConnectionResetError):
                pass
        return out

    def close(self):
        self.sock.close()


@pytest.fixture
def setup():
    with FakeViiperServer(connect_timeout=5.0) as server:
        cfg = Config(
            bind_address="127.0.0.1", port=0, viiper_port=server.port,
            release_s=0.3, remove_s=0.8, viiper_retry_s=0.2,
            rumble_duration_ms=500, rumble_refresh_ms=100, heartbeat_ms=0,
        )  # fmt: skip
        receiver = Receiver(cfg)
        receiver.TICK_S = 0.01
        thread = threading.Thread(target=receiver.run, daemon=True)
        thread.start()
        dongle = Dongle(receiver.port)
        try:
            yield server, receiver, dongle
        finally:
            receiver.stop()
            thread.join(timeout=2)
            receiver.close()
            dongle.close()


def test_input_reaches_viiper_as_xbox360_report(setup):
    server, _, dongle = setup
    dongle.send(buttons=mapping.BUTTON_A, dpad=mapping.DPAD_RIGHT, lx=511, ly=-512, lt=1023)
    wait_for(lambda: server.devices() and server.devices()[0].reports)
    report = server.devices()[0].reports[0]
    assert report == mapping.to_xbox360_report(
        InputPacket(buttons=mapping.BUTTON_A, dpad=mapping.DPAD_RIGHT, lx=511, ly=-512, lt=1023)
    )
    # Controllo esplicito dei byte: A + D-pad destro, LT 255, LX 32704, LY +32767 (invertito).
    assert report == bytes.fromhex("08100000" "ff" "00" "c07f" "ff7f" "0000" "0000") + bytes(6)


def test_invalid_and_old_packets_are_ignored(setup):
    server, receiver, dongle = setup
    dongle.sock.sendto(b"non e' un pacchetto", dongle.target)
    dongle.seq = 100
    dongle.send(buttons=mapping.BUTTON_A)  # seq 101
    wait_for(lambda: server.devices() and len(server.devices()[0].reports) == 1)
    dongle.seq = 50
    dongle.send(buttons=mapping.BUTTON_B)  # seq 51: vecchio
    dongle.seq = 101
    dongle.send(buttons=mapping.BUTTON_X)  # seq 102
    wait_for(lambda: len(server.devices()[0].reports) == 2)
    pressed = [int.from_bytes(r[:4], "little") for r in server.devices()[0].reports]
    assert pressed == [mapping.X360_A, mapping.X360_X]
    assert receiver.invalid == 1
    assert receiver.sessions[0].dropped == 1


def test_release_then_remove_on_silence(setup):
    server, _, dongle = setup
    start = time.monotonic()  # i timeout partono dalla ricezione, quindi non prima di qui
    dongle.send(buttons=mapping.BUTTON_A)
    wait_for(lambda: server.devices() and server.devices()[0].reports)
    dev = server.devices()[0]
    wait_for(lambda: dev.reports[-1] == mapping.NEUTRAL_REPORT)  # rilascio a 0,3 s
    assert time.monotonic() - start >= 0.25  # margine per la risoluzione dell'orologio
    wait_for(lambda: server.devices() == [])  # rimozione a 0,8 s
    assert time.monotonic() - start >= 0.75
    assert (dev.bus_id, dev.dev_id) in server.removed
    # Il bus creato dal ricevitore viene tolto subito dopo (richiesta successiva).
    wait_for(lambda: server.buses == {})


def test_rumble_returns_to_input_source(setup):
    server, _, dongle = setup
    dongle.send()
    wait_for(lambda: server.devices() and server.devices()[0].stream is not None)
    server.send_rumble(server.devices()[0], 200, 50)

    stop = threading.Event()

    def keepalive():
        while not stop.wait(0.05):
            dongle.send()

    thread = threading.Thread(target=keepalive, daemon=True)
    thread.start()
    try:
        # Invio iniziale più i rinnovi ogni 100 ms: ne bastano tre.
        got = dongle.rumbles(3.0, enough=lambda out: len(out) >= 3)
        assert len(got) >= 3
        assert all(r == protocol.RumblePacket(0, 500, 50, 200) for r in got)

        stop_packet = protocol.RumblePacket(0, 0, 0, 0)
        server.send_rumble(server.devices()[0], 0, 0)
        assert stop_packet in dongle.rumbles(3.0, enough=lambda out: stop_packet in out)
    finally:
        stop.set()
        thread.join()


def test_pad_survives_viiper_stream_drop(setup):
    server, _, dongle = setup
    dongle.send(buttons=mapping.BUTTON_A)
    wait_for(lambda: server.devices() and server.devices()[0].reports)
    dev = server.devices()[0]
    server.drop_stream(dev)
    wait_for(lambda: dev.stream_opens == 2)  # il ricevitore riapre lo stream da solo
    dongle.send(buttons=mapping.BUTTON_B)
    wait_for(lambda: int.from_bytes(dev.reports[-1][:4], "little") == mapping.X360_B)
    assert server.devices() == [dev]  # stesso dispositivo, non uno nuovo


def test_close_removes_virtual_pad(setup):
    server, receiver, dongle = setup
    dongle.send()
    wait_for(lambda: len(server.devices()) == 1)
    receiver.stop()
    time.sleep(0.1)
    receiver.close()
    assert server.devices() == []
    assert server.buses == {}


def test_two_pad_indexes_get_two_devices(setup):
    server, _, dongle = setup
    dongle.send(pad_idx=0, buttons=mapping.BUTTON_A)
    dongle.send(pad_idx=1, buttons=mapping.BUTTON_B)
    wait_for(lambda: len(server.devices()) == 2 and all(d.reports for d in server.devices()))
    pressed = sorted(int.from_bytes(d.reports[0][:4], "little") for d in server.devices())
    assert pressed == [mapping.X360_A, mapping.X360_B]


def test_dualsense_end_to_end(setup):
    server, _, dongle = setup
    flags = protocol.FLAG_CONNECTED | protocol.FLAG_MOTION | protocol.FLAG_TOUCH
    fields = dict(version=protocol.VERSION2, kind=protocol.KIND_DUALSENSE, flags=flags,
                  buttons=mapping.BUTTON_B, battery=100, power=protocol.POWER_FULL,
                  ext_buttons=protocol.EXT_TOUCHPAD, touch=((100, 200, True), (0, 0, False)),
                  gyro=(10, 20, 30), accel=(0, 8192, 0))  # fmt: skip
    dongle.send(**fields)
    wait_for(lambda: server.devices() and server.devices()[0].reports)
    dev = server.devices()[0]
    assert dev.dev_type == "dualsense"
    assert dev.device_specific == {"battery_status": 0x2A}
    assert dev.reports[0] == mapping.to_dualsense_state(InputPacket(**fields))

    # Vibrazione e luci dal gioco: tornano al dongle come FEEDBACK v2.
    server.send_feedback(dev, bytes((50, 200, 0, 255, 0, 0x15)))
    expected = protocol.RumblePacket(0, 500, 50, 200, version=2, lightbar=(0, 255, 0),
                                     player_leds=0x15)  # fmt: skip
    got = dongle.rumbles(3.0, enough=lambda out: expected in out)
    assert expected in got


def test_shipped_config_matches_defaults():
    # Il config.toml di esempio deve caricarsi e non deve cambiare i default.
    assert load_config() == Config()


def test_config_rejects_unknown_key(tmp_path):
    path = tmp_path / "config.toml"
    path.write_text("[udp]\nprot = 47900\n", encoding="utf-8")
    with pytest.raises(ValueError, match="udp.prot"):
        load_config(path)
