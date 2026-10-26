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


def test_binary_gdr_is_rejected(tmp_path):
    p = tmp_path / "x.gdr"
    p.write_bytes(b"GDR\x02\x00\x01")
    with pytest.raises(ValueError, match="бинарный"):
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
