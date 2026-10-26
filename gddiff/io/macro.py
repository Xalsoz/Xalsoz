"""Чтение макросов (клик-логов) ботов Geometry Dash.

Поддерживаются:
- GDR JSON (.gdr.json, .json): экспорт xdBot, MH Replay и других ботов формата GDR;
- GDR 1 (.gdr): тот же JSON, упакованный в MessagePack (читается встроенным разбором, без внешних библиотек);
- xdBot .xd: первая строка частота или "android", дальше "кадр|нажато|кнопка|игрок2|только_позиция|...";
- бинарный GDR 2 (.gdr2, файл начинается с байтов "GDR"): формат maxnut/GDReplayFormat,
  разбор по его заголовкам gdr.hpp и binarystream.hpp (платформерные макросы не читаются);
- текстовый формат: строки "<кадр> <1|0> [<игрок 2: 1|0>]" и необязательная строка "fps=240".
"""

from __future__ import annotations

import json
import math
import struct
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


class _Reader:
    """Чтение по правилам binarystream.hpp: varint (LEB128), строки с длиной-префиксом,
    float и double в порядке байт от старшего к младшему."""

    def __init__(self, data: bytes, name: str):
        self.data = data
        self.pos = 0
        self.name = name

    def left(self) -> int:
        return len(self.data) - self.pos

    def _need(self, n: int) -> None:
        if n < 0 or self.left() < n:
            raise ValueError(f"{self.name}: файл GDR оборван или повреждён (позиция {self.pos})")

    def varint(self) -> int:
        result = 0
        for i in range(10):
            self._need(1)
            b = self.data[self.pos]
            self.pos += 1
            result |= (b & 0x7F) << (7 * i)
            if not b & 0x80:
                return result
        raise ValueError(f"{self.name}: слишком длинное число в файле GDR (позиция {self.pos})")

    def string(self) -> str:
        n = self.varint()
        self._need(n)
        raw = self.data[self.pos : self.pos + n]
        self.pos += n
        return raw.decode("utf-8", errors="replace")

    def floating(self, size: int) -> float:
        self._need(size)
        raw = self.data[self.pos : self.pos + size]
        self.pos += size
        return struct.unpack(">f" if size == 4 else ">d", raw)[0]

    def skip(self, n: int) -> None:
        self._need(n)
        self.pos += n


def parse_gdr2(raw: bytes, source: str | None = None) -> Macro:
    """Бинарный GDR 2 (.gdr2). Расширения ботов (позиции, скорости) пропускаются."""
    name = source or "макрос"
    if raw[:3] != b"GDR":
        raise ValueError(f"{name}: нет заголовка GDR")
    r = _Reader(raw, name)
    r.pos = 3
    version = r.varint()
    if version != 2:
        raise ValueError(f"{name}: версия формата GDR {version}, читается только 2")
    input_tag = r.string()
    r.string()  # автор
    r.string()  # описание
    r.floating(4)  # длительность
    r.varint()  # версия игры
    fps = r.floating(8)
    r.varint()  # seed
    r.varint()  # монеты
    r.varint()  # ldm
    platformer = bool(r.varint())
    bot_name = r.string()
    r.varint()  # версия бота
    level_id = r.varint()
    level_name = r.string()
    if platformer:
        raise ValueError(f"{name}: макрос для платформера, такие не поддерживаются")
    r.skip(r.varint())  # расширение всего макроса

    n_deaths = r.varint()
    for _ in range(n_deaths):
        r.varint()

    r.varint()  # число входов всего (читаем по концу файла, как в игре)
    p1_left = r.varint()
    inputs: list[Input] = []
    player2 = p1_left == 0
    prev = 0
    while r.left() > 0:
        packed = r.varint()
        frame = prev + (packed >> 1)
        inputs.append(Input(frame=frame, down=bool(packed & 1), player2=player2))
        prev = frame
        if input_tag:
            r.skip(r.varint())  # расширение входа
        if p1_left > 0:
            p1_left -= 1
            if p1_left == 0:
                prev = 0
                player2 = True
    inputs.sort(key=lambda i: (i.frame, i.player2))
    return Macro(
        inputs=inputs,
        fps=float(fps) if fps and math.isfinite(fps) else float(TPS),
        level_id=level_id or None,
        level_name=level_name or None,
        bot=bot_name or None,
        source=source,
    )


def to_gdr2(macro: Macro, author: str = "", tag: str = "") -> bytes:
    """Запись в GDR 2 без расширений. Нужна для тестов; игра и боты такой файл читают."""

    def vi(n: int) -> bytes:
        out = bytearray()
        while True:
            b = n & 0x7F
            n >>= 7
            out.append(b | (0x80 if n else 0))
            if not n:
                return bytes(out)

    def st(t: str) -> bytes:
        b = t.encode("utf-8")
        return vi(len(b)) + b

    ins = sorted(macro.inputs, key=lambda i: (i.frame, i.player2))
    p1 = [i for i in ins if not i.player2]
    p2 = [i for i in ins if i.player2]
    out = bytearray(b"GDR")
    out += vi(2) + st(tag) + st(author) + st("")
    out += struct.pack(">f", 0.0) + vi(22081) + struct.pack(">d", macro.fps)
    out += vi(0) + vi(0) + vi(0) + vi(0)
    out += st(macro.bot or "gddiff") + vi(1) + vi(macro.level_id or 0) + st(macro.level_name or "")
    out += vi(0)  # расширение макроса
    out += vi(0)  # смерти
    out += vi(len(ins)) + vi(len(p1))
    for group in (p1, p2):
        prev = 0
        for i in group:
            out += vi(((i.frame - prev) << 1) | int(i.down))
            if tag:
                out += vi(0)
            prev = i.frame
    return bytes(out)


def _msgpack(data: bytes, name: str):
    """Минимальный разбор MessagePack: числа, строки, bin, массивы, словари, nil и bool."""
    pos = 0

    def take(n: int) -> bytes:
        nonlocal pos
        if n < 0 or pos + n > len(data):
            raise ValueError(f"{name}: файл MessagePack оборван (позиция {pos})")
        out = data[pos : pos + n]
        pos += n
        return out

    def value(depth: int = 0):
        if depth > 64:
            raise ValueError(f"{name}: слишком глубокая вложенность MessagePack")
        t = take(1)[0]
        if t <= 0x7F:
            return t
        if t >= 0xE0:
            return t - 256
        if 0xA0 <= t <= 0xBF:
            return take(t & 0x1F).decode("utf-8", errors="replace")
        if 0x90 <= t <= 0x9F:
            return [value(depth + 1) for _ in range(t & 0x0F)]
        if 0x80 <= t <= 0x8F:
            return {value(depth + 1): value(depth + 1) for _ in range(t & 0x0F)}
        fixed = {
            0xCC: ">B", 0xCD: ">H", 0xCE: ">I", 0xCF: ">Q",
            0xD0: ">b", 0xD1: ">h", 0xD2: ">i", 0xD3: ">q",
            0xCA: ">f", 0xCB: ">d",
        }
        if t in fixed:
            fmt = fixed[t]
            return struct.unpack(fmt, take(struct.calcsize(fmt)))[0]
        if t == 0xC0:
            return None
        if t in (0xC2, 0xC3):
            return t == 0xC3
        lens = {0xD9: 1, 0xC4: 1, 0xDA: 2, 0xC5: 2, 0xDB: 4, 0xC6: 4}
        if t in lens:
            n = int.from_bytes(take(lens[t]), "big")
            raw = take(n)
            return raw.decode("utf-8", errors="replace") if t in (0xD9, 0xDA, 0xDB) else raw
        if t in (0xDC, 0xDD):
            n = int.from_bytes(take(2 if t == 0xDC else 4), "big")
            if n > len(data):
                raise ValueError(f"{name}: неверный размер массива MessagePack")
            return [value(depth + 1) for _ in range(n)]
        if t in (0xDE, 0xDF):
            n = int.from_bytes(take(2 if t == 0xDE else 4), "big")
            if n > len(data):
                raise ValueError(f"{name}: неверный размер словаря MessagePack")
            return {value(depth + 1): value(depth + 1) for _ in range(n)}
        raise ValueError(f"{name}: неподдерживаемый тип MessagePack 0x{t:02x}")

    return value()


def parse_gdr1(raw: bytes, source: str | None = None) -> Macro:
    """GDR 1 (.gdr): MessagePack с теми же полями, что у GDR JSON."""
    obj = _msgpack(raw, source or "макрос")
    if not isinstance(obj, dict):
        raise ValueError(f"{source or 'макрос'}: в MessagePack ожидался словарь")
    return parse_gdr_json(obj, source)


def parse_xd(text: str, source: str | None = None) -> Macro:
    """xdBot .xd (по Macro::XDtoGDR из xdBot); строки с позициями пропускаются."""
    name = source or "макрос"
    mult = 1.0
    fps = float(TPS)
    inputs = []
    for line_no, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        f = line.split("|")
        if len(f) < 4:
            if f[0] == "android":
                mult, fps = 4.0, 60.0
            else:
                try:
                    fps = float(f[0])
                except ValueError:
                    raise ValueError(f"{name}: строка {line_no}: неверная частота кадров") from None
                if fps <= 0:
                    raise ValueError(f"{name}: строка {line_no}: неверная частота кадров")
                mult = TPS / fps
            continue
        if len(f) < 5:
            raise ValueError(f"{name}: строка {line_no}: мало полей")
        try:
            frame = float(f[0])
        except ValueError:
            raise ValueError(f"{name}: строка {line_no}: неверный кадр") from None
        if f[4] == "1" or int(f[2] or 0) != JUMP_BUTTON:
            continue
        inputs.append(Input(frame=int(round(frame * mult)), down=f[1] == "1", player2=f[3] == "1"))
    inputs.sort(key=lambda i: (i.frame, i.player2))
    return Macro(inputs=inputs, fps=float(TPS), bot="xdBot (.xd)", source=source)


def load_macro(path: str | Path) -> Macro:
    path = Path(path)
    raw = path.read_bytes()
    if raw[:3] == b"GDR":
        return parse_gdr2(raw, str(path))
    if raw[:1] and (0x80 <= raw[0] <= 0x8F or raw[0] in (0xDE, 0xDF)):
        return parse_gdr1(raw, str(path))
    text = raw.decode("utf-8-sig")
    if text.lstrip().startswith("{"):
        return parse_gdr_json(json.loads(text), str(path))
    if "|" in text:
        return parse_xd(text, str(path))
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
