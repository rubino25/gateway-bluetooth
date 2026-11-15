import struct

import pytest

import mapping
from protocol import InputPacket


def unpack(report: bytes):
    """(buttons, lt, rt, lx, ly, rx, ry, riservati) secondo il layout di VIIPER."""
    assert len(report) == 20
    return struct.unpack("<IBBhhhh6s", report)


def test_neutral_report_is_all_zero():
    assert mapping.to_xbox360_report(InputPacket()) == bytes(20) == mapping.NEUTRAL_REPORT


@pytest.mark.parametrize(
    ("field", "bp_mask", "x360_mask"),
    [
        ("buttons", 0x0001, 0x1000),  # A
        ("buttons", 0x0002, 0x2000),  # B
        ("buttons", 0x0004, 0x4000),  # X
        ("buttons", 0x0008, 0x8000),  # Y
        ("buttons", 0x0010, 0x0100),  # LB
        ("buttons", 0x0020, 0x0200),  # RB
        ("buttons", 0x0100, 0x0040),  # LS
        ("buttons", 0x0200, 0x0080),  # RS
        ("dpad", 0x01, 0x0001),  # su
        ("dpad", 0x02, 0x0002),  # giù
        ("dpad", 0x04, 0x0008),  # destra: in Bluepad32 è il bit 2, in XInput il bit 3
        ("dpad", 0x08, 0x0004),  # sinistra
        ("misc", 0x01, 0x0400),  # Xbox -> Guide
        ("misc", 0x02, 0x0020),  # View -> Back
        ("misc", 0x04, 0x0010),  # Menu -> Start
        ("misc", 0x08, 0x0000),  # Share: ignorato
    ],
)
def test_single_button(field, bp_mask, x360_mask):
    report = mapping.to_xbox360_report(InputPacket(**{field: bp_mask}))
    assert unpack(report)[0] == x360_mask


def test_buttons_combine():
    pkt = InputPacket(buttons=0x0001 | 0x0020, dpad=0x01 | 0x08, misc=0x04)
    assert unpack(mapping.to_xbox360_report(pkt))[0] == 0x1000 | 0x0200 | 0x0001 | 0x0004 | 0x0010


@pytest.mark.parametrize(
    ("value", "expected"),
    [(0, 0), (1, 64), (-1, -64), (511, 32704), (-512, -32768), (600, 32767), (-600, -32768)],
)
def test_scale_stick(value, expected):
    assert mapping.scale_stick(value) == expected


@pytest.mark.parametrize(
    ("value", "expected"),
    [(0, 0), (1, -64), (-1, 64), (511, -32704), (-512, 32767), (-600, 32767), (600, -32768)],
)
def test_scale_stick_inverted(value, expected):
    assert mapping.scale_stick(value, invert=True) == expected


@pytest.mark.parametrize(
    ("value", "expected"), [(0, 0), (3, 0), (4, 1), (512, 128), (1023, 255), (5000, 255)]
)
def test_scale_trigger(value, expected):
    assert mapping.scale_trigger(value) == expected


def test_report_axes_invert_y_by_default():
    pkt = InputPacket(lx=100, ly=-200, rx=-300, ry=400, lt=1023, rt=512)
    buttons, lt, rt, lx, ly, rx, ry, reserved = unpack(mapping.to_xbox360_report(pkt))
    assert (buttons, lt, rt) == (0, 255, 128)
    # Stick in alto in Bluepad32 (y negativa) -> y positiva in XInput.
    assert (lx, ly, rx, ry) == (6400, 12800, -19200, -25600)
    assert reserved == bytes(6)


def test_report_axes_without_inversion():
    pkt = InputPacket(lx=100, ly=-200, rx=-300, ry=400)
    _, _, _, lx, ly, rx, ry, _ = unpack(mapping.to_xbox360_report(pkt, invert_y=False))
    assert (lx, ly, rx, ry) == (6400, -12800, -19200, 25600)


def test_digital_trigger_fallback():
    # Grilletto premuto solo come pulsante: il valore analogico va a fondo corsa...
    pkt = InputPacket(buttons=mapping.BUTTON_TRIGGER_L | mapping.BUTTON_TRIGGER_R)
    buttons, lt, rt, *_ = unpack(mapping.to_xbox360_report(pkt))
    assert (buttons, lt, rt) == (0, 255, 255)
    # ...ma se c'è un valore analogico vince quello.
    pkt = InputPacket(buttons=mapping.BUTTON_TRIGGER_L, lt=400)
    assert unpack(mapping.to_xbox360_report(pkt))[1] == 100


def test_rumble_from_viiper():
    # VIIPER: (sinistro = motore grande, destro = motore piccolo) -> (weak, strong)
    assert mapping.rumble_from_viiper(200, 50) == (50, 200)
