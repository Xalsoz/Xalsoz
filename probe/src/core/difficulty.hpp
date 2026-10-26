// Сложность измеренных окон для игрока с данной частотой кадров.
// Перенос gddiff/features/refresh.py на C++: модель и числа те же (см. описание там).
// Окно клика W мс; игра принимает нажатие на границе следующего кадра; игрок целится
// на полкадра раньше середины окна с разбросом sigma. Сложность уровня равна
// сумме -log P по кликам.

#pragma once

#include <vector>

namespace probe {

// Вероятность пройти клик с окном windowMs при частоте hz (infinity = ввод между кадрами).
double hitProbability(double windowMs, double hz, double sigmaMs, int phases = 64);

struct DifficultyRow {
    double hz = 0;
    double nats = 0;           // сумма -log P
    double log10Attempts = 0;  // то же в десятичных логарифмах: порядок числа попыток
    double worstP = 1;         // вероятность самого трудного клика
    double impossibleShare = 0;  // доля попыток, в которых в какой-то клик не попадает ни одна граница кадра
};

std::vector<DifficultyRow> levelDifficulty(const std::vector<double>& windowsMs, const std::vector<double>& hzList,
                                           double sigmaMs);

}  // namespace probe
