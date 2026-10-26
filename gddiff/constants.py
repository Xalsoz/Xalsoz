"""Константы Geometry Dash 2.2, на которые опирается проект.

ID объектов и скорости собраны в одном месте: если на реальных уровнях
что-то не совпадёт, править нужно только здесь.
"""

# Физика 2.2 считается с частотой 240 тиков в секунду.
TPS = 240
TICK_MS = 1000.0 / TPS

MODES = ("cube", "ship", "ball", "ufo", "wave", "robot", "spider", "swing")

# Порталы режимов: ID объекта -> режим.
GAMEMODE_PORTALS = {
    12: "cube",
    13: "ship",
    47: "ball",
    111: "ufo",
    660: "wave",
    745: "robot",
    1331: "spider",
    1933: "swing",
}

# Порталы скорости: ID объекта -> множитель скорости.
SPEED_PORTALS = {200: 0.5, 201: 1.0, 202: 2.0, 203: 3.0, 1334: 4.0}

# Горизонтальная скорость игрока в игровых единицах в секунду (1 блок = 30 единиц).
UNITS_PER_SECOND = {0.5: 251.16, 1.0: 311.58, 2.0: 387.42, 3.0: 468.0, 4.0: 576.0}

# Заголовок строки уровня: kA2 - стартовый режим, kA4 - стартовая скорость.
HEADER_MODE = dict(enumerate(MODES))
HEADER_SPEED = {0: 1.0, 1: 0.5, 2: 2.0, 3: 3.0, 4: 4.0}

# Триггеры, которые влияют на видимость.
CAMERA_TRIGGERS = frozenset({1520, 1913, 1914, 1916, 2015, 2062})  # тряска, зум, статика, смещение, поворот, край
VISIBILITY_TRIGGERS = frozenset({1006, 1007, 1049})  # pulse, alpha, toggle
OTHER_TRIGGERS = frozenset({899, 901})  # color, move

# Объекты, которые не являются геометрией (не считаются в плотности объектов).
NON_GEOMETRY = frozenset(GAMEMODE_PORTALS) | frozenset(SPEED_PORTALS) | CAMERA_TRIGGERS | VISIBILITY_TRIGGERS | OTHER_TRIGGERS
