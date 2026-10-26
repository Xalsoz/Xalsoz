// Ядро измерителя окон допуска. Не зависит от игры: им управляет адаптер
// (src/main.cpp в моде Geode или игрушечная физика в тестах).
//
// Идея. Берём макрос, который проходит уровень. Для каждого ввода по очереди
// сдвигаем его на s тиков раньше или позже, оставляя остальные вводы как в
// макросе, и смотрим, выживает ли игрок в течение горизонта после ввода.
// Самые дальние сдвиги, при которых он выживает, это края окна.
//
// Поиск края: разгон 1, 2, 4, 8... тиков до первой смерти или до предела,
// затем деление пополам между последним выживанием и первой смертью.
// Предполагается, что окно сплошное (выживание монотонно по сдвигу).
//
// Чтобы не играть каждый раз с начала, перед вводом ставится чекпоинт, и
// каждая проба начинается с него. Режим exactRestart вместо этого начинает
// каждую пробу с начала уровня: медленно, но без погрешностей чекпоинтов.
//
// Договор с адаптером на каждом тике:
//   1. игра считает физику тика;
//   2. адаптер вызывает tick(frame, died, ended);
//   3. если команда MarkCheckpoint, адаптер ставит чекпоинт ДО нажатия кликов;
//   4. адаптер нажимает клики из press по порядку;
//   5. если команда RestoreCheckpoint, адаптер восстанавливает чекпоинт
//      (или перезапускает уровень), вызывает afterRestore(frame) и нажимает
//      клики из его ответа.

#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace probe {

struct Input {
    int64_t frame = 0;
    bool down = true;
    bool player2 = false;
};

struct Config {
    int maxShift = 48;          // предел сдвига в тиках в каждую сторону (48 тиков = 200 мс)
    int horizon = 240;          // сколько тиков после ввода игрок должен прожить (240 = 1 с)
    int stride = 1;             // мерить каждый stride-й ввод (остальные просто проигрываются)
    int firstIndex = 0;         // с какого ввода начинать
    int lastIndex = -1;         // по какой ввод включительно (-1 = до конца)
    bool exactRestart = false;  // каждая проба с начала уровня вместо чекпоинта
    bool verifyRestore = true;  // после восстановления проверять, что исходный клик проходит
    bool checkpointOnRelease = true;  // ставить чекпоинты только там, где кнопка отпущена
    int minCheckpointFrame = 2; // чекпоинт не раньше этого тика
};

enum class Status { NotMeasured, Ok, Skipped };

struct Window {
    size_t index = 0;
    Input input;
    int early = 0;       // насколько тиков раньше ещё можно нажать
    int late = 0;        // насколько тиков позже ещё можно нажать
    int earlyLimit = 0;  // предел поиска раньше (сосед или maxShift); early == earlyLimit -> окно может быть шире
    int lateLimit = 0;
    int trials = 0;
    Status status = Status::NotMeasured;
};

enum class Command {
    None,
    MarkCheckpoint,     // поставить чекпоинт сейчас, до нажатия кликов этого тика
    RestoreCheckpoint,  // вернуться к последнему чекпоинту (или в начало при exactRestart)
    Finish,             // всё измерено
    Abort,              // ошибка, см. message
};

struct TickOutput {
    std::vector<Input> press;  // нажать или отпустить сейчас, в этом порядке
    Command command = Command::None;
    std::string message;
};

class Controller {
public:
    Controller(std::vector<Input> macro, Config config);

    // Вызывать один раз на каждом тике после физики.
    TickOutput tick(int64_t frame, bool died, bool ended);

    // Вызывать сразу после выполнения RestoreCheckpoint.
    TickOutput afterRestore(int64_t frame);

    bool done() const { return m_phase == Phase::Finished || m_phase == Phase::Aborted; }
    bool aborted() const { return m_phase == Phase::Aborted; }
    const std::string& abortReason() const { return m_abortReason; }
    const std::vector<Window>& windows() const { return m_windows; }
    const Config& config() const { return m_cfg; }
    size_t measuredCount() const;
    size_t targetCount() const { return m_targets.size(); }
    int64_t totalTrials() const { return m_totalTrials; }
    std::string progressText() const;

private:
    enum class Phase { Advance, Trial, Finished, Aborted };
    enum class Side { Baseline, Verify, Early, Late };

    Window& current() { return m_windows[m_targets[m_targetPos]]; }
    size_t activeIndex() const;
    void collect(int64_t fromExclusive, int64_t toInclusive, std::vector<Input>& out) const;
    bool heldBefore(int64_t tick, bool player2, int64_t* pressFrame) const;
    int64_t releasedTickAtOrBefore(int64_t tick) const;
    void enterBaseline(bool afterRestore);
    void planTarget();
    bool startSide(Side side);
    bool prepareNextTrial(bool lastPassed);
    TickOutput onOutcome(bool passed);
    TickOutput moveToNextTarget();
    TickOutput restore();
    TickOutput abort(std::string message);
    TickOutput finish();

    std::vector<Input> m_macro;
    Config m_cfg;
    std::vector<size_t> m_targets;
    std::vector<Window> m_windows;

    Phase m_phase = Phase::Advance;
    size_t m_targetPos = 0;
    int64_t m_planned = 0;           // тик, на котором поставить чекпоинт для текущей цели
    bool m_haveCheckpoint = false;
    int64_t m_checkpointFrame = -1;
    int64_t m_markLastFrame = -1;    // m_lastFrame в момент установки чекпоинта
    int64_t m_lastFrame = -1;        // клики по этот тик включительно уже нажаты
    int64_t m_trialEnd = 0;
    int m_shift = 0;                 // сдвиг активного ввода в текущей пробе

    Side m_side = Side::Baseline;
    bool m_baselineAfterRestore = false;  // базовая проба началась с восстановления, а не сплошным прогоном
    int m_lastPass = 0;              // модуль последнего прошедшего сдвига
    int m_firstFail = -1;            // модуль первого смертельного сдвига (-1 = ещё нет)
    int m_magnitude = 0;             // модуль текущего сдвига

    int64_t m_totalTrials = 0;
    int64_t m_tickFrame = 0;         // тик последнего вызова tick()
    std::string m_abortReason;
};

// Текстовый формат входа: "frame down player2" на строку (down и player2: 1 или 0),
// # комментарии, строка "tps=240" (поддерживается только 240).
std::vector<Input> parseMacroText(const std::string& text, std::string* error = nullptr);

// CSV с результатами; первая строка комментарий с версией и параметрами.
void writeCsv(std::ostream& out, const Controller& controller, int tps = 240);

const char* statusName(Status status);

}  // namespace probe
