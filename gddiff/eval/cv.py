"""Кросс-валидация по уровням: уровень целиком либо в обучении, либо в проверке."""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..models.baseline import ModelConfig, TimingModel
from ..models.naive import NAIVE_METHODS, naive_scores
from .metrics import ranking_metrics


def cross_validate(tables: dict, positions: pd.Series, config: ModelConfig, folds: int = 5, seed: int = 0) -> pd.DataFrame:
    ids = np.array([lid for lid in tables if lid in positions.index])
    if len(ids) < 2 * folds:
        raise ValueError(f"мало уровней для {folds} фолдов: {len(ids)}")
    fold_of = np.random.default_rng(seed).permutation(len(ids)) % folds
    rows = []
    for f in range(folds):
        train, test = ids[fold_of != f], ids[fold_of == f]
        model = TimingModel(config).fit({i: tables[i] for i in train}, positions.loc[train])
        test_tables = {i: tables[i] for i in test}
        pos = positions.loc[test].to_numpy(float)
        scores = model.score(test_tables).loc[test].to_numpy()
        rows.append({"fold": f, "method": "model", **ranking_metrics(scores, pos)})
        naive = naive_scores(test_tables).loc[test]
        for method in NAIVE_METHODS:
            rows.append({"fold": f, "method": method, **ranking_metrics(naive[method].to_numpy(), pos)})
    return pd.DataFrame(rows)


def summarize(cv: pd.DataFrame) -> pd.DataFrame:
    metrics = [c for c in cv.columns if c not in ("fold", "method", "levels")]
    summary = cv.groupby("method")[metrics].agg(["mean", "std"])
    return summary.sort_values(("spearman", "mean"), ascending=False)
