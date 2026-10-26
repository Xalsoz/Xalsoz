"""Данные уровня: строка уровня из GD или файл .gmd.

Строка уровня имеет вид "заголовок;объект;объект;...", где объект это пары
"ключ,значение" через запятую (1 - ID, 2 - x, 3 - y). С серверов она приходит
сжатой (gzip или zlib) и закодированной в URL-safe base64.

Из уровня берём порталы режимов и скорости. По ним время клика переводится в
координату x, а x в режим и скорость. Ограничение: считается, что игрок
проходит через все порталы и скорость меняют только порталы (не триггеры).
"""

from __future__ import annotations

import base64
import gzip
import re
import zlib
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from ..constants import GAMEMODE_PORTALS, HEADER_MODE, HEADER_SPEED, SPEED_PORTALS, UNITS_PER_SECOND


def decode_level_string(s: str) -> str:
    s = s.strip()
    if s.startswith(("H4sI", "eJ")):  # gzip или zlib в base64
        raw = base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))
        return zlib.decompress(raw, 15 | 32).decode("utf-8", errors="replace")
    return s


def encode_level_string(text: str) -> str:
    return base64.urlsafe_b64encode(gzip.compress(text.encode("utf-8"))).decode("ascii")


def _pairs(chunk: str) -> dict[str, str]:
    items = chunk.split(",")
    return dict(zip(items[0::2], items[1::2]))


def _int(value) -> int | None:
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


@dataclass
class LevelLayout:
    obj_id: np.ndarray
    obj_x: np.ndarray
    obj_y: np.ndarray
    start_mode: str = "cube"
    start_speed: float = 1.0

    def __post_init__(self):
        self.obj_id = np.asarray(self.obj_id, dtype=int)
        self.obj_x = np.asarray(self.obj_x, dtype=float)
        self.obj_y = np.asarray(self.obj_y, dtype=float)

        sp = np.isin(self.obj_id, list(SPEED_PORTALS))
        order = np.argsort(self.obj_x[sp], kind="stable")
        sx = self.obj_x[sp][order]
        sv = np.array([SPEED_PORTALS[i] for i in self.obj_id[sp][order]], dtype=float)
        keep = sx > 0
        self._seg_x = np.concatenate([[0.0], sx[keep]])
        self._seg_speed = np.concatenate([[self.start_speed], sv[keep]])
        self._seg_ups = np.array([UNITS_PER_SECOND[v] for v in self._seg_speed])
        seg_len = np.diff(self._seg_x)
        self._seg_t = np.concatenate([[0.0], np.cumsum(seg_len / self._seg_ups[:-1])])

        mp = np.isin(self.obj_id, list(GAMEMODE_PORTALS))
        order = np.argsort(self.obj_x[mp], kind="stable")
        self._mode_x = self.obj_x[mp][order]
        self._mode_name = np.array([GAMEMODE_PORTALS[i] for i in self.obj_id[mp][order]], dtype=object)

    @property
    def length_x(self) -> float:
        return float(self.obj_x.max()) if len(self.obj_x) else 0.0

    def x_at(self, t_s) -> np.ndarray:
        """Координата игрока через t секунд после старта."""
        t = np.asarray(t_s, dtype=float)
        idx = np.clip(np.searchsorted(self._seg_t, t, side="right") - 1, 0, None)
        return self._seg_x[idx] + (t - self._seg_t[idx]) * self._seg_ups[idx]

    def t_at(self, x) -> np.ndarray:
        """Время в секундах, когда игрок проходит координату x."""
        x = np.asarray(x, dtype=float)
        idx = np.clip(np.searchsorted(self._seg_x, x, side="right") - 1, 0, None)
        return self._seg_t[idx] + (x - self._seg_x[idx]) / self._seg_ups[idx]

    def speed_at(self, x) -> np.ndarray:
        x = np.asarray(x, dtype=float)
        idx = np.clip(np.searchsorted(self._seg_x, x, side="right") - 1, 0, None)
        return self._seg_speed[idx]

    def units_per_second_at(self, x) -> np.ndarray:
        x = np.asarray(x, dtype=float)
        idx = np.clip(np.searchsorted(self._seg_x, x, side="right") - 1, 0, None)
        return self._seg_ups[idx]

    def mode_at(self, x) -> np.ndarray:
        x = np.asarray(x, dtype=float)
        if len(self._mode_x) == 0:
            return np.full(x.shape, self.start_mode, dtype=object)
        idx = np.searchsorted(self._mode_x, x, side="right") - 1
        names = self._mode_name[np.clip(idx, 0, None)]
        return np.where(idx >= 0, names, self.start_mode).astype(object)


def parse_level(level_string: str) -> LevelLayout:
    text = decode_level_string(level_string)
    chunks = [c for c in text.split(";") if c]
    header: dict[str, str] = {}
    if chunks and chunks[0].startswith("k"):
        header = _pairs(chunks[0])
        chunks = chunks[1:]
    ids, xs, ys = [], [], []
    for chunk in chunks:
        d = _pairs(chunk)
        try:
            oid, x, y = int(d["1"]), float(d.get("2", 0.0)), float(d.get("3", 0.0))
        except (KeyError, ValueError):
            continue
        ids.append(oid)
        xs.append(x)
        ys.append(y)
    return LevelLayout(
        obj_id=np.array(ids, dtype=int),
        obj_x=np.array(xs, dtype=float),
        obj_y=np.array(ys, dtype=float),
        start_mode=HEADER_MODE.get(_int(header.get("kA2")), "cube"),
        start_speed=HEADER_SPEED.get(_int(header.get("kA4")), 1.0),
    )


_GMD_LEVEL_STRING = re.compile(r"<k>k4</k>\s*<s>([^<]*)</s>")


def load_layout(path: str | Path) -> LevelLayout:
    """Строка уровня из .txt (сырая или закодированная) или из экспорта .gmd."""
    path = Path(path)
    text = path.read_text(encoding="utf-8", errors="replace")
    if path.suffix.lower() == ".gmd" or text.lstrip().startswith("<?xml"):
        match = _GMD_LEVEL_STRING.search(text)
        if not match:
            raise ValueError(f"{path.name}: в .gmd нет строки уровня (ключ k4)")
        text = match.group(1)
    return parse_level(text)
