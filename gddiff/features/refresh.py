"""Сложность измеренных окон для игрока с данной частотой кадров.

Модель. Окно клика это отрезок времени длиной W = (early + late + 1) тиков.
Игра принимает нажатие на границе ближайшего следующего кадра: при частоте f
кадр длится D = 1000/f мс, и сетка кадров лежит случайно относительно окна.
В среднем это запаздывание D/2, и игрок к нему привыкает: целится на D/2
раньше середины окна и ошибается на нормальную величину с разбросом sigma.
Клик проходит, если граница кадра, на которой игра его приняла, попала в окно.

При W < D бывают попытки, где в окно не попадает ни одна граница кадра:
пройти такой клик невозможно, как бы точно игрок ни нажал. При f -> inf
(ввод между кадрами) остаётся только точность игрока.

Сложность уровня = сумма -log P по кликам: логарифм числа попыток, которое
в среднем нужно на прохождение без смертей, если клики независимы. Модель
грубая: sigma одна на все клики, маршрут считается выученным.

Насколько известно, обычная GD 2.2 принимает клики раз в кадр, а моды вроде
Click Between Frames и, судя по привязкам, сама игра с версии 2.208 умеют
учитывать время нажатия внутри кадра. Поэтому частота задаётся явно.
"""

from __future__ import annotations

import numpy as np
from scipy.special import ndtr

from ..constants import TICK_MS


def hit_probability(window_ms, hz: float, sigma_ms: float, phases: int = 64) -> np.ndarray:
    """Вероятность пройти клик с окном window_ms (массив) при частоте hz."""
    w = np.atleast_1d(np.asarray(window_ms, dtype=float))
    half = w / 2.0
    if not np.isfinite(hz):
        return ndtr(half / sigma_ms) - ndtr(-half / sigma_ms)
    frame = 1000.0 / hz
    # Окно [-half, half]; границы кадров g = phi + k*frame. Нажатие в момент t
    # принимается на ближайшей границе g >= t, то есть t в (g - frame, g].
    # Игрок целится в -frame/2: t ~ N(-frame/2, sigma).
    phi = (np.arange(phases) + 0.5) / phases * frame
    max_k = int(np.ceil(np.nanmax(w) / frame)) + 2 if np.isfinite(w).any() else 2
    k = np.arange(-max_k, max_k + 1)
    g = phi[:, None] + k[None, :] * frame  # (phases, K)
    inside = (g[None, :, :] > -half[:, None, None]) & (g[None, :, :] <= half[:, None, None])
    aim = -frame / 2.0
    mass = ndtr((g - aim) / sigma_ms) - ndtr((g - frame - aim) / sigma_ms)  # (phases, K)
    p = (inside * mass[None, :, :]).sum(axis=2).mean(axis=1)
    return np.where(np.isfinite(w), p, np.nan)


def frame_luck(window_ms, hz: float) -> np.ndarray:
    """Доля попыток, где в окно попадает хотя бы одна граница кадра."""
    w = np.asarray(window_ms, dtype=float)
    if not np.isfinite(hz):
        return np.ones_like(w)
    return np.minimum(1.0, w * hz / 1000.0)


def level_summary(window_ms, hz_list, sigma_ms: float) -> list[dict]:
    """Шанс пройти все клики с одной попытки и ожидаемое число попыток."""
    w = np.asarray(window_ms, dtype=float)
    w = w[np.isfinite(w)]
    rows = []
    for hz in hz_list:
        p = np.clip(hit_probability(w, hz, sigma_ms), 1e-300, 1.0)
        nll = float(-np.log(p).sum())
        rows.append(
            {
                "hz": "inf" if not np.isfinite(hz) else f"{hz:g}",
                "difficulty_nats": nll,
                "log10_attempts": nll / np.log(10.0),
                "impossible_share_of_attempts": float(1.0 - np.prod(frame_luck(w, hz))),
                "worst_click_p": float(p.min()) if len(p) else float("nan"),
            }
        )
    return rows


def window_ticks_to_ms(ticks) -> np.ndarray:
    return np.asarray(ticks, dtype=float) * TICK_MS
