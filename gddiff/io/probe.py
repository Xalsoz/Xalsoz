"""Обмен с модом gddiff Window Probe.

probe-prepare: макрос GDR JSON -> probe_input.txt (кадры при 240 тиках, только
кнопка прыжка). Мод читает этот файл из папки сохранений.

read_probe: probe_<levelID>.csv из мода -> таблица окон. Окно в тиках это все
тики, в которые клик ещё проходит: early + late + 1. Если early == early_limit
(или late == late_limit), поиск упёрся в предел, и настоящее окно может быть
шире: такие окна помечены как цензурированные.
"""

from __future__ import annotations

import re
from pathlib import Path

import numpy as np
import pandas as pd

from ..constants import TPS
from .macro import JUMP_BUTTON, Macro

PROBE_COLUMNS = ("index", "frame", "down", "player2", "early", "late", "early_limit", "late_limit", "trials", "status")


def probe_input_text(macro: Macro) -> tuple[str, list[str]]:
    """Текст probe_input.txt и список предупреждений."""
    warnings: list[str] = []
    scale = TPS / macro.fps
    if macro.fps != TPS:
        warnings.append(f"частота макроса {macro.fps:g}, кадры пересчитаны в {TPS} тиков")
    inputs = [i for i in macro.inputs if i.button == JUMP_BUTTON]
    dropped = len(macro.inputs) - len(inputs)
    if dropped:
        warnings.append(f"пропущено вводов не кнопки прыжка: {dropped} (платформер не поддерживается)")
    lines = [f"# gddiff probe input from {macro.source or 'macro'}", f"tps={TPS}"]
    for i in sorted(inputs, key=lambda x: (round(x.frame * scale), x.player2)):
        lines.append(f"{round(i.frame * scale)} {int(i.down)} {int(i.player2)}")
    return "\n".join(lines) + "\n", warnings


_HEADER = re.compile(r"(\w+)=([^\s]+)")


def read_probe(path: str | Path) -> pd.DataFrame:
    path = Path(path)
    with path.open(encoding="utf-8") as f:
        first = f.readline()
    meta = dict(_HEADER.findall(first)) if first.startswith("#") else {}
    df = pd.read_csv(path, comment="#")
    missing = set(PROBE_COLUMNS) - set(df.columns)
    if missing:
        raise ValueError(f"{path.name}: нет колонок {', '.join(sorted(missing))}")
    tps = float(meta.get("tps", TPS))
    df = df.sort_values("frame", kind="stable").reset_index(drop=True)
    df["down"] = df["down"].astype(bool)
    df["player2"] = df["player2"].astype(bool)
    ok = df["status"] == "ok"
    tick_ms = 1000.0 / tps
    df["t_ms"] = df["frame"] * tick_ms
    df["window_ticks"] = np.where(ok, df["early"] + df["late"] + 1, np.nan)
    df["window_ms"] = df["window_ticks"] * tick_ms
    df["censored"] = ok & ((df["early"] >= df["early_limit"]) | (df["late"] >= df["late_limit"]))
    df.attrs["tps"] = tps
    df.attrs["complete"] = meta.get("complete", "1") == "1"
    df.attrs["source"] = str(path)
    return df


def write_probe(path: str | Path, df: pd.DataFrame, tps: float = TPS, **meta) -> None:
    """Файл в формате мода (нужен синтетике и тестам)."""
    head = " ".join([f"tps={tps:g}"] + [f"{k}={v}" for k, v in meta.items()])
    out = df[list(PROBE_COLUMNS)].copy()
    out["down"] = out["down"].astype(int)
    out["player2"] = out["player2"].astype(int)
    with Path(path).open("w", encoding="utf-8", newline="") as f:
        f.write(f"# gddiff-probe v1 {head}\n")
        out.to_csv(f, index=False)
