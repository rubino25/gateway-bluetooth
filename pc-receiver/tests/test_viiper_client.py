"""Client VIIPER contro il finto server TCP locale (fake_viiper.py)."""

import socket
import time

import pytest

import mapping
from fake_viiper import FakeViiperServer
from protocol import InputPacket
from viiper_client import ViiperClient, ViiperError, Xbox360Pad


def wait_for(condition, timeout=2.0):
    """Attende che `condition()` diventi vera; fallisce il test allo scadere."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if condition():
            return
        time.sleep(0.01)
    assert condition(), "condizione non raggiunta in tempo"


@pytest.fixture
def server():
    # Timeout largo: i test che vogliono vedere la rimozione lo accorciano da soli,
    # gli altri non devono dipendere dalla velocità della macchina.
    with FakeViiperServer(connect_timeout=5.0) as srv:
        yield srv


@pytest.fixture
def client(server):
    return ViiperClient("127.0.0.1", server.port, timeout=1.0)


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


# --- API ---------------------------------------------------------------------


def test_ping(client):
    assert client.ping()["server"] == "VIIPER"


def test_request_framing(server, client):
    client.bus_create(5)
    client.device_add(5)
    assert server.requests == ["bus/create 5", 'bus/5/add {"type": "xbox360"}']


def test_bus_create_list_remove(client):
    assert client.bus_list() == []
    assert client.bus_create() == 1
    assert client.bus_create(7) == 7
    assert client.bus_list() == [1, 7]
    client.bus_remove(1)
    assert client.bus_list() == [7]


def test_device_add_list_remove(client):
    bus = client.bus_create()
    dev = client.device_add(bus)
    assert isinstance(dev, str)
    assert [d["devId"] for d in client.device_list(bus)] == [dev]
    client.device_remove(bus, dev)
    assert client.device_list(bus) == []


def test_server_error_raises_with_status(client):
    with pytest.raises(ViiperError) as err:
        client.device_list(99)
    assert err.value.status == 404
    client.bus_create(3)
    with pytest.raises(ViiperError) as err:
        client.bus_create(3)
    assert err.value.status == 409


def test_unreachable_server_raises():
    client = ViiperClient("127.0.0.1", free_port(), timeout=0.3)
    with pytest.raises(ViiperError) as err:
        client.ping()
    assert err.value.status is None


# --- Xbox360Pad ----------------------------------------------------------------


def test_pad_connect_creates_bus_device_and_stream(server, client):
    pad = Xbox360Pad(client)
    pad.connect()
    assert pad.connected and pad.bus_id == 1
    wait_for(lambda: server.devices() and server.devices()[0].stream is not None)
    assert server.devices()[0].dev_id == pad.dev_id
    pad.close()


def test_stream_opened_in_time_keeps_device(server, client):
    pad = Xbox360Pad(client)
    pad.connect()
    # Con lo stream aperto il server annulla la scadenza del dispositivo.
    wait_for(lambda: server.devices()[0].deadline is None)
    time.sleep(0.2)
    assert len(server.devices()) == 1
    pad.close()


def test_device_without_stream_is_removed_by_server(server, client):
    # Controprova: è l'apertura dello stream a tenere in vita il dispositivo.
    server.connect_timeout = 0.2
    bus = client.bus_create()
    client.device_add(bus)
    wait_for(lambda: server.devices() == [])


def test_pad_sends_reports(server, client):
    pad = Xbox360Pad(client)
    pad.connect()
    first = mapping.to_xbox360_report(InputPacket(buttons=mapping.BUTTON_A, lx=511))
    second = mapping.to_xbox360_report(InputPacket(lt=1023))
    pad.send_report(first)
    pad.send_report(second)
    dev = server.devices()[0]
    wait_for(lambda: len(dev.reports) == 2)
    assert dev.reports == [first, second]
    pad.close()


def test_pad_rejects_wrong_report_size(client):
    pad = Xbox360Pad(client)
    pad.connect()
    with pytest.raises(ValueError):
        pad.send_report(b"\x00" * 19)
    pad.close()


def test_pad_reads_rumble(server, client):
    pad = Xbox360Pad(client)
    pad.connect()
    assert pad.read_rumble() == []  # niente in arrivo: non blocca
    wait_for(lambda: server.devices()[0].stream is not None)
    dev = server.devices()[0]
    server.send_rumble(dev, 200, 50)
    server.send_rumble(dev, 0, 0)
    events = []
    wait_for(lambda: events.extend(pad.read_rumble()) or len(events) == 2)
    assert events == [(200, 50), (0, 0)]
    pad.close()


def test_pad_reassembles_split_rumble(server, client):
    pad = Xbox360Pad(client)
    pad.connect()
    wait_for(lambda: server.devices()[0].stream is not None)
    stream = server.devices()[0].stream
    stream.sendall(b"\x11")
    time.sleep(0.1)
    assert pad.read_rumble() == []  # mezzo pacchetto: resta in attesa
    stream.sendall(b"\x22")
    events = []
    wait_for(lambda: events.extend(pad.read_rumble()) or events)
    assert events == [(0x11, 0x22)]
    pad.close()


def test_pad_detects_closed_stream_and_reopens_it(server, client):
    pad = Xbox360Pad(client)
    pad.connect()
    wait_for(lambda: server.devices()[0].stream is not None)
    dev = server.devices()[0]
    server.drop_stream(dev)

    def stream_reported_closed():
        try:
            pad.read_rumble()
        except ViiperError:
            return True
        return False

    wait_for(stream_reported_closed)
    assert not pad.connected

    pad.reconnect()  # entro il timeout: stesso dispositivo, nuovo stream
    assert pad.connected and pad.dev_id == dev.dev_id
    wait_for(lambda: dev.stream_opens == 2)
    report = mapping.to_xbox360_report(InputPacket(buttons=mapping.BUTTON_B))
    pad.send_report(report)
    wait_for(lambda: dev.reports == [report])
    assert len(server.devices()) == 1
    pad.close()


def test_pad_reconnect_recreates_removed_device(server, client):
    pad = Xbox360Pad(client)
    pad.connect()
    old_dev = pad.dev_id
    wait_for(lambda: server.devices()[0].stream is not None)
    server.connect_timeout = 0.2
    server.drop_stream(server.devices()[0])
    wait_for(lambda: server.devices() == [])  # il server lo rimuove per timeout

    pad.reconnect()
    assert pad.connected and pad.dev_id != old_dev
    assert [d.dev_id for d in server.devices()] == [pad.dev_id]
    pad.close()


def test_pad_reconnect_after_server_lost_everything(server, client):
    pad = Xbox360Pad(client)
    pad.connect()
    with server.lock:  # come un riavvio di VIIPER: bus e dispositivi spariti
        server.buses.clear()
    pad.reconnect()
    assert pad.connected
    assert len(server.devices()) == 1
    pad.close()


def test_pad_reconnect_fails_when_server_is_down(client):
    pad = Xbox360Pad(client)
    pad.connect()
    client.port = free_port()
    client.timeout = 0.3
    with pytest.raises(ViiperError):
        pad.reconnect()
    assert not pad.connected


def test_close_removes_device_and_owned_bus(server, client):
    pad = Xbox360Pad(client)
    pad.connect()
    bus, dev = pad.bus_id, pad.dev_id
    pad.close()
    assert server.devices() == []
    assert server.buses == {}
    assert f"bus/{bus}/remove {dev}" in server.requests
    assert f"bus/remove {bus}" in server.requests
    assert not pad.connected


def test_close_keeps_bus_created_by_others(server, client):
    client.bus_create(4)
    pad = Xbox360Pad(client)
    pad.connect()
    assert pad.bus_id == 4  # riusa il bus esistente
    pad.close()
    assert server.devices() == []
    assert list(server.buses) == [4]


def test_configured_bus_id_is_created_and_removed(server, client):
    pad = Xbox360Pad(client, bus_id=9)
    pad.connect()
    assert pad.bus_id == 9 and list(server.buses) == [9]
    pad.close()
    assert server.buses == {}


def test_two_pads_share_bus_and_last_close_removes_it(server, client):
    first, second = Xbox360Pad(client), Xbox360Pad(client)
    first.connect()
    second.connect()
    assert first.bus_id == second.bus_id
    assert first.dev_id != second.dev_id
    first.close()
    assert len(server.devices()) == 1 and len(server.buses) == 1
    second.close()
    assert server.buses == {}


def test_failed_auto_attach_leaves_no_orphan_device(server, client):
    server.attach_error = 'exec: "usbip": executable file not found in %PATH%'
    pad = Xbox360Pad(client)
    for _ in range(3):
        with pytest.raises(ViiperError) as err:
            pad.connect()
        assert err.value.status == 409 and "usbip" in str(err.value)
    assert server.devices() == []  # nessun dispositivo orfano lasciato su VIIPER
    assert not pad.connected

    server.attach_error = None  # usbip installato: ora funziona
    pad.connect()
    assert pad.connected and len(server.devices()) == 1
    pad.close()


def test_orphan_cleanup_spares_other_devices(server, client):
    other = Xbox360Pad(client)
    other.connect()
    server.attach_error = "usbip mancante"
    with pytest.raises(ViiperError):
        Xbox360Pad(client).connect()
    assert [d.dev_id for d in server.devices()] == [other.dev_id]
    server.attach_error = None
    other.close()


def test_connect_fails_when_server_is_down():
    pad = Xbox360Pad(ViiperClient("127.0.0.1", free_port(), timeout=0.3))
    with pytest.raises(ViiperError):
        pad.connect()
    assert not pad.connected
    pad.close()  # non deve sollevare eccezioni
