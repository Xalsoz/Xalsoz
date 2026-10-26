import json
import shutil
import subprocess
from pathlib import Path

import numpy as np
import pandas as pd
import pytest
from scipy.special import ndtr

from gddiff.cli import main
from gddiff.constants import TICK_MS
from gddiff.eval.cv import cross_validate
from gddiff.features.refresh import frame_luck, hit_probability, level_summary
from gddiff.features.timing import timing_table
from gddiff.io.dataset import load_dataset
from gddiff.io.macro import parse_gdr_json
from gddiff.io.probe import probe_input_text, read_probe
from gddiff.models.baseline import ModelConfig
from gddiff.pipeline import build_tables
from gddiff.synth import generate

PROBE_DIR = Path(__file__).resolve().parent.parent / "probe"


def test_probe_input_converts_framerate_and_skips_other_buttons():
    m = parse_gdr_json(
        {
            "framerate": 120,
            "inputs": [
                {"frame": 10, "btn": 1, "down": True},
                {"frame": 11, "btn": 2, "down": True},
                {"frame": 20, "btn": 1, "down": False, "2p": True},
            ],
        }
    )
    text, warnings = probe_input_text(m)
    lines = [l for l in text.splitlines() if l and not l.startswith("#")]
    assert lines == ["tps=240", "20 1 0", "40 0 1"]
    assert len(warnings) == 2


def test_read_probe_window_and_censoring(tmp_path):
    p = tmp_path / "probe_1.csv"
    p.write_text(
        "# gddiff-probe v1 tps=240 max_shift=48 horizon=240 restore=checkpoint complete=0\n"
        "index,frame,down,player2,early,late,early_limit,late_limit,trials,status\n"
        "0,100,1,0,3,4,48,48,12,ok\n"
        "1,130,0,0,10,48,29,48,14,ok\n"
        "2,200,1,0,0,0,0,0,0,not_measured\n"
    )
    df = read_probe(p)
    assert df["window_ticks"].tolist()[:2] == [8, 59]
    assert np.isclose(df["window_ms"].iloc[0], 8 * TICK_MS)
    assert df["censored"].tolist() == [False, True, False]
    assert np.isnan(df["window_ms"].iloc[2])
    assert df.attrs["complete"] is False


@pytest.mark.skipif(shutil.which("g++") is None, reason="нужен g++")
def test_cpp_core_output_is_read_correctly(tmp_path):
    exe = tmp_path / "emit_csv"
    subprocess.run(
        ["g++", "-std=c++20", "-O1", "-I", "src", "-I", "tests", "-o", str(exe), "tests/emit_csv.cpp", "src/core/probe_core.cpp"],
        cwd=PROBE_DIR,
        check=True,
    )
    csv = tmp_path / "probe.csv"
    res = subprocess.run([str(exe), str(csv), "7", "60"], capture_output=True, text=True, check=True)
    truth = np.array([[int(x) for x in line.split()] for line in res.stdout.splitlines()])
    df = read_probe(csv)
    assert (df["status"] == "ok").all()
    exp_early = np.minimum(truth[:, 1], df["early_limit"].to_numpy())
    exp_late = np.minimum(truth[:, 2], df["late_limit"].to_numpy())
    assert np.array_equal(df["early"].to_numpy(), exp_early)
    assert np.array_equal(df["late"].to_numpy(), exp_late)
    assert np.array_equal(df["window_ticks"].to_numpy(), exp_early + exp_late + 1)


@pytest.mark.skipif(shutil.which("g++") is None, reason="нужен g++")
def test_cpp_solver_macro_is_read_by_python(tmp_path):
    from gddiff.io.macro import load_macro

    exe = tmp_path / "emit_solved"
    subprocess.run(
        ["g++", "-std=c++20", "-O1", "-I", "src", "-I", "tests", "-o", str(exe), "tests/emit_solved.cpp",
         "src/core/solve_core.cpp", "src/core/probe_core.cpp"],
        cwd=PROBE_DIR,
        check=True,
    )
    res = subprocess.run([str(exe), str(tmp_path), "4", "30"], capture_output=True, text=True, check=True)
    expected = [tuple(int(x) for x in line.split()) for line in res.stdout.splitlines()]
    assert len(expected) == 30
    for name in ("solved.gdr.json", "solved.txt"):
        m = load_macro(tmp_path / name)
        assert [(i.frame, int(i.down)) for i in m.inputs] == expected
        assert m.fps == 240.0
    assert load_macro(tmp_path / "solved.gdr.json").level_id == 123
    # Найденный макрос годится для замера: probe-prepare принимает его без предупреждений.
    text, warnings = probe_input_text(load_macro(tmp_path / "solved.gdr.json"))
    assert not warnings


def test_cpp_difficulty_matches_python_numbers():
    # Те же числа зашиты в probe/tests/test_difficulty.cpp (проверка C++ против Python).
    assert abs(float(hit_probability([20.8], 60, 10)[0]) - 0.6500801776185636) < 1e-12
    rows = {r["hz"]: r for r in level_summary([20.8, 4.17, 50.0, 12.5], [60.0, 144.0, 240.0, float("inf")], 10)}
    assert abs(rows["60"]["difficulty_nats"] - 3.2218568669572347) < 1e-12
    assert abs(rows["inf"]["difficulty_nats"] - 2.92686041580521) < 1e-12


def test_hit_probability_limits():
    w = np.array([2.0, 8.0, 30.0, 120.0])
    sigma = 8.0
    inf = hit_probability(w, float("inf"), sigma)
    assert np.allclose(inf, ndtr(w / 2 / sigma) - ndtr(-w / 2 / sigma))
    # Очень высокая частота почти не отличается от ввода между кадрами.
    assert np.allclose(hit_probability(w, 20000.0, sigma), inf, atol=2e-3)
    # Монотонно по окну.
    p60 = hit_probability(w, 60.0, sigma)
    assert np.all(np.diff(p60) > 0)
    # Идеально точный игрок: шанс = доля фаз сетки, при которых в окно попадает граница кадра.
    tiny = hit_probability(np.array([4.0, 8.0]), 60.0, 0.01)
    assert np.allclose(tiny, frame_luck([4.0, 8.0], 60.0), atol=0.02)


def test_level_summary_grows_with_lower_refresh_rate():
    w = np.full(200, 25.0)
    rows = {r["hz"]: r for r in level_summary(w, [60.0, 240.0, float("inf")], 8.0)}
    assert rows["60"]["difficulty_nats"] > rows["240"]["difficulty_nats"] > 0
    assert rows["inf"]["impossible_share_of_attempts"] == 0.0


@pytest.fixture(scope="module")
def synth_measured(tmp_path_factory):
    root = tmp_path_factory.mktemp("synth_measured")
    generate(root, n_levels=80, n_macros=3, seed=5)
    return root, load_dataset(root)


def test_synthetic_probe_matches_truth(synth_measured):
    root, records = synth_measured
    rec = records[0]
    df = timing_table(rec, window_method="measured")
    truth = pd.read_csv(root / "truth" / f"{rec.level_id}.csv")["window_ms"].to_numpy()
    ok = ~df["censored"].to_numpy()
    # Измерение с точностью до тика: ошибка не больше двух тиков.
    assert np.all(np.abs(df["window_ms"].to_numpy()[ok] - truth[ok]) <= 2 * TICK_MS + 1e-9)


def test_measured_windows_beat_macro_spread(synth_measured):
    _, records = synth_measured
    cfg = ModelConfig(maxiter=200)
    res = {}
    for method in ("measured", "spread"):
        tables, positions, skipped = build_tables(records, method)
        assert not skipped
        cv = cross_validate(tables, positions, cfg, folds=4)
        res[method] = cv[cv["method"] == "model"]["spearman"].mean()
    assert res["measured"] > res["spread"]
    assert res["measured"] > 0.9


def test_cli_probe_prepare_and_windows(tmp_path, synth_measured, capsys):
    root, records = synth_measured
    lid = records[0].level_id
    macro = next((root / "macros" / str(lid)).glob("*.json"))
    out = tmp_path / "probe_input.txt"
    main(["probe-prepare", "--macro", str(macro), "--out", str(out)])
    assert out.read_text().splitlines()[1] == "tps=240"
    main(["windows", "--probe", str(root / "windows" / f"{lid}.csv"), "--hz", "60", "240", "inf", "--top", "3"])
    text = capsys.readouterr().out
    assert "самых узких кликов" in text and "вклад секций" in text
