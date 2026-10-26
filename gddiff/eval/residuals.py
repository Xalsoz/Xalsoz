"""Проверка гипотезы о видимости по остаткам модели.

1. Для каждой секции считаем долю смертей среди дошедших до неё попыток.
2. Объясняем её сложностью секции по таймингам (log D_s); остаток r_s это
   смерти, которых тайминги не объясняют.
3. Смотрим, объясняют ли признаки видимости этот остаток, и сравниваем с
   перестановочным тестом: если R² не выше случайного, признаки бесполезны.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..features.visibility import VIS_FEATURES


def section_outcomes(df: pd.DataFrame, deaths: pd.DataFrame, layout=None) -> pd.DataFrame:
    """Смерти и дошедшие попытки по секциям.

    deaths: колонки percent, deaths; строка percent >= 100 означает прохождения.
    Процент переводится в x по длине уровня, а без данных уровня в долю времени.
    """
    use_x = layout is not None and np.isfinite(df["x"].to_numpy(float)).all()
    coord = "x" if use_x else "t_ms"
    total = layout.length_x if use_x else float(df["t_ms"].max())
    starts = df.groupby("section")[coord].min().sort_index()
    pct = deaths["percent"].to_numpy(float)
    count = deaths["deaths"].to_numpy(float)
    completions = float(count[pct >= 100].sum())
    died = pct < 100
    idx = np.clip(np.searchsorted(starts.to_numpy(), pct[died] / 100.0 * total, side="right") - 1, 0, None)
    per = np.bincount(idx, weights=count[died], minlength=len(starts))
    reached = completions + np.cumsum(per[::-1])[::-1]
    return pd.DataFrame({"section": starts.index.to_numpy(), "deaths": per, "reached": reached})


def _r2(y: np.ndarray, A: np.ndarray) -> tuple[float, np.ndarray, np.ndarray]:
    coef, *_ = np.linalg.lstsq(A, y, rcond=None)
    resid = y - A @ coef
    ss_tot = float(np.sum((y - y.mean()) ** 2))
    return (1.0 - float(resid @ resid) / ss_tot if ss_tot > 0 else 0.0), coef, resid


def visibility_check(model, tables: dict, records, min_reached: int = 30, n_perm: int = 500, seed: int = 0) -> dict:
    """tables должны содержать колонки VIS_FEATURES (build_tables(..., with_visibility=True))."""
    by_id = {r.level_id: r for r in records}
    parts = []
    for lid, df in tables.items():
        rec = by_id.get(lid)
        if rec is None or rec.deaths is None:
            continue
        sec = model.explain(df)[["section", "log_difficulty"]]
        out = section_outcomes(df, rec.deaths, rec.layout)
        vis = df.groupby("section")[list(VIS_FEATURES)].mean()
        parts.append(sec.merge(out, on="section").merge(vis, left_on="section", right_index=True).assign(level_id=lid))
    if not parts:
        raise ValueError("нет уровней с данными о смертях (deaths/<level_id>.csv)")
    data = pd.concat(parts, ignore_index=True)
    data = data[data["reached"] >= min_reached].dropna(subset=list(VIS_FEATURES))
    if len(data) < 10:
        raise ValueError(f"мало секций с данными: {len(data)}")

    y = np.log((data["deaths"] + 0.5) / (data["reached"] - data["deaths"] + 0.5)).to_numpy()
    ones = np.ones(len(data))
    r2_timing, _, resid = _r2(y, np.column_stack([ones, data["log_difficulty"].to_numpy()]))

    V = data[list(VIS_FEATURES)].to_numpy(float)
    sd = V.std(axis=0)
    keep = sd > 0
    names = [n for n, k in zip(VIS_FEATURES, keep) if k]
    V = (V[:, keep] - V[:, keep].mean(axis=0)) / sd[keep]
    r2_vis, coef, _ = _r2(resid, np.column_stack([ones, V]))

    rng = np.random.default_rng(seed)
    perm = np.array([_r2(resid, np.column_stack([ones, V[rng.permutation(len(V))]]))[0] for _ in range(n_perm)])
    return {
        "sections": int(len(data)),
        "levels": int(data["level_id"].nunique()),
        "r2_timing": r2_timing,
        "r2_visibility_on_residuals": r2_vis,
        "r2_permutation_95": float(np.quantile(perm, 0.95)),
        "p_value": float((1 + np.sum(perm >= r2_vis)) / (1 + n_perm)),
        "coefficients": pd.Series(coef[1:], index=names, name="coef_per_sd"),
    }
