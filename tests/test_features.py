import numpy as np
import pandas as pd

from gddiff.features.timing import FEATURE_NAMES, design_matrix, local_density
from gddiff.features.windows import interval_window, spread_window
from gddiff.preprocess import assign_sections


def _events(t, down=None):
    t = np.asarray(t, float)
    down = np.arange(len(t)) % 2 == 0 if down is None else np.asarray(down)
    return pd.DataFrame({"t_ms": t, "down": down, "player2": np.zeros(len(t), bool)})


def test_interval_window_takes_nearest_neighbour():
    w = interval_window([0, 100, 130, 400])
    assert w.tolist() == [100, 30, 30, 270]


def test_interval_window_per_group():
    w = interval_window([0, 10, 100, 110], group=[0, 1, 0, 1])
    assert w.tolist() == [100, 100, 100, 100]


def test_spread_window_recovers_jitter_range():
    ref = _events([100, 300, 500, 700])
    a = _events([104, 290, 500, 712])
    b = _events([98, 305, 495, 700])
    spread, n = spread_window(ref, [a, b], radius_ms=60)
    assert n.tolist() == [3, 3, 3, 3]
    assert np.allclose(spread, [6, 15, 5, 12])


def test_spread_window_ignores_far_or_wrong_kind_clicks():
    ref = _events([100, 300])
    other = _events([300, 500], down=[True, False])  # нажатие на 300 не совпадает с отпусканием на 300
    spread, n = spread_window(ref, [other], radius_ms=60)
    assert n.tolist() == [1, 1]
    assert np.isnan(spread).all()


def test_sections_split_on_gap_and_mode():
    sec = assign_sections([0, 100, 200, 1500, 1600], ["cube", "cube", "ship", "ship", "ship"], gap_ms=1000)
    assert sec.tolist() == [0, 0, 1, 2, 2]


def test_local_density():
    d = local_density([0, 100, 200, 2000], radius_ms=500)
    assert d.tolist() == [2.0, 2.0, 2.0, 0.0]


def test_design_matrix_columns():
    df = pd.DataFrame(
        {"mode": ["cube", "wave"], "speed": [1.0, 2.0], "density": [0.0, 3.0], "position": [0.0, 1.0], "down": [True, False]}
    )
    X = design_matrix(df)
    assert X.shape == (2, len(FEATURE_NAMES))
    row = dict(zip(FEATURE_NAMES, X[1]))
    assert row["mode_wave"] == 1 and row["log2_speed"] == 1 and row["release"] == 1
    assert not X[0, : len(FEATURE_NAMES) - 4].any()  # куб - базовый режим
