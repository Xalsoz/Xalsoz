"""Сборка таблиц таймингов по набору уровней."""

from __future__ import annotations

import sys

import pandas as pd

from .features.timing import NotEnoughMacros, timing_table
from .features.visibility import visibility_table


def build_tables(
    records,
    window_method: str = "interval",
    gap_ms: float = 1000.0,
    with_visibility: bool = False,
    smooth_events: int | None = None,
):
    """Возвращает (таблицы по level_id, позиции по level_id, пропущенные уровни)."""
    tables: dict = {}
    positions: dict = {}
    skipped: list = []
    for rec in records:
        try:
            df = timing_table(rec, window_method=window_method, gap_ms=gap_ms, smooth_events=smooth_events)
        except NotEnoughMacros:
            skipped.append(rec.level_id)
            continue
        if len(df) < 2:
            skipped.append(rec.level_id)
            continue
        if with_visibility:
            df = df.join(visibility_table(df, rec.layout))
        tables[rec.level_id] = df
        if rec.position is not None:
            positions[rec.level_id] = rec.position
    if skipped:
        print(f"пропущено уровней ({window_method}): {len(skipped)}", file=sys.stderr)
    return tables, pd.Series(positions, dtype=float), skipped
