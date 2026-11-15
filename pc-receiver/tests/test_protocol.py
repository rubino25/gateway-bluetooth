import pytest

import protocol
from protocol import InputPacket, ProtocolError, RumblePacket

# INPUT scritto a mano seguendo la tabella di docs/protocol.md.
RAW_INPUT = bytes.fromhex(
    "5847"  # magic 0x4758 little-endian
    "01"  # version
    "01"  # type INPUT
    "3412"  # seq 0x1234
    "02"  # pad_idx
    "01"  # flags: connected
    "0102"  # buttons 0x0201
    "08"  # dpad
    "04"  # misc
    "00fe"  # lx -512
    "ff01"  # ly 511
    "9cff"  # rx -100
    "6400"  # ry 100
    "ff03"  # lt 1023
    "0002"  # rt 512
)

RAW_RUMBLE = bytes.fromhex(
    "5847"  # magic
    "01"  # version
    "02"  # type RUMBLE
    "01"  # pad_idx
    "00"  # reserved
    "f401"  # duration_ms 500
    "40"  # weak 64
    "c8"  # strong 200
    "0000"  # reserved
)


def test_sizes():
    assert protocol.INPUT_SIZE == 24
    assert protocol.RUMBLE_SIZE == 12
    assert len(RAW_INPUT) == 24
    assert len(RAW_RUMBLE) == 12


def test_parse_input_known_bytes():
    pkt = protocol.parse_input(RAW_INPUT)
    assert pkt == InputPacket(
        seq=0x1234, pad_idx=2, flags=1, buttons=0x0201, dpad=0x08, misc=0x04,
        lx=-512, ly=511, rx=-100, ry=100, lt=1023, rt=512,
    )  # fmt: skip
    assert pkt.connected


def test_build_input_known_bytes():
    assert protocol.build_input(protocol.parse_input(RAW_INPUT)) == RAW_INPUT


def test_input_roundtrip_disconnected():
    pkt = InputPacket(seq=65535, pad_idx=3, flags=0)
    out = protocol.parse_input(protocol.build_input(pkt))
    assert out == pkt
    assert not out.connected


def test_build_input_wraps_seq():
    assert protocol.parse_input(protocol.build_input(InputPacket(seq=65536 + 7))).seq == 7


def test_parse_rumble_known_bytes():
    assert protocol.parse_rumble(RAW_RUMBLE) == RumblePacket(
        pad_idx=1, duration_ms=500, weak=64, strong=200
    )


def test_build_rumble_known_bytes():
    assert protocol.build_rumble(RumblePacket(1, 500, 64, 200)) == RAW_RUMBLE


def test_parse_header_returns_type():
    assert protocol.parse_header(RAW_INPUT) == protocol.TYPE_INPUT
    assert protocol.parse_header(RAW_RUMBLE) == protocol.TYPE_RUMBLE


@pytest.mark.parametrize(
    "data",
    [
        b"",
        b"XG",
        b"\x00\x00" + RAW_INPUT[2:],  # magic errato
        b"GX" + RAW_INPUT[2:],  # magic con i byte scambiati
        RAW_INPUT[:2] + b"\x02" + RAW_INPUT[3:],  # versione 2
        RAW_INPUT[:3] + b"\x07" + RAW_INPUT[4:],  # tipo sconosciuto
        RAW_INPUT[:-1],  # troppo corto
        RAW_INPUT + b"\x00",  # troppo lungo
        RAW_RUMBLE,  # tipo giusto ma non INPUT
        RAW_INPUT[:6] + b"\x04" + RAW_INPUT[7:],  # pad_idx 4
    ],
)
def test_parse_input_rejects(data):
    with pytest.raises(ProtocolError):
        protocol.parse_input(data)


@pytest.mark.parametrize(
    "data",
    [
        RAW_RUMBLE[:-1],
        RAW_RUMBLE + b"\x00",
        RAW_INPUT,
        b"\x00\x00" + RAW_RUMBLE[2:],
        RAW_RUMBLE[:2] + b"\x00" + RAW_RUMBLE[3:],
    ],
)
def test_parse_rumble_rejects(data):
    with pytest.raises(ProtocolError):
        protocol.parse_rumble(data)


def test_build_rejects_out_of_range():
    with pytest.raises(ProtocolError):
        protocol.build_rumble(RumblePacket(weak=256))
    with pytest.raises(ProtocolError):
        protocol.build_input(InputPacket(lx=40000))
