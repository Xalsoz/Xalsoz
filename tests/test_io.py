import json

import numpy as np
import pytest

from gddiff.constants import UNITS_PER_SECOND
from gddiff.io.level import decode_level_string, encode_level_string, load_layout, parse_level
from gddiff.io.macro import load_macro, parse_gdr_json, parse_plaintext


def test_gdr_json_sorts_p2_inputs_and_reads_fps():
    data = {
        "framerate": 240,
        "level": {"id": 42, "name": "Test"},
        "inputs": [
            {"frame": 10, "btn": 1, "2p": False, "down": True},
            {"frame": 30, "btn": 1, "2p": False, "down": False},
            {"frame": 20, "btn": 1, "2p": True, "down": True},
        ],
    }
    m = parse_gdr_json(data)
    assert [i.frame for i in m.inputs] == [10, 20, 30]
    assert m.level_id == 42
    ev = m.events()
    assert np.allclose(ev["t_ms"], [10 / 240 * 1000, 20 / 240 * 1000, 30 / 240 * 1000])
    assert ev["player2"].tolist() == [False, True, False]


def test_events_skip_non_jump_buttons():
    m = parse_gdr_json({"inputs": [{"frame": 1, "btn": 2, "down": True}, {"frame": 2, "btn": 1, "down": True}]})
    assert len(m.events()) == 1


def test_plaintext_macro():
    m = parse_plaintext("fps=120\n# comment\n10 1\n20 0\n15 1 1\n")
    assert m.fps == 120
    assert [(i.frame, i.down, i.player2) for i in m.inputs] == [(10, True, False), (15, True, True), (20, False, False)]


# Файл записан настоящей библиотекой maxnut/GDReplayFormat (gdr.hpp) с расширениями xdBot-типа:
# уровень GRIEF (id 5000000), два игрока, расширение на каждом входе, смерти, большие кадры.
GDR2_FROM_LIBRARY = bytes.fromhex(
    "474452020c50687973696373496e707574026d65016441480000c1ac01406e0000000000008906020000057864426f7403c096b10205475249454608b9600568656c6c6f026496010a0801100000000040000000400a0000000000000a1040f0000040000000400a000000000000cf041043e1000040000000400a0000000000000a1043e4c00040000000400a000000000000ffc0081047cd140040000000400a000000000000141047cd1b8040000000400a000000000000031047cd1c4040000000400a000000000000d2071047ce8b0040000000400a00000000000051104270000040000000400a00000000000064104307000040000000400a000000000000"
)


def test_gdr2_written_by_the_real_library(tmp_path):
    p = tmp_path / "x.gdr2"
    p.write_bytes(GDR2_FROM_LIBRARY)
    m = load_macro(p)
    assert (m.level_id, m.level_name, m.bot, m.fps) == (5000000, "GRIEF", "xdBot", 240.0)
    got = [(i.frame, i.down, i.player2) for i in m.inputs]
    assert got == [
        (0, True, False), (5, False, False), (40, True, True), (90, False, True),
        (300, True, False), (305, False, False), (70000, True, False),
        (70010, False, False), (70011, True, False), (70500, False, False),
    ]


def test_gdr2_roundtrip_and_errors(tmp_path):
    from gddiff.io.macro import Input, Macro, to_gdr2

    m = Macro(inputs=[Input(3, True), Input(9, False), Input(5, True, True), Input(200000, True)], fps=240.0)
    for tag in ("", "PhysicsInput"):
        p = tmp_path / "y.gdr2"
        p.write_bytes(to_gdr2(m, tag=tag))
        back = load_macro(p)
        assert [(i.frame, i.down, i.player2) for i in back.inputs] == [
            (3, True, False), (5, True, True), (9, False, False), (200000, True, False)
        ]
    p.write_bytes(to_gdr2(m)[:20])
    with pytest.raises(ValueError, match="оборван"):
        load_macro(p)
    p.write_bytes(b"GDR\x01\x00")
    with pytest.raises(ValueError, match="версия"):
        load_macro(p)


def test_load_json_file(tmp_path):
    p = tmp_path / "m.gdr.json"
    p.write_text(json.dumps({"inputs": [{"frame": 5, "down": True}]}))
    assert load_macro(p).inputs[0].frame == 5


def test_level_string_roundtrip():
    text = "kA2,0,kA4,0;1,1,2,15,3,15;"
    enc = encode_level_string(text)
    assert enc.startswith("H4sI")
    assert decode_level_string(enc) == text


def test_layout_time_position_and_portals():
    # Старт: корабль на 1x. Портал 2x на x=300, портал волны на x=600.
    text = "kA2,1,kA4,0;1,202,2,300,3,105;1,660,2,600,3,105;1,1,2,900,3,15;"
    layout = parse_level(encode_level_string(text))
    v1, v2 = UNITS_PER_SECOND[1.0], UNITS_PER_SECOND[2.0]
    t_portal = 300 / v1
    assert np.isclose(layout.x_at(t_portal), 300)
    assert np.isclose(layout.x_at(t_portal + 1.0), 300 + v2)
    assert np.isclose(layout.t_at(300 + v2), t_portal + 1.0)
    assert list(layout.mode_at([100, 650])) == ["ship", "wave"]
    assert list(layout.speed_at([100, 650])) == [1.0, 2.0]
    assert layout.length_x == 900


def test_gmd_file(tmp_path):
    enc = encode_level_string("kA2,4,kA4,1;1,1,2,15,3,15;")
    p = tmp_path / "lvl.gmd"
    p.write_text(f'<?xml version="1.0"?><plist><dict><k>k2</k><s>Name</s><k>k4</k><s>{enc}</s></dict></plist>')
    layout = load_layout(p)
    assert layout.start_mode == "wave" and layout.start_speed == 0.5


GDR1_MSGPACK = bytes.fromhex(
    "8dab67616d6556657273696f6ecb4001a8c154c985f0ab6465736372697074696f6ea0a776657273696f6ecb"
    "3ff0000000000000a86475726174696f6ecb400c000000000000a3626f7482a46e616d65a57864426f74a776"
    "657273696f6ea776322e332e3131a56c6576656c82a26964ce004c4b40a46e616d65a54752494546a6617574"
    "686f72a26d65a47365656400a5636f696e7300a36c646dc2a96672616d6572617465cb406e000000000000a6"
    "696e707574739684a56672616d6564a362746e01a23270c2a4646f776ec384a56672616d65cc82a362746e01"
    "a23270c2a4646f776ec284a56672616d65cd012ca362746e01a23270c3a4646f776ec384a56672616d65cd01"
    "36a362746e02a23270c2a4646f776ec384a56672616d65ce00011170a362746e01a23270c2a4646f776ec384"
    "a56672616d65ce0001117aa362746e01a23270c2a4646f776ec2aa6672616d6546697865739182a56672616d"
    "6505a2703182a178cb3ff8000000000000a179cb4004000000000000"
)


def test_gdr1_messagepack_written_by_the_msgpack_library(tmp_path):
    p = tmp_path / "grief.gdr"
    p.write_bytes(GDR1_MSGPACK)
    m = load_macro(p)
    assert (m.level_id, m.level_name, m.bot) == (5000000, "GRIEF", "xdBot")
    assert [(i.frame, i.down, i.player2) for i in m.inputs if i.button == 1] == [
        (100, True, False), (130, False, False), (300, True, True), (70000, True, False), (70010, False, False)
    ]
    ev = m.events()
    assert len(ev) == 5  # кнопка 2 не считается прыжком
    with pytest.raises(ValueError):
        load_macro_bytes = tmp_path / "cut.gdr"
        load_macro_bytes.write_bytes(GDR1_MSGPACK[:200])
        load_macro(load_macro_bytes)


def test_xd_file(tmp_path):
    p = tmp_path / "m.xd"
    p.write_text("60\n10|1|1|0|0|0\n10|0|0|0|1|1.5|2.5\n13|0|1|0|0|0\n20|1|1|1|0|0\n", encoding="utf-8")
    m = load_macro(p)
    assert [(i.frame, i.down, i.player2) for i in m.inputs] == [(40, True, False), (52, False, False), (80, True, True)]
    a = tmp_path / "a.xd"
    a.write_text("android\n5|1|1|0|0\n", encoding="utf-8")
    assert load_macro(a).inputs[0].frame == 20
    bad = tmp_path / "b.xd"
    bad.write_text("60\nabc|1|1|0|0\n", encoding="utf-8")
    with pytest.raises(ValueError):
        load_macro(bad)
