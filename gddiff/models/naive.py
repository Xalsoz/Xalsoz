"""Наивные оценки сложности без обучения: с ними сравнивается модель."""

from __future__ import annotations

import numpy as np
import pandas as pd

NAIVE_METHODS = ("clicks_per_second", "peak_density", "tight_windows")


def naive_scores(tables: dict) -> pd.DataFrame:
    """clicks_per_second: среднее число вводов в секунду.
    peak_density: самый плотный участок уровня.
    tight_windows: -log 5-го перцентиля окна (самые узкие окна уровня).
    """
    rows = {}
    for lid, df in tables.items():
        t = df["t_ms"].to_numpy(float)
        duration = max((t.max() - t.min()) / 1000.0, 1e-9)
        rows[lid] = {
            "clicks_per_second": len(t) / duration,
            "peak_density": float(df["density"].max()),
            "tight_windows": -float(np.log(np.quantile(df["window_ms"].to_numpy(float), 0.05))),
        }
    return pd.DataFrame.from_dict(rows, orient="index")[list(NAIVE_METHODS)]
