"""Набор уровней на диске.

Структура папки:

    levels.csv              level_id,name,position   (position: 1 = самый сложный)
    macros/<level_id>/*.json  или  macros/<level_id>.gdr.json
    windows/<level_id>.csv  окна, измеренные модом (probe_<levelID>.csv); заменяют макросы
    layouts/<level_id>.gmd  или  layouts/<level_id>.txt      (необязательно)
    deaths/<level_id>.csv   percent,deaths; строка percent=100 = число прохождений (необязательно)
"""

from __future__ import annotations

import sys
from dataclasses import dataclass, field
from pathlib import Path

import pandas as pd

from .level import LevelLayout, load_layout
from .macro import Macro, load_macro
from .probe import read_probe


@dataclass
class LevelRecord:
    level_id: int
    name: str
    position: float | None
    macros: list[Macro]
    layout: LevelLayout | None = None
    deaths: pd.DataFrame | None = None
    probe: pd.DataFrame | None = None
    notes: list[str] = field(default_factory=list)


def _macro_paths(root: Path, lid: int) -> list[Path]:
    base = root / "macros"
    paths: list[Path] = []
    sub = base / str(lid)
    if sub.is_dir():
        paths += sorted(p for p in sub.iterdir() if p.is_file())
    paths += sorted(base.glob(f"{lid}.*")) + sorted(base.glob(f"{lid}_*"))
    return paths


def _layout_path(root: Path, lid: int) -> Path | None:
    for suffix in (".gmd", ".txt"):
        p = root / "layouts" / f"{lid}{suffix}"
        if p.is_file():
            return p
    return None


def load_dataset(root: str | Path, require_macros: bool = True) -> list[LevelRecord]:
    root = Path(root)
    levels = pd.read_csv(root / "levels.csv")
    missing = {"level_id", "position"} - set(levels.columns)
    if missing:
        raise ValueError(f"в levels.csv нет колонок: {', '.join(sorted(missing))}")

    records: list[LevelRecord] = []
    skipped: list[int] = []
    for row in levels.to_dict("records"):
        lid = int(row["level_id"])
        notes: list[str] = []
        macros = []
        for p in _macro_paths(root, lid):
            try:
                macros.append(load_macro(p))
            except ValueError as err:
                notes.append(str(err))
        probe_path = root / "windows" / f"{lid}.csv"
        probe = read_probe(probe_path) if probe_path.is_file() else None
        if not macros and probe is None and require_macros:
            skipped.append(lid)
            continue
        layout_path = _layout_path(root, lid)
        layout = load_layout(layout_path) if layout_path else None
        deaths_path = root / "deaths" / f"{lid}.csv"
        deaths = pd.read_csv(deaths_path) if deaths_path.is_file() else None
        position = row.get("position")
        records.append(
            LevelRecord(
                level_id=lid,
                name=str(row.get("name", lid)),
                position=float(position) if pd.notna(position) else None,
                macros=macros,
                layout=layout,
                deaths=deaths,
                probe=probe,
                notes=notes,
            )
        )
    if skipped:
        print(f"пропущено уровней без макросов: {len(skipped)}", file=sys.stderr)
    return records
