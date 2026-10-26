"""Базовая модель сложности.

Сложность тайминга:   log d_i = log f(w_i) + θ·c_i
    форма "power":    f(w) = w0 / max(w, w_min)        (k = 1, см. ниже)
    форма "exp":      f(w) = exp(-w / τ), τ фиксировано
Сложность секции:     D_s = (Σ_i d_i^p)^(1/p)
Сложность уровня:     D   = (Σ_s D_s^q)^(1/q)
Обучение:             P(a сложнее b) = σ(β · (log D_a - log D_b))

Степенное среднее даёт сумму при p = 1 и стремится к максимуму при росте p,
поэтому вопрос «сумма или самая тяжёлая секция» решают данные.

Почему k = 1. Метки это только порядок уровней, а замена (k, θ, p, q) на
(c·k, c·θ, p/c, q/c) умножает все log D на c и порядок не меняет. Поэтому по
данным определимы только k·p, k·q и θ/k. Мы фиксируем k = 1: сложность
измеряется в единицах логарифма окна, p и q здесь это k·p и k·q исходной
формулы, θ показывает, во сколько раз признак «сужает» окно (в e^θ раз), а
общую резкость берёт на себя β. Для формы exp то же самое делает фиксированное τ.

Всё считается в логарифмах (log-sum-exp), градиент аналитический.
"""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np
import pandas as pd
from scipy.optimize import minimize
from scipy.special import expit

from ..constants import TICK_MS
from ..features.timing import FEATURE_NAMES, design_matrix

FORMS = ("power", "exp")


@dataclass
class ModelConfig:
    form: str = "power"
    w0_ms: float = 100.0
    tau_ms: float = 50.0
    w_min_ms: float = TICK_MS
    l2: float = 1e-3
    max_pairs: int = 200_000
    maxiter: int = 500
    seed: int = 0
    # Параметры построения признаков: хранятся с моделью, чтобы explain считал так же.
    window_method: str = "interval"
    gap_ms: float = 1000.0
    smooth_events: int | None = None


@dataclass
class Packed:
    level_ids: np.ndarray
    w: np.ndarray
    logw: np.ndarray
    X: np.ndarray
    ev_starts: np.ndarray  # начало каждой секции в массиве таймингов
    ev_counts: np.ndarray
    sec_starts: np.ndarray  # начало каждого уровня в массиве секций
    sec_counts: np.ndarray


def pack(tables: dict, w_min_ms: float) -> Packed:
    """Склеивает таблицы таймингов уровней в плоские массивы, сгруппированные по секциям."""
    ids, ws, Xs, ev_starts, sec_counts = [], [], [], [], []
    offset = 0
    for lid, df in tables.items():
        if len(df) == 0:
            continue
        df = df.sort_values(["section", "t_ms"], kind="stable")
        sec = df["section"].to_numpy()
        local = np.flatnonzero(np.r_[True, sec[1:] != sec[:-1]])
        ev_starts.append(local + offset)
        sec_counts.append(len(local))
        offset += len(df)
        ws.append(np.maximum(df["window_ms"].to_numpy(float), w_min_ms))
        Xs.append(design_matrix(df))
        ids.append(lid)
    if not ids:
        raise ValueError("нет уровней с таймингами")
    ev_starts_arr = np.concatenate(ev_starts)
    sec_counts_arr = np.asarray(sec_counts)
    w = np.concatenate(ws)
    return Packed(
        level_ids=np.asarray(ids),
        w=w,
        logw=np.log(w),
        X=np.vstack(Xs),
        ev_starts=ev_starts_arr,
        ev_counts=np.diff(np.append(ev_starts_arr, offset)),
        sec_starts=np.concatenate([[0], np.cumsum(sec_counts_arr)[:-1]]).astype(int),
        sec_counts=sec_counts_arr,
    )


def make_pairs(positions: np.ndarray, max_pairs: int, seed: int = 0) -> tuple[np.ndarray, np.ndarray]:
    """Пары (a, b), где уровень a сложнее b (меньше номер позиции)."""
    pos = np.asarray(positions, dtype=float)
    i, j = np.triu_indices(len(pos), 1)
    if len(i) > max_pairs:
        sel = np.random.default_rng(seed).choice(len(i), max_pairs, replace=False)
        i, j = i[sel], j[sel]
    diff = pos[i] - pos[j]
    keep = diff != 0
    i, j, diff = i[keep], j[keep], diff[keep]
    return np.where(diff < 0, i, j), np.where(diff < 0, j, i)


def _grouped_lse(vals: np.ndarray, starts: np.ndarray, counts: np.ndarray) -> np.ndarray:
    m = np.maximum.reduceat(vals, starts)
    s = np.add.reduceat(np.exp(vals - np.repeat(m, counts)), starts)
    return m + np.log(s)


class TimingModel:
    def __init__(self, config: ModelConfig | None = None):
        self.config = config or ModelConfig()
        if self.config.form not in FORMS:
            raise ValueError(f"form должна быть одной из {FORMS}")
        self.feature_names = list(FEATURE_NAMES)
        self.vector: np.ndarray | None = None
        self.reference: dict | None = None
        self.fit_info: dict = {}

    # --- параметры: [log β, log(p-1), log(q-1), θ...] ---

    def _init(self) -> np.ndarray:
        return np.zeros(3 + len(self.feature_names))

    def _bounds(self) -> list[tuple[float, float]]:
        return [(-5.0, 5.0), (-4.0, 3.5), (-4.0, 3.5)] + [(-10.0, 10.0)] * len(self.feature_names)

    # --- прямой и обратный проход ---

    def _base(self, P: Packed) -> np.ndarray:
        cfg = self.config
        if cfg.form == "power":
            return np.log(cfg.w0_ms) - P.logw
        return -P.w / cfg.tau_ms

    def _forward(self, v: np.ndarray, P: Packed) -> dict:
        p, q, theta = 1.0 + np.exp(v[1]), 1.0 + np.exp(v[2]), v[3:]
        logd = self._base(P) + P.X @ theta
        lse_s = _grouped_lse(p * logd, P.ev_starts, P.ev_counts)
        pi = np.exp(p * logd - np.repeat(lse_s, P.ev_counts))
        logDs = lse_s / p
        lse_l = _grouped_lse(q * logDs, P.sec_starts, P.sec_counts)
        rho = np.exp(q * logDs - np.repeat(lse_l, P.sec_counts))
        logD = lse_l / q
        return {"p": p, "q": q, "logd": logd, "pi": pi, "logDs": logDs, "rho": rho, "logD": logD}

    def _backward(self, v: np.ndarray, P: Packed, F: dict, gL: np.ndarray) -> np.ndarray:
        """Градиент по p, q, θ при известном dLoss/dlogD для каждого уровня (β считается отдельно)."""
        p, q = F["p"], F["q"]
        gS = np.repeat(gL, P.sec_counts) * F["rho"]  # dLoss/dlogD_s
        gE = np.repeat(gS, P.ev_counts) * F["pi"]  # dLoss/dlog d_i
        grad = np.zeros_like(v)
        grad[3:] = P.X.T @ gE
        mean_s = np.add.reduceat(F["pi"] * F["logd"], P.ev_starts)
        grad[1] = np.sum(gS * (mean_s - F["logDs"]) / p) * (p - 1.0)
        mean_l = np.add.reduceat(F["rho"] * F["logDs"], P.sec_starts)
        grad[2] = np.sum(gL * (mean_l - F["logD"]) / q) * (q - 1.0)
        return grad

    def _loss_grad(self, v: np.ndarray, P: Packed, a: np.ndarray, b: np.ndarray) -> tuple[float, np.ndarray]:
        cfg = self.config
        beta = np.exp(v[0])
        F = self._forward(v, P)
        z = beta * (F["logD"][a] - F["logD"][b])
        loss = float(np.mean(np.logaddexp(0.0, -z)))
        gz = -expit(-z) / len(a)  # dLoss/dz
        n = len(P.level_ids)
        gm = gz * beta
        gL = np.bincount(a, gm, minlength=n) - np.bincount(b, gm, minlength=n)
        grad = self._backward(v, P, F, gL)
        grad[0] = float(np.sum(gz * z))  # dLoss/dlog β
        theta = v[3:]
        loss += cfg.l2 * float(theta @ theta)
        grad[3:] += 2.0 * cfg.l2 * theta
        return loss, grad

    # --- обучение и применение ---

    def fit(self, tables: dict, positions) -> "TimingModel":
        P = pack(tables, self.config.w_min_ms)
        pos = pd.Series(positions).loc[P.level_ids].to_numpy(float)
        a, b = make_pairs(pos, self.config.max_pairs, self.config.seed)
        if len(a) == 0:
            raise ValueError("нужно минимум два уровня с разными позициями")
        res = minimize(
            self._loss_grad,
            self._init(),
            args=(P, a, b),
            jac=True,
            method="L-BFGS-B",
            bounds=self._bounds(),
            options={"maxiter": self.config.maxiter},
        )
        self.vector = res.x
        scores = self._forward(res.x, P)["logD"]
        self.reference = {"scores": scores.tolist(), "positions": pos.tolist()}
        self.fit_info = {
            "loss": float(res.fun),
            "converged": bool(res.success),
            "iterations": int(res.nit),
            "levels": int(len(P.level_ids)),
            "pairs": int(len(a)),
            "pair_accuracy": float(np.mean(scores[a] > scores[b])),
        }
        return self

    def _check(self):
        if self.vector is None:
            raise RuntimeError("модель не обучена")

    def score(self, tables: dict) -> pd.Series:
        """log D для каждого уровня: чем больше, тем сложнее."""
        self._check()
        P = pack(tables, self.config.w_min_ms)
        return pd.Series(self._forward(self.vector, P)["logD"], index=P.level_ids, name="score")

    def estimate_position(self, scores) -> np.ndarray:
        """Примерная позиция в списке по уровням, на которых модель обучалась."""
        self._check()
        ref_s = np.asarray(self.reference["scores"])
        ref_p = np.sort(np.asarray(self.reference["positions"]))
        xp = -np.sort(ref_s)[::-1]
        return np.interp(-np.asarray(scores, dtype=float), xp, ref_p)

    def timing_difficulty(self, df: pd.DataFrame) -> pd.Series:
        """log d_i для каждого тайминга уровня."""
        self._check()
        df = df.sort_values(["section", "t_ms"], kind="stable")
        P = pack({0: df}, self.config.w_min_ms)
        return pd.Series(self._forward(self.vector, P)["logd"], index=df.index, name="log_d")

    def explain(self, df: pd.DataFrame) -> pd.DataFrame:
        """Секции уровня: их сложность и вклад в сложность уровня (вклады в сумме дают 1)."""
        self._check()
        df = df.sort_values(["section", "t_ms"], kind="stable").reset_index(drop=True)
        P = pack({0: df}, self.config.w_min_ms)
        F = self._forward(self.vector, P)
        rows = []
        for s, (start, count) in enumerate(zip(P.ev_starts, P.ev_counts)):
            part = df.iloc[start : start + count]
            rows.append(
                {
                    "section": int(part["section"].iloc[0]),
                    "t_start_s": round(float(part["t_ms"].iloc[0]) / 1000.0, 2),
                    "t_end_s": round(float(part["t_ms"].iloc[-1]) / 1000.0, 2),
                    "mode": str(part["mode"].mode().iloc[0]),
                    "speed": float(part["speed"].median()),
                    "events": int(count),
                    "min_window_ms": round(float(part["window_ms"].min()), 1),
                    "log_difficulty": float(F["logDs"][s]),
                    "share": float(F["rho"][s]),
                }
            )
        return pd.DataFrame(rows)

    def params(self) -> dict:
        self._check()
        v = self.vector
        out: dict = {"form": self.config.form}
        if self.config.form == "exp":
            out["tau_ms"] = self.config.tau_ms
        out["beta"] = float(np.exp(v[0]))
        out["p"] = float(1.0 + np.exp(v[1]))
        out["q"] = float(1.0 + np.exp(v[2]))
        out["theta"] = {name: float(x) for name, x in zip(self.feature_names, v[3:])}
        if self.config.form == "power":
            # Во сколько раз признак со значением 1 эквивалентно сужает окно.
            out["window_narrowing"] = {name: float(np.exp(x)) for name, x in zip(self.feature_names, v[3:])}
        return out

    # --- сохранение ---

    def save(self, path: str | Path) -> None:
        self._check()
        payload = {
            "config": asdict(self.config),
            "feature_names": self.feature_names,
            "vector": self.vector.tolist(),
            "reference": self.reference,
            "fit_info": self.fit_info,
            "params": self.params(),
        }
        Path(path).write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")

    @classmethod
    def load(cls, path: str | Path) -> "TimingModel":
        d = json.loads(Path(path).read_text(encoding="utf-8"))
        if d["feature_names"] != list(FEATURE_NAMES):
            raise ValueError("модель обучена на другом наборе признаков")
        model = cls(ModelConfig(**d["config"]))
        model.vector = np.asarray(d["vector"], dtype=float)
        model.reference = d["reference"]
        model.fit_info = d.get("fit_info", {})
        return model
