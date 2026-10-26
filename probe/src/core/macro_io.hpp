// Чтение макросов ботов прямо в моде, без Python: бинарный GDR 2 (.gdr2),
// GDR JSON (.gdr.json, .json) и текстовый формат мода. Не зависит от игры.
//
// GDR 2 разобран по gdr.hpp и binarystream.hpp из maxnut/GDReplayFormat; тот же
// разбор в Python (gddiff/io/macro.py), оба проверены на файле, записанном
// самой библиотекой.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "probe_core.hpp"

namespace probe {

struct MacroInfo {
    double fps = 240.0;
    int levelId = 0;
    std::string levelName;
    std::string bot;
    std::string note;  // предупреждение: пересчёт частоты, пропущенные клики и т.п.
};

// Определяет формат по содержимому. Кадры приводятся к 240 тикам в секунду.
// Возвращает пустой вектор и заполняет error при ошибке.
std::vector<Input> parseMacroBytes(const std::vector<uint8_t>& bytes, MacroInfo* info, std::string* error);

std::vector<Input> parseGdr2(const std::vector<uint8_t>& bytes, MacroInfo* info, std::string* error);
std::vector<Input> parseGdrJson(const std::string& text, MacroInfo* info, std::string* error);

}  // namespace probe
