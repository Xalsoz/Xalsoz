"""Таблица таймингов уровня и матрица признаков контекста."""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..constants import MODES, TICK_MS
from ..preprocess import assign_sections
from .windows import interval_window, spread_window

# Куб - базовый режим, его эффект входит в общий масштаб.
MODE_FEATURES = tuple(m for m in MODES if m != "cube") + ("unknown",)
FEATURE_NAMES = tuple(f"mode_{m}" for m in MODE_FEATURES) + ("log2_speed", "log_density", "position", "release")

WINDOW_METHODS = ("interval", "spread", "measured")
DEFAULT_SMOOTH = {"interval": 5, "spread": 5, "measured": 1}


class NotEnoughMacros(ValueError):
    """Для выбранного способа оценки окна у уровня не хватает данных."""


def local_density(t_ms, radius_ms: float = 500.0) -> np.ndarray:
    """Число соседних таймингов в секунду в окне ±radius_ms (t отсортировано)."""
    t = np.asarray(t_ms, dtype=float)
    lo = np.searchsorted(t, t - radius_ms, side="left")
    hi = np.searchsorted(t, t + radius_ms, side="right")
    return (hi - lo - 1) / (2 * radius_ms / 1000.0)


def smooth_windows(window: np.ndarray, sections: np.ndarray, k: int) -> np.ndarray:
    """Скользящая медиана окна по k соседним таймингам внутри секции (в логарифмах).

    Оценка окна по одному таймингу шумная, а агрегация выбирает самые узкие окна,
    то есть самые шумные оценки. Медиана по соседям это гасит, но может сгладить
    одиночный очень узкий тайминг среди лёгких: k подбирается на проверке.
    """
    if k <= 1:
        return window
    s = pd.Series(np.log(window))
    out = s.groupby(np.asarray(sections)).transform(lambda g: g.rolling(k, center=True, min_periods=1).median())
    return np.exp(out.to_numpy())


def timing_table(
    record,
    window_method: str = "interval",
    gap_ms: float = 1000.0,
    density_radius_ms: float = 500.0,
    spread_radius_ms: float = 60.0,
    w_floor_ms: float = TICK_MS,
    smooth_events: int | None = None,
) -> pd.DataFrame:
    """Одна строка на тайминг (нажатие или отпускание).

    interval, spread: события из первого макроса уровня.
    measured: события и окна из файла мода (record.probe).
    """
    if window_method not in WINDOW_METHODS:
        raise ValueError(f"window_method должен быть одним из {WINDOW_METHODS}")
    if smooth_events is None:
        smooth_events = DEFAULT_SMOOTH[window_method]
    censored = None
    if window_method == "measured":
        if record.probe is None:
            raise NotEnoughMacros(f"уровень {record.level_id}: нет файла windows/{record.level_id}.csv от мода")
        events = record.probe[["t_ms", "down", "player2"]].reset_index(drop=True)
        measured = record.probe["window_ms"].to_numpy(float)
        censored = record.probe["censored"].to_numpy(bool)
    else:
        if not record.macros:
            raise NotEnoughMacros(f"уровень {record.level_id}: нет макросов")
        events = record.macros[0].events()
    t = events["t_ms"].to_numpy(float)
    down = events["down"].to_numpy(bool)
    p2 = events["player2"].to_numpy(bool)
    n = len(t)

    layout = record.layout
    if layout is not None:
        x = layout.x_at(t / 1000.0)
        mode = layout.mode_at(x)
        speed = layout.speed_at(x)
    else:
        x = np.full(n, np.nan)
        mode = np.full(n, "unknown", dtype=object)
        speed = np.ones(n)

    w_interval = interval_window(t, p2.astype(int))
    if window_method == "measured":
        # Неизмеренные клики (пропущенные модом) получают медиану уровня.
        fill = float(np.nanmedian(measured)) if np.isfinite(measured).any() else w_floor_ms
        window = np.where(np.isfinite(measured), measured, fill)
    elif window_method == "interval":
        window = w_interval
    else:
        if len(record.macros) < 2:
            raise NotEnoughMacros(f"уровень {record.level_id}: для метода spread нужно минимум два макроса")
        spread, matched = spread_window(events, [m.events() for m in record.macros[1:]], radius_ms=spread_radius_ms)
        # Размах n равномерных точек в окне w в среднем равен w·(n-1)/(n+1):
        # без поправки уровни с разным числом макросов несравнимы.
        spread = spread * (matched + 1) / np.maximum(matched - 1, 1)
        fill = float(np.nanmedian(spread)) if np.isfinite(spread).any() else w_floor_ms
        window = np.where(np.isfinite(spread), spread, fill)
    window = np.maximum(window, w_floor_ms)

    section = assign_sections(t, mode if layout is not None else None, gap_ms)
    window = np.maximum(smooth_windows(window, section, smooth_events), w_floor_ms)

    tmax = t.max() if n else 0.0
    return pd.DataFrame(
        {
            "level_id": record.level_id,
            "t_ms": t,
            "down": down,
            "player2": p2,
            "x": x,
            "mode": mode,
            "speed": speed,
            "window_ms": window,
            "interval_ms": w_interval,
            "density": local_density(t, density_radius_ms),
            "position": t / tmax if tmax > 0 else np.zeros(n),
            "section": section,
            "censored": censored if censored is not None else np.zeros(n, dtype=bool),
        }
    )


def design_matrix(df: pd.DataFrame) -> np.ndarray:
    """Признаки контекста c_i в порядке FEATURE_NAMES. Без нормировки: шкалы уже сопоставимы."""
    mode = df["mode"].to_numpy(dtype=object)
    cols = [(mode == m).astype(float) for m in MODE_FEATURES]
    cols += [
        np.log2(df["speed"].to_numpy(float)),
        np.log1p(df["density"].to_numpy(float)),
        df["position"].to_numpy(float),
        (~df["down"].to_numpy(bool)).astype(float),
    ]
    return np.column_stack(cols)
