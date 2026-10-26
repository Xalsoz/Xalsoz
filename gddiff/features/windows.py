"""Оценка окна допуска для каждого тайминга.

Из одного макроса окно не видно: бот кликает в одну точку. Поэтому есть два
способа оценки.

interval: время до ближайшего соседнего ввода того же игрока. Это прокси:
плотный ритм обычно означает узкие окна, но связь не точная и зависит от режима.

spread: размах времени одного и того же клика в нескольких макросах. Разные
авторы макросов попадают в разные точки окна, поэтому размах это нижняя
оценка ширины окна. Нужно минимум два макроса уровня.
"""

from __future__ import annotations

import numpy as np
import pandas as pd


def interval_window(t_ms, group=None, cap_ms: float = 1000.0) -> np.ndarray:
    """Время до ближайшего соседнего события в той же группе (t отсортировано)."""
    t = np.asarray(t_ms, dtype=float)
    n = len(t)
    w = np.full(n, cap_ms)
    groups = np.zeros(n, dtype=int) if group is None else np.asarray(group)
    for g in np.unique(groups):
        idx = np.flatnonzero(groups == g)
        if len(idx) < 2:
            continue
        gaps = np.diff(t[idx])
        prev = np.concatenate([[np.inf], gaps])
        nxt = np.concatenate([gaps, [np.inf]])
        w[idx] = np.minimum(np.minimum(prev, nxt), cap_ms)
    return w


def _kind(events: pd.DataFrame) -> np.ndarray:
    return events["down"].to_numpy(bool).astype(int) * 2 + events["player2"].to_numpy(bool).astype(int)


def spread_window(
    ref: pd.DataFrame,
    others: list[pd.DataFrame],
    radius_ms: float = 60.0,
    min_matches: int = 2,
) -> tuple[np.ndarray, np.ndarray]:
    """Размах времени каждого клика опорного макроса по всем макросам.

    Клик другого макроса сопоставляется, если он того же вида (нажатие или
    отпускание, тот же игрок) и ближе, чем radius_ms и половина расстояния до
    соседнего клика того же вида. Возвращает (размах в мс или NaN, число совпадений).
    """
    t = ref["t_ms"].to_numpy(float)
    kind = _kind(ref)
    radius = np.minimum(radius_ms, interval_window(t, kind, cap_ms=np.inf) / 2)
    lo, hi = t.copy(), t.copy()
    matched = np.ones(len(t), dtype=int)
    for other in others:
        ot = other["t_ms"].to_numpy(float)
        okind = _kind(other)
        for k in np.unique(kind):
            ri = np.flatnonzero(kind == k)
            cand = ot[okind == k]
            if len(cand) == 0:
                continue
            pos = np.searchsorted(cand, t[ri])
            left = cand[np.clip(pos - 1, 0, len(cand) - 1)]
            right = cand[np.clip(pos, 0, len(cand) - 1)]
            near = np.where(np.abs(left - t[ri]) <= np.abs(right - t[ri]), left, right)
            ok = np.abs(near - t[ri]) <= radius[ri]
            sel = ri[ok]
            lo[sel] = np.minimum(lo[sel], near[ok])
            hi[sel] = np.maximum(hi[sel], near[ok])
            matched[sel] += 1
    spread = np.where(matched >= min_matches, hi - lo, np.nan)
    return spread, matched
