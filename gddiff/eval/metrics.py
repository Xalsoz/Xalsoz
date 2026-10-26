"""Метрики ранжирования: насколько порядок уровней по модели совпадает со списком."""

from __future__ import annotations

import numpy as np
from scipy.stats import kendalltau, rankdata, spearmanr


def ranking_metrics(scores, positions, k: int = 10) -> dict:
    """scores: больше = сложнее; positions: меньше = сложнее."""
    s = np.asarray(scores, dtype=float)
    pos = np.asarray(positions, dtype=float)
    n = len(s)
    k = min(k, n)
    pred_rank = rankdata(-s)
    true_rank = rankdata(pos)
    top_pred = set(np.argsort(-s, kind="stable")[:k])
    top_true = set(np.argsort(pos, kind="stable")[:k])
    return {
        "spearman": float(spearmanr(s, -pos).statistic),
        "kendall": float(kendalltau(s, -pos).statistic),
        "rank_mae": float(np.mean(np.abs(pred_rank - true_rank))),
        f"top{k}": len(top_pred & top_true) / k if k else float("nan"),
        "levels": n,
    }
