"""Чтение макросов (клик-логов) ботов Geometry Dash.

Поддерживаются:
- GDR JSON (.gdr.json, .json): экспорт xdBot, MH Replay и других ботов формата GDR;
- текстовый формат: строки "<кадр> <1|0> [<игрок 2: 1|0>]" и необязательная строка "fps=240".

Бинарный GDR (файл начинается с байтов "GDR") пока не читается: его нужно
выгрузить в JSON тем же ботом.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

import pandas as pd

from ..constants import TPS

JUMP_BUTTON = 1


@dataclass(frozen=True)
class Input:
    frame: int
    down: bool
    player2: bool = False
    button: int = JUMP_BUTTON


@dataclass
class Macro:
    inputs: list[Input]
    fps: float = float(TPS)
    level_id: int | None = None
    level_name: str | None = None
    bot: str | None = None
    source: str | None = None

    def events(self, buttons: tuple[int, ...] = (JUMP_BUTTON,)) -> pd.DataFrame:
        """Нажатия и отпускания в миллисекундах от начала попытки, по возрастанию времени."""
        rows = [(i.frame, i.down, i.player2) for i in self.inputs if i.button in buttons]
        df = pd.DataFrame(rows, columns=["frame", "down", "player2"])
        df = df.drop_duplicates().sort_values(["frame", "player2", "down"], kind="stable").reset_index(drop=True)
        df["t_ms"] = df["frame"].to_numpy(dtype=float) * (1000.0 / self.fps)
        df["down"] = df["down"].astype(bool)
        df["player2"] = df["player2"].astype(bool)
        return df[["t_ms", "down", "player2"]]


def _pick(d: dict, *keys, default=None):
    for k in keys:
        if k in d:
            return d[k]
    return default


def parse_gdr_json(data: dict, source: str | None = None) -> Macro:
    name = source or "макрос"
    raw = data.get("inputs")
    if not isinstance(raw, list):
        raise ValueError(f"{name}: нет списка inputs")
    inputs = []
    for n, item in enumerate(raw):
        frame = _pick(item, "frame", "f")
        down = _pick(item, "down", "d", "hold")
        if frame is None or down is None:
            raise ValueError(f"{name}: у входа #{n} нет frame или down")
        inputs.append(
            Input(
                frame=int(frame),
                down=bool(down),
                player2=bool(_pick(item, "2p", "player2", "p2", default=False)),
                button=int(_pick(item, "btn", "button", default=JUMP_BUTTON)),
            )
        )
    # В GDR сначала записаны все входы первого игрока, потом второго, поэтому сортируем.
    inputs.sort(key=lambda i: (i.frame, i.player2))
    fps = float(_pick(data, "framerate", "fps", "tps", default=TPS) or TPS)
    level = data.get("level") if isinstance(data.get("level"), dict) else {}
    bot = data.get("bot")
    bot_name = bot.get("name") if isinstance(bot, dict) else (bot if isinstance(bot, str) else None)
    level_id = level.get("id")
    return Macro(
        inputs=inputs,
        fps=fps,
        level_id=int(level_id) if level_id not in (None, "", 0) else None,
        level_name=level.get("name") or None,
        bot=bot_name,
        source=source,
    )


def parse_plaintext(text: str, source: str | None = None) -> Macro:
    fps = float(TPS)
    inputs = []
    for line_no, line in enumerate(text.splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if "=" in line:
            key, _, value = line.partition("=")
            if key.strip().lower() in ("fps", "tps", "framerate"):
                fps = float(value)
            continue
        parts = line.replace(",", " ").split()
        if len(parts) < 2:
            raise ValueError(f"{source or 'макрос'}: строка {line_no}: ожидается '<кадр> <1|0>'")
        truthy = ("1", "true", "down")
        inputs.append(Input(int(parts[0]), parts[1].lower() in truthy, len(parts) > 2 and parts[2].lower() in truthy))
    inputs.sort(key=lambda i: (i.frame, i.player2))
    return Macro(inputs=inputs, fps=fps, source=source)


def load_macro(path: str | Path) -> Macro:
    path = Path(path)
    raw = path.read_bytes()
    if raw.lstrip()[:3] == b"GDR":
        raise ValueError(f"{path.name}: бинарный GDR пока не поддерживается, выгрузи макрос в JSON (.gdr.json)")
    text = raw.decode("utf-8-sig")
    if text.lstrip().startswith("{"):
        return parse_gdr_json(json.loads(text), str(path))
    return parse_plaintext(text, str(path))


def to_gdr_json(macro: Macro) -> dict:
    return {
        "gameVersion": 2.2074,
        "version": 1.0,
        "framerate": macro.fps,
        "bot": {"name": macro.bot or "gddiff", "version": "0"},
        "level": {"id": macro.level_id or 0, "name": macro.level_name or ""},
        "inputs": [{"frame": i.frame, "btn": i.button, "2p": i.player2, "down": i.down} for i in macro.inputs],
    }
