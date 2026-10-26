"""Признаки видимости по данным уровня.

В основную модель не входят: ими проверяют остатки модели (см. eval.residuals).
Это первые грубые признаки; контраст, перекрытие декором и время видимости
конкретной опасности требуют разбора объектов глубже и добавятся позже.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..constants import CAMERA_TRIGGERS, NON_GEOMETRY, VISIBILITY_TRIGGERS

VIS_FEATURES = ("objects_ahead", "lookahead_ms", "camera_triggers", "alpha_triggers")


def _count_between(sorted_x: np.ndarray, a: np.ndarray, b: np.ndarray) -> np.ndarray:
    return np.searchsorted(sorted_x, b, side="right") - np.searchsorted(sorted_x, a, side="left")


def visibility_table(
    df: pd.DataFrame,
    layout,
    lookahead_units: float = 420.0,
    lookback_units: float = 600.0,
) -> pd.DataFrame:
    """Признаки для каждого тайминга.

    objects_ahead: объекты геометрии в полосе lookahead_units впереди игрока.
    lookahead_ms: сколько времени объект с края этой полосы летит до игрока.
    camera_triggers, alpha_triggers: триггеры камеры и прозрачности позади
    игрока в пределах lookback_units, то есть недавно сработавшие.
    """
    out = pd.DataFrame(index=df.index, columns=list(VIS_FEATURES), dtype=float)
    if layout is None:
        return out
    x = df["x"].to_numpy(float)
    ids, xs = layout.obj_id, layout.obj_x
    geometry = np.sort(xs[~np.isin(ids, list(NON_GEOMETRY))])
    camera = np.sort(xs[np.isin(ids, list(CAMERA_TRIGGERS))])
    alpha = np.sort(xs[np.isin(ids, list(VISIBILITY_TRIGGERS))])
    out["objects_ahead"] = _count_between(geometry, x, x + lookahead_units)
    out["lookahead_ms"] = lookahead_units / layout.units_per_second_at(x) * 1000.0
    out["camera_triggers"] = _count_between(camera, x - lookback_units, x)
    out["alpha_triggers"] = _count_between(alpha, x - lookback_units, x)
    return out


def section_visibility(df: pd.DataFrame) -> pd.DataFrame:
    """Средние признаки видимости по секциям (df уже содержит колонки VIS_FEATURES)."""
    return df.groupby("section")[list(VIS_FEATURES)].mean()
