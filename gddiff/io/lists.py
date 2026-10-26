"""Позиции уровней в демонлистах.

Pointercrate: GET /api/v2/demons/listed/ с пагинацией limit/after по позиции.
AREDL: адрес API задаётся параметром url, потому что публичный API AREDL
меняется; парсер принимает список объектов с полями position, name, level_id.

Сетевые функции принимают get_json, чтобы их можно было проверять без сети.
"""

from __future__ import annotations

import json
import urllib.request
from typing import Callable

import pandas as pd
from scipy.stats import spearmanr

POINTERCRATE_URL = "https://pointercrate.com/api/v2/demons/listed/"
AREDL_URL = "https://api.aredl.net/v2/api/aredl/levels"

GetJson = Callable[[str], object]


def _http_get_json(url: str) -> object:
    req = urllib.request.Request(url, headers={"User-Agent": "gddiff/0.1 (research)"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _rows(items) -> list[dict]:
    if isinstance(items, dict):
        items = items.get("data") or items.get("levels") or []
    rows = []
    for d in items or []:
        if not isinstance(d, dict):
            continue
        position = d.get("position")
        level_id = d.get("level_id", d.get("levelId"))
        if position is None or level_id in (None, 0):
            continue
        rows.append({"level_id": int(level_id), "name": d.get("name", ""), "position": float(position)})
    return rows


def _frame(rows: list[dict], source: str) -> pd.DataFrame:
    df = pd.DataFrame(rows, columns=["level_id", "name", "position"])
    df = df.drop_duplicates("level_id").sort_values("position").reset_index(drop=True)
    df["list"] = source
    return df


def fetch_pointercrate(get_json: GetJson | None = None, limit: int = 100) -> pd.DataFrame:
    get_json = get_json or _http_get_json
    rows: list[dict] = []
    after = 0
    while True:
        page = get_json(f"{POINTERCRATE_URL}?limit={limit}&after={after}")
        batch = _rows(page)
        if not batch:
            break
        rows += batch
        after = int(max(r["position"] for r in batch))
        if len(batch) < limit:
            break
    return _frame(rows, "pointercrate")


def fetch_aredl(get_json: GetJson | None = None, url: str = AREDL_URL) -> pd.DataFrame:
    get_json = get_json or _http_get_json
    return _frame(_rows(get_json(url)), "aredl")


def list_agreement(a: pd.DataFrame, b: pd.DataFrame) -> dict:
    """Согласие двух списков на общих уровнях: потолок качества для любой модели."""
    common = a.merge(b, on="level_id", suffixes=("_a", "_b"))
    if len(common) < 3:
        return {"common_levels": len(common), "spearman": float("nan")}
    rho = spearmanr(common["position_a"], common["position_b"]).statistic
    return {"common_levels": int(len(common)), "spearman": float(rho)}
