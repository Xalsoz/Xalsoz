"""Синтетический набор уровней с известной «истинной» сложностью.

Нужен, чтобы проверить код и методику до сбора реальных макросов. Генератор
знает настоящие окна допуска и параметры, а модель видит только то, что
увидела бы на реальных данных: клики ботов (.gdr.json) и строку уровня.

Что заложено:
- окна допуска по секциям, ритм связан с окном через множитель режима;
- несколько макросов на уровень, каждый кликает в случайную точку окна;
- позиция в списке = истинная сложность + шум мнений;
- часть секций «плохо видна» (триггеры прозрачности): в них больше смертей,
  но на позицию в списке это не влияет. Это проверка для анализа остатков.
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pandas as pd
from scipy.special import expit, logsumexp

from .constants import GAMEMODE_PORTALS, HEADER_SPEED, MODES, SPEED_PORTALS, TPS, UNITS_PER_SECOND
from .features.timing import FEATURE_NAMES, design_matrix, local_density
from .io.level import encode_level_string
from .io.macro import Input, Macro, to_gdr_json
from .io.probe import write_probe

TRUE_PARAMS = {
    "k": 2.0,
    "p": 4.0,
    "q": 3.0,
    "w0_ms": 100.0,
    "theta": {
        "mode_ship": 0.20,
        "mode_ball": 0.10,
        "mode_ufo": 0.10,
        "mode_wave": 0.45,
        "mode_robot": 0.00,
        "mode_spider": 0.20,
        "mode_swing": 0.30,
        "mode_unknown": 0.00,
        "log2_speed": 0.15,
        "log_density": 0.00,
        "position": 0.30,
        "release": -0.10,
    },
}

MODE_P = (0.30, 0.17, 0.08, 0.08, 0.15, 0.07, 0.07, 0.08)
SPEEDS = (0.5, 1.0, 2.0, 3.0, 4.0)
SPEED_P = (0.05, 0.45, 0.25, 0.20, 0.05)
# Во сколько раз пауза между вводами больше окна допуска, по режимам.
MODE_GAP = {"cube": 3.5, "ship": 3.0, "ball": 3.5, "ufo": 3.5, "wave": 2.6, "robot": 4.0, "spider": 3.0, "swing": 3.0}
MODE_PORTAL_ID = {v: k for k, v in GAMEMODE_PORTALS.items()}
SPEED_PORTAL_ID = {v: k for k, v in SPEED_PORTALS.items()}
SPEED_HEADER_ID = {v: k for k, v in HEADER_SPEED.items()}
FILLER_IDS = (1, 8, 39)  # блок, шип, малый шип


def _make_level(rng: np.random.Generator, lid: int, coupling: float = 0.3) -> dict:
    """coupling: насколько ритм секции следует за окном (1 = полностью, 0 = независимо)."""
    level_skill = rng.normal(0.0, 0.45)
    n_sec = int(rng.integers(4, 11))
    t, x = 0.0, 0.0
    events, sections, objects = [], [], []
    start_mode = start_speed = None
    for s in range(n_sec):
        mode = str(rng.choice(MODES, p=MODE_P))
        speed = float(rng.choice(SPEEDS, p=SPEED_P))
        ups = UNITS_PER_SECOND[speed]
        if s == 0:
            start_mode, start_speed = mode, speed
        else:
            objects.append((MODE_PORTAL_ID[mode], x, 105.0))
            objects.append((SPEED_PORTAL_ID[speed], x, 135.0))
        hidden = bool(rng.random() < 0.2)
        sec_w = float(np.exp(np.log(70.0) - level_skill + rng.normal(0.0, 0.35)))
        sec_rhythm = float(np.exp(np.log(220.0) + rng.normal(0.0, 0.4)))
        duration = rng.uniform(3000.0, 8000.0)
        t0, x0 = t, x
        tt = t + rng.uniform(250.0, 400.0)
        down = True
        while tt < t0 + duration:
            w = max(sec_w * float(np.exp(rng.normal(0.0, 0.25))), 10.0)
            events.append((tt, down, w, s, mode, speed))
            coupled = w * MODE_GAP[mode] * rng.uniform(0.8, 1.6)
            free = sec_rhythm * rng.uniform(0.8, 1.25)
            gap = float(np.exp(coupling * np.log(coupled) + (1.0 - coupling) * np.log(free)))
            tt += min(max(gap, 2.2 * w), 900.0)
            down = not down
        t_next = tt + rng.uniform(1300.0, 1800.0)
        x_next = x0 + (t_next - t0) / 1000.0 * ups
        n_fill = int(rng.uniform(1.0, 4.0) * (x_next - x0) / 30.0)
        for fx in rng.uniform(x0, x_next, n_fill):
            objects.append((int(rng.choice(FILLER_IDS)), float(fx), float(rng.uniform(15, 300))))
        if hidden:
            for ax in rng.uniform(x0, x_next, int(rng.integers(6, 16))):
                objects.append((1007, float(ax), 0.0))
        for cx in rng.uniform(x0, x_next, int(rng.integers(0, 3))):
            objects.append((int(rng.choice((1913, 1916))), float(cx), 0.0))
        sections.append({"start_x": x0, "end_x": x_next, "hidden": hidden})
        t, x = t_next, x_next
    objects.append((1, x, 15.0))  # конец уровня
    ev = pd.DataFrame(events, columns=["t_ms", "down", "window_ms", "section", "mode", "speed"])
    return {
        "level_id": lid,
        "name": f"Synthetic {lid}",
        "events": ev,
        "sections": sections,
        "objects": objects,
        "start_mode": start_mode,
        "start_speed": start_speed,
        "length_x": x,
    }


def _true_difficulty(ev: pd.DataFrame) -> tuple[np.ndarray, float]:
    df = ev.assign(
        density=local_density(ev["t_ms"].to_numpy()),
        position=ev["t_ms"] / ev["t_ms"].max(),
    )
    theta = np.array([TRUE_PARAMS["theta"][n] for n in FEATURE_NAMES])
    k, p, q = TRUE_PARAMS["k"], TRUE_PARAMS["p"], TRUE_PARAMS["q"]
    logd = k * (np.log(TRUE_PARAMS["w0_ms"]) - np.log(df["window_ms"].to_numpy())) + design_matrix(df) @ theta
    sec = df["section"].to_numpy()
    logDs = np.array([logsumexp(p * logd[sec == s]) / p for s in np.unique(sec)])
    return logDs, float(logsumexp(q * logDs) / q)


PROBE_MAX_SHIFT = 48


def _simulate_probe(frames: np.ndarray, down: np.ndarray, t_ms: np.ndarray, w_ms: np.ndarray) -> pd.DataFrame:
    """Что намерил бы мод по первому макросу: те же пределы, что в ядре C++.

    Клик на тике g происходит в момент g·TICK_MS и проходит, если этот момент
    внутри настоящего окна [t - w/2, t + w/2]. Сдвиг ограничен PROBE_MAX_SHIFT,
    соседними вводами и началом уровня (минимальный тик чекпоинта 2).
    """
    tick = 1000.0 / TPS
    frames = np.asarray(frames, dtype=int)
    true_early = np.maximum(0, frames - np.ceil((t_ms - w_ms / 2) / tick)).astype(int)
    true_late = np.maximum(0, np.floor((t_ms + w_ms / 2) / tick) - frames).astype(int)
    gap_prev = np.r_[PROBE_MAX_SHIFT + 1, np.diff(frames)]
    gap_next = np.r_[np.diff(frames), PROBE_MAX_SHIFT + 1]
    early_limit = np.clip(np.minimum.reduce([np.full(len(frames), PROBE_MAX_SHIFT), gap_prev - 1, frames - 2]), 0, None)
    late_limit = np.clip(np.minimum(PROBE_MAX_SHIFT, gap_next - 1), 0, None)
    return pd.DataFrame(
        {
            "index": np.arange(len(frames)),
            "frame": frames,
            "down": down,
            "player2": False,
            "early": np.minimum(true_early, early_limit),
            "late": np.minimum(true_late, late_limit),
            "early_limit": early_limit,
            "late_limit": late_limit,
            "trials": 0,
            "status": "ok",
        }
    )


def _level_string(lv: dict) -> str:
    header = f"kA2,{MODES.index(lv['start_mode'])},kA4,{SPEED_HEADER_ID[lv['start_speed']]}"
    objs = ";".join(f"1,{oid},2,{ox:.2f},3,{oy:.1f}" for oid, ox, oy in lv["objects"])
    return f"{header};{objs};"


def generate(
    out_dir: str | Path,
    n_levels: int = 200,
    n_macros: int = 3,
    seed: int = 0,
    label_noise: float = 0.15,
    coupling: float = 0.3,
) -> dict:
    rng = np.random.default_rng(seed)
    out = Path(out_dir)
    for sub in ("macros", "layouts", "deaths", "truth", "windows"):
        (out / sub).mkdir(parents=True, exist_ok=True)

    levels = [_make_level(rng, 1000 + i, coupling) for i in range(n_levels)]
    for lv in levels:
        lv["logDs"], lv["logD"] = _true_difficulty(lv["events"])

    scores = np.array([lv["logD"] for lv in levels])
    noisy = scores + rng.normal(0.0, label_noise * scores.std(), n_levels)
    positions = np.empty(n_levels, dtype=int)
    positions[np.argsort(-noisy)] = np.arange(1, n_levels + 1)

    all_logDs = np.concatenate([lv["logDs"] for lv in levels])
    mu, sd = all_logDs.mean(), all_logDs.std()

    rows, truth = [], []
    for lv, pos in zip(levels, positions):
        lid = lv["level_id"]
        ev = lv["events"]
        t = ev["t_ms"].to_numpy()
        w = ev["window_ms"].to_numpy()
        down = ev["down"].to_numpy(bool)
        # Истинные окна в порядке вводов макроса: для оценки потолка модели.
        ev[["t_ms", "window_ms"]].to_csv(out / "truth" / f"{lid}.csv", index=False)
        mdir = out / "macros" / str(lid)
        mdir.mkdir(exist_ok=True)
        for j in range(n_macros):
            frames = np.round((t + rng.uniform(-0.5, 0.5, len(t)) * w) * TPS / 1000.0).astype(int)
            macro = Macro([Input(int(f), bool(d)) for f, d in zip(frames, down)], float(TPS), lid, lv["name"], "gddiff-synth")
            (mdir / f"m{j}.gdr.json").write_text(json.dumps(to_gdr_json(macro)), encoding="utf-8")
            if j == 0:
                probe = _simulate_probe(frames, down, t, w)
                write_probe(out / "windows" / f"{lid}.csv", probe, max_shift=PROBE_MAX_SHIFT, restore="synthetic")

        text = _level_string(lv)
        (out / "layouts" / f"{lid}.txt").write_text(encode_level_string(text) if lid % 2 == 0 else text, encoding="utf-8")

        hidden = np.array([s["hidden"] for s in lv["sections"]], dtype=float)
        hazard = expit(-3.2 + 0.9 * (lv["logDs"] - mu) / sd + 1.3 * hidden)
        reach = 3000
        death_rows = []
        for s, h in zip(lv["sections"], hazard):
            d = int(rng.binomial(reach, h))
            reach -= d
            mid = (s["start_x"] + s["end_x"]) / 2.0
            death_rows.append({"percent": round(mid / lv["length_x"] * 100.0, 2), "deaths": d})
        death_rows.append({"percent": 100.0, "deaths": reach})
        pd.DataFrame(death_rows).to_csv(out / "deaths" / f"{lid}.csv", index=False)

        rows.append({"level_id": lid, "name": lv["name"], "position": int(pos)})
        truth.append({"level_id": lid, "true_log_difficulty": lv["logD"], "sections": len(lv["sections"]), "hidden_sections": int(hidden.sum())})

    pd.DataFrame(rows).sort_values("position").to_csv(out / "levels.csv", index=False)
    pd.DataFrame(truth).to_csv(out / "truth.csv", index=False)
    (out / "truth.json").write_text(json.dumps(TRUE_PARAMS, indent=2), encoding="utf-8")
    label_rho = float(pd.Series(scores).corr(pd.Series(-positions.astype(float)), method="spearman"))
    return {"levels": n_levels, "macros_per_level": n_macros, "coupling": coupling, "label_spearman_vs_truth": label_rho}
