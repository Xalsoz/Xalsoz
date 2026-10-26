"""Разбиение таймингов уровня на секции."""

from __future__ import annotations

import numpy as np


def assign_sections(t_ms, modes=None, gap_ms: float = 1000.0) -> np.ndarray:
    """Номер секции для каждого тайминга.

    Новая секция начинается при смене режима или при паузе длиннее gap_ms.
    Тайминги должны быть отсортированы по времени.
    """
    t = np.asarray(t_ms, dtype=float)
    if len(t) == 0:
        return np.zeros(0, dtype=int)
    brk = np.zeros(len(t), dtype=bool)
    brk[1:] = np.diff(t) > gap_ms
    if modes is not None:
        m = np.asarray(modes, dtype=object)
        brk[1:] |= m[1:] != m[:-1]
    return np.cumsum(brk)
