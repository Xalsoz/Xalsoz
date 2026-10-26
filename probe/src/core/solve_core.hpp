// Ядро автопрохождения: само находит клики уровня, макрос не нужен.
// Как и probe_core, не зависит от игры: адаптер (src/main.cpp или игрушечная
// физика в тестах) крутит его тик за тиком.
//
// Идея. Игра умеет откатываться к чекпоинту, поэтому клик можно «примерить»:
//   1. Стоим в точке решения t0 (кнопка отпущена или удерживается).
//   2. Базовая проба: ничего не нажимать. Если игрок выживает горизонт, точка
//      безопасна: идём вперёд без клика.
//   3. Иначе по очереди пробуем нажать (или отпустить) через d = 1, 2, ... тиков
//      и запоминаем тик смерти T_d: игрок после клика ничего больше не делает.
//   4. Клики, спасающие от ближайшего препятствия, умирают позже, чем «ничего»
//      или клик не вовремя. Смежные d с похожим T_d дают окно. Берём окно с самым
//      поздним T_d (при равенстве самое широкое) и жмём в его середине: так
//      маршрут получается самым «прощающим».
//   5. Идём дальше от выбранного клика. Если впереди тупик, откатываемся к
//      прошлому решению и берём следующий вариант (поиск в глубину).
//
// Чекпоинты лежат стеком: ставим на отпущенной кнопке (стандартные чекпоинты GD
// не хранят удержание), откатываемся к верхнему и проигрываем недавние клики.
//
// Договор с адаптером тот же, что у Controller (tick / afterRestore), плюс поля
// TickOutput::popCheckpoints и dropOldestCheckpoints: адаптер выполняет их без
// участия игры.

#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "probe_core.hpp"

namespace probe {

struct SolveConfig {
    int horizon = 180;        // сколько тиков смотреть вперёд после пробного клика
    int maxDelay = 60;        // как далеко искать следующий клик, тиков
    int scanStride = 2;       // сначала каждый scanStride-й сдвиг, потом уточнение вокруг успешных
    int slack = 4;            // допуск тика смерти внутри одного окна
    int deadMargin = 6;       // если лучшая проба умирает раньше, чем через столько тиков, это тупик
    int maxTail = 900;        // дольше этого без чекпоинта при зажатой кнопке не ходим
    int maxAnchors = 24;      // сколько чекпоинтов держать (глубина отката)
    int minCheckpointFrame = 2;
    int64_t maxTicks = 400'000'000;
    int maxBacktracks = 200'000;
    int maxReplayFailures = 25;  // подряд: проигрыш чекпоинта не повторяет пробу
};

class Solver {
public:
    explicit Solver(SolveConfig config);

    TickOutput tick(int64_t frame, bool died, bool ended);
    TickOutput afterRestore(int64_t frame);

    bool done() const { return m_state == State::Finished || m_state == State::Aborted; }
    bool solved() const { return m_state == State::Finished; }
    bool aborted() const { return m_state == State::Aborted; }
    const std::string& abortReason() const { return m_abortReason; }

    // Найденные клики. Если уровень не пройден до конца, глубочайший достигнутый префикс.
    std::vector<Input> macro() const { return solved() ? m_script : m_best; }
    // Ширина окна (в тиках), в середину которого поставлен каждый клик из macro().
    std::vector<int> widths() const { return solved() ? m_widths : m_bestWidths; }

    int64_t ticksUsed() const { return m_ticks; }
    int64_t backtracks() const { return m_backtracks; }
    int64_t reachedTick() const { return m_reached; }
    int64_t tickRebases() const { return m_rebases; }
    const SolveConfig& config() const { return m_cfg; }
    std::string progressText() const;

private:
    enum class State { Boot, Running, Finished, Aborted };
    enum class Purpose { Baseline, Candidate, Commit };
    struct Anchor {
        int64_t id = 0;
        int64_t tick = 0;
        bool held = false;
    };
    struct Option {
        int64_t tick = 0;   // когда нажать или отпустить
        int64_t lo = 0;     // границы окна, в тиках
        int64_t hi = 0;
        int width = 0;
        int64_t score = 0;  // тик смерти после этого клика (kInf: горизонт пережит)
    };
    struct Node {
        int64_t t0 = 0;
        bool held = false;
        size_t scriptLen = 0;
        int64_t anchorId = 0;
        bool ownsAnchor = false;
        bool scanned = false;     // у узла есть варианты (иначе это просто безопасный шаг вперёд)
        int64_t tNone = 0;
        int dMax = 0;
        std::vector<int64_t> T;   // T[d]: тик смерти при клике через d (kUnset: не мерили)
        std::vector<int> pending;
        size_t pendingPos = 0;
        bool refining = false;
        bool waited = false;      // удерживаемая кнопка: уже пробовали просто подождать
        std::vector<Option> options;
        size_t nextOption = 0;
    };
    struct Run {
        bool active = false;
        Purpose purpose = Purpose::Baseline;
        int64_t limit = 0;
        size_t scriptPos = 0;
        int64_t extraTick = -1;
        bool extraDown = false;
        int d = 0;
        int64_t target = 0;       // для Commit: тик, на котором узел становится текущим
        bool waitOnly = false;    // Commit без нового клика: просто дойти до тика target
    };

    Node& top() { return m_nodes.back(); }
    void applyInputs(int64_t frame, std::vector<Input>& press);
    TickOutput beginRun(Run run);
    TickOutput finishRun(bool died, bool ended, int64_t frame);
    TickOutput onBaseline(bool died, bool ended, int64_t frame);
    TickOutput onCandidate(bool died, bool ended, int64_t frame);
    TickOutput continueScan();
    TickOutput chooseNext();
    TickOutput reachTarget(int64_t frame);
    TickOutput advanceSafe(int64_t frame);
    void makeNode(int64_t frame, bool mark, bool heldAnchor, TickOutput& out);
    void finalizeOptions(Node& n);
    void popTopNode();
    void noteProgress(int64_t t0);
    TickOutput abort(std::string message);
    TickOutput finish();
    TickOutput flush(TickOutput out);

    SolveConfig m_cfg;
    State m_state = State::Boot;
    std::vector<Input> m_script;
    std::vector<int> m_widths;
    std::vector<Input> m_best;
    std::vector<int> m_bestWidths;
    int64_t m_bestT0 = -1000;
    std::vector<Anchor> m_anchors;
    int64_t m_nextAnchorId = 1;
    std::deque<Node> m_nodes;
    Run m_run;
    bool m_expectRestore = false;
    int m_pendingPops = 0;
    int m_pendingDrops = 0;
    int64_t m_ticks = 0;
    int64_t m_backtracks = 0;
    int64_t m_reached = 0;
    int64_t m_rebases = 0;
    int m_replayFails = 0;
    int64_t m_lastFrame = 0;
    std::string m_abortReason;
};

// Запись найденных кликов в тех же форматах, что читает Python (probe-prepare) и xdBot-подобные боты.
void writeMacroText(std::ostream& out, const std::vector<Input>& macro);
void writeMacroGdrJson(std::ostream& out, const std::vector<Input>& macro, int levelId, const std::string& levelName);

}  // namespace probe
