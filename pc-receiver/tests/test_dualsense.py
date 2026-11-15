"""Protocollo v2 e DualSense virtuale: pacchetti, mappatura, sessione."""

import struct

import pytest

import mapping
import protocol
from config import Config
from protocol import InputPacket, ProtocolError, RumblePacket
from test_receiver_logic import ADDR, World

# INPUT v2 scritto a mano seguendo la tabella di docs/protocol.md.
RAW_INPUT_V2 = bytes.fromhex(
    "5847" "02" "01"  # magic, versione 2, INPUT
    "3412" "00" "07"  # seq 0x1234, pad 0, flags connected|motion|touch
    "2100" "05" "01"  # buttons A|SHOULDER_L (0x0021), dpad su|destra, misc system
    "00fe" "ff01" "0000" "0000"  # lx -512, ly 511, rx 0, ry 0
    "fc03" "0000"  # lt 1020, rt 0
    "02" "50" "02" "01"  # kind DualSense, batteria 80, in carica, clic del touchpad
    "8007" "3804" "0a00" "1400"  # dito 1 (1920, 1080), dito 2 (10, 20)
    "01" "00"  # solo dito 1 attivo, riservato
    "0010" "00f0" "0100"  # gyro 4096, -4096, 1
    "0000" "0020" "00e0"  # accel 0, 8192, -8192
    "0000"  # riservato
)

RAW_FEEDBACK_V2 = bytes.fromhex(
    "5847" "02" "02"  # magic, versione 2, FEEDBACK
    "00" "00" "f401" "40" "c8" "0000"  # pad 0, durata 500, weak 64, strong 200
    "03" "ff" "80" "00" "04" "000000"  # barra (255,128,0) e LED 0x04 validi
)


def ds_packet(**fields):
    base = dict(
        version=protocol.VERSION2,
        kind=protocol.KIND_DUALSENSE,
        flags=protocol.FLAG_CONNECTED | protocol.FLAG_MOTION | protocol.FLAG_TOUCH,
    )
    base.update(fields)
    return InputPacket(**base)


# --- protocollo ---------------------------------------------------------------


def test_v2_sizes():
    assert protocol.INPUT_V2_SIZE == len(RAW_INPUT_V2) == 52
    assert protocol.FEEDBACK_V2_SIZE == len(RAW_FEEDBACK_V2) == 20


def test_parse_input_v2_known_bytes():
    pkt = protocol.parse_input(RAW_INPUT_V2)
    assert pkt == InputPacket(
        seq=0x1234, pad_idx=0, flags=7, buttons=0x21, dpad=0x05, misc=0x01,
        lx=-512, ly=511, rx=0, ry=0, lt=1020, rt=0,
        version=2, kind=protocol.KIND_DUALSENSE, battery=80, power=protocol.POWER_CHARGING,
        ext_buttons=protocol.EXT_TOUCHPAD,
        touch=((1920, 1080, True), (10, 20, False)),
        gyro=(4096, -4096, 1), accel=(0, 8192, -8192),
    )  # fmt: skip
    assert pkt.has_motion and pkt.has_touch and pkt.touchpad_click


def test_build_input_v2_known_bytes():
    assert protocol.build_input(protocol.parse_input(RAW_INPUT_V2)) == RAW_INPUT_V2


def test_v1_still_accepted():
    pkt = protocol.parse_input(protocol.build_input(InputPacket(seq=3, buttons=1)))
    assert pkt.version == protocol.VERSION and pkt.kind == protocol.KIND_NONE


@pytest.mark.parametrize(
    "data",
    [
        RAW_INPUT_V2[:24],  # v2 con la lunghezza della v1
        RAW_INPUT_V2 + b"\x00",
        RAW_INPUT_V2[:2] + b"\x03" + RAW_INPUT_V2[3:],  # versione 3
    ],
)
def test_parse_input_v2_rejects(data):
    with pytest.raises(ProtocolError):
        protocol.parse_input(data)


def test_parse_feedback_v2_known_bytes():
    assert protocol.parse_rumble(RAW_FEEDBACK_V2) == RumblePacket(
        0, 500, 64, 200, version=2, lightbar=(255, 128, 0), player_leds=0x04
    )


def test_build_feedback_v2_roundtrip():
    pkt = RumblePacket(1, 0, 0, 0, version=2, lightbar=None, player_leds=0x1F)
    data = protocol.build_rumble(pkt)
    assert len(data) == 20 and data[12] == protocol.FB_PLAYER_LEDS
    assert protocol.parse_rumble(data) == pkt
    assert protocol.build_rumble(protocol.parse_rumble(RAW_FEEDBACK_V2)) == RAW_FEEDBACK_V2


def test_feedback_v1_has_no_lights():
    data = protocol.build_rumble(RumblePacket(0, 500, 1, 2))
    assert len(data) == 12
    assert protocol.parse_rumble(data).lightbar is None


# --- mappatura ----------------------------------------------------------------


def unpack_ds(state):
    return struct.unpack("<4bIBBBHHBHHB3h3h", state)


def test_dualsense_state_layout():
    pkt = ds_packet(
        buttons=mapping.BUTTON_A | mapping.BUTTON_Y | mapping.BUTTON_TRIGGER_L | mapping.BUTTON_THUMB_R,
        misc=mapping.MISC_BUTTON_SYSTEM | mapping.MISC_BUTTON_SELECT | mapping.MISC_BUTTON_CAPTURE,
        dpad=mapping.DPAD_RIGHT | mapping.DPAD_DOWN,
        ext_buttons=protocol.EXT_TOUCHPAD,
        lx=-512, ly=511, rx=100, ry=-100, lt=1023, rt=4,
        touch=((1000, 500, True), (5000, 5000, False)),
        gyro=(1, -2, 3), accel=(-4, 5, -6),
    )  # fmt: skip
    state = mapping.to_dualsense_state(pkt)
    assert len(state) == mapping.DS_STATE_SIZE == 33
    (lx, ly, rx, ry, buttons, dpad, l2, r2, t1x, t1y, t1a, t2x, t2y, t2a,
     gx, gy, gz, ax, ay, az) = unpack_ds(state)  # fmt: skip
    assert (lx, ly, rx, ry) == (-128, 127, 25, -25)  # niente inversione della Y
    assert buttons == (mapping.DS_CROSS | mapping.DS_TRIANGLE | mapping.DS_L2 | mapping.DS_R3
                       | mapping.DS_PS | mapping.DS_CREATE | mapping.DS_MIC_MUTE
                       | mapping.DS_TOUCHPAD)  # fmt: skip
    assert dpad == mapping.DS_DPAD_RIGHT | mapping.DS_DPAD_DOWN
    assert (l2, r2) == (255, 1)
    assert (t1x, t1y, t1a) == (1000, 500, 1)
    assert (t2x, t2y, t2a) == (1920, 1080, 0)  # limitato all'area del touchpad
    assert (gx, gy, gz, ax, ay, az) == (1, -2, 3, -4, 5, -6)


def test_dualsense_face_buttons():
    for src, dst in ((mapping.BUTTON_A, mapping.DS_CROSS), (mapping.BUTTON_B, mapping.DS_CIRCLE),
                     (mapping.BUTTON_X, mapping.DS_SQUARE), (mapping.BUTTON_Y, mapping.DS_TRIANGLE)):  # fmt: skip
        assert unpack_ds(mapping.to_dualsense_state(ds_packet(buttons=src)))[4] == dst


def test_dualsense_without_motion_uses_resting_accel():
    pkt = ds_packet(flags=protocol.FLAG_CONNECTED, gyro=(9, 9, 9), accel=(9, 9, 9),
                    touch=((5, 5, True), (0, 0, False)))  # fmt: skip
    fields = unpack_ds(mapping.to_dualsense_state(pkt))
    assert fields[8:14] == (0, 0, 0, 0, 0, 0)  # touch ignorato senza FLAG_TOUCH
    assert fields[14:] == (0, 0, 0) + mapping.DS_DEFAULT_ACCEL


def test_dualsense_neutral_state():
    assert mapping.DS_NEUTRAL_STATE == mapping.to_dualsense_state(
        InputPacket(version=2, kind=protocol.KIND_DUALSENSE)
    )


def test_feedback_from_dualsense():
    fb = mapping.Feedback.from_dualsense(bytes((10, 20, 1, 2, 3, 0x0A)))
    assert fb == mapping.Feedback(weak=10, strong=20, lightbar=(1, 2, 3), player_leds=0x0A)
    # Zero = "non impostato" (VIIPER azzera i campi non toccati dal gioco).
    assert mapping.Feedback.from_dualsense(bytes(6)) == mapping.Feedback()


@pytest.mark.parametrize(
    "battery,power,expected",
    [
        (80, protocol.POWER_DISCHARGING, 0x08),
        (45, protocol.POWER_CHARGING, 0x15),
        (100, protocol.POWER_FULL, 0x2A),
        (protocol.BATTERY_UNKNOWN, protocol.POWER_UNKNOWN, None),
    ],
)
def test_battery_status_byte(battery, power, expected):
    assert mapping.battery_status_byte(ds_packet(battery=battery, power=power)) == expected


def test_device_type_choice():
    assert mapping.device_type_for(ds_packet(), "dualsense") == "dualsense"
    assert mapping.device_type_for(ds_packet(), "xbox360") == "xbox360"
    xbox = InputPacket(version=2, kind=protocol.KIND_XBOX)
    assert mapping.device_type_for(xbox, "dualsense") == "xbox360"
    assert mapping.device_type_for(InputPacket(), "dualsense") == "xbox360"  # v1


# --- sessione -----------------------------------------------------------------


def ds_input(world, now, **fields):
    world.seq += 1
    return world.session.on_input(ds_packet(seq=world.seq, **fields), ADDR, now)


def test_dualsense_creates_dualsense_pad_with_battery():
    w = World()
    ds_input(w, 0.0, buttons=mapping.BUTTON_B, battery=50, power=protocol.POWER_CHARGING)
    assert w.pad.device_type == "dualsense"
    assert w.pad.meta == {"battery_status": 0x15}
    assert w.pad.reports == [mapping.to_dualsense_state(
        ds_packet(buttons=mapping.BUTTON_B, battery=50, power=protocol.POWER_CHARGING))]  # fmt: skip


def test_dualsense_as_xbox360_when_configured():
    w = World(dualsense_device="xbox360")
    ds_input(w, 0.0, buttons=mapping.BUTTON_A)
    assert w.pad.device_type == "xbox360"
    assert w.pad.reports == [mapping.to_xbox360_report(ds_packet(buttons=mapping.BUTTON_A))]


def test_controller_change_replaces_virtual_pad():
    w = World()
    w.input(0.0, version=2, kind=protocol.KIND_XBOX)
    assert w.pad.device_type == "xbox360"
    ds_input(w, 0.1)
    assert len(w.pads) == 2 and w.pads[0].closed
    assert w.pad.device_type == "dualsense" and not w.pad.closed


def test_release_sends_dualsense_neutral():
    w = World()
    ds_input(w, 0.0, buttons=mapping.BUTTON_A)
    w.session.on_tick(1.0)
    assert w.pad.reports[-1] == mapping.DS_NEUTRAL_STATE


def test_lights_forwarded_as_feedback_v2():
    w = World()
    ds_input(w, 0.0)
    w.pad.rumble_queue = [mapping.Feedback(10, 20, (255, 0, 0), 0x04)]
    w.session.on_viiper_readable(0.01)
    assert w.rumbles[-1] == (ADDR, RumblePacket(0, 500, 10, 20, version=2,
                                                lightbar=(255, 0, 0), player_leds=0x04))  # fmt: skip


def test_lights_sticky_and_change_triggers_send():
    w = World()
    ds_input(w, 0.0)
    w.pad.rumble_queue = [mapping.Feedback(0, 0, (0, 0, 255), None)]
    w.session.on_viiper_readable(0.01)
    sent = len(w.rumbles)
    # Report del gioco che non tocca le luci: niente di nuovo da inviare.
    w.pad.rumble_queue = [mapping.Feedback(0, 0, None, None)]
    w.session.on_viiper_readable(0.02)
    assert len(w.rumbles) == sent
    # Il segnale di presenza porta comunque l'ultimo colore.
    for i in range(1, 15):
        ds_input(w, i * 0.1)
        w.session.on_tick(i * 0.1)
    assert w.rumbles[-1][1] == RumblePacket(0, 0, 0, 0, version=2, lightbar=(0, 0, 255))


def test_v1_input_gets_v1_rumble():
    w = World()
    w.input(0.0)
    w.pad.rumble_queue = [(200, 50)]
    w.session.on_viiper_readable(0.01)
    assert w.rumbles[-1][1].version == protocol.VERSION


def test_config_rejects_bad_dualsense_value(tmp_path):
    from config import load_config

    path = tmp_path / "config.toml"
    path.write_text('[mapping]\ndualsense = "ps4"\n', encoding="utf-8")
    with pytest.raises(ValueError, match="dualsense"):
        load_config(path)
    assert Config().dualsense_device == "dualsense"
