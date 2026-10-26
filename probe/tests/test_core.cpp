// Тесты ядра на игрушечной «физике» (см. toy_sim.hpp).

#include "core/probe_core.hpp"
#include "toy_sim.hpp"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace probe;

static int g_failed = 0;
static int g_checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

using namespace toy;

static void checkWindows(const ToyLevel& level, const RunResult& r, const Config& cfg) {
    size_t ok = 0;
    for (const Window& w : r.windows) {
        if (w.status != Status::Ok) continue;
        ++ok;
        const size_t k = w.index;
        const int expectEarly = std::min(-level.lo[k], w.earlyLimit);
        const int expectLate = std::min(level.hi[k], w.lateLimit);
        CHECK(w.early == expectEarly);
        CHECK(w.late == expectLate);
        if (w.early != expectEarly || w.late != expectLate) {
            std::fprintf(stderr, "  input %zu: got [-%d,+%d], expected [-%d,+%d], limits [%d,%d]\n", k, w.early, w.late,
                         expectEarly, expectLate, w.earlyLimit, w.lateLimit);
        }
        CHECK(w.earlyLimit <= cfg.maxShift && w.lateLimit <= cfg.maxShift);
    }
    CHECK(ok > 0);
}

static void testRecoversWindowsWithCheckpoints() {
    for (unsigned seed = 1; seed <= 20; ++seed) {
        ToyLevel level = randomLevel(seed, 120, 100);
        Config cfg;
        RunResult r = run(level, cfg);
        CHECK(!r.aborted);
        if (r.aborted) std::fprintf(stderr, "  aborted: %s\n", r.reason.c_str());
        checkWindows(level, r, cfg);
        size_t measured = 0;
        for (const Window& w : r.windows) measured += w.status == Status::Ok;
        CHECK(measured == level.macro.size());
    }
}

static void testExactRestartGivesSameWindows() {
    ToyLevel level = randomLevel(7, 40, 60);
    Config a;
    Config b;
    b.exactRestart = true;
    RunResult ra = run(level, a);
    RunResult rb = run(level, b);
    CHECK(!ra.aborted && !rb.aborted);
    for (size_t i = 0; i < level.macro.size(); ++i) {
        CHECK(ra.windows[i].early == rb.windows[i].early);
        CHECK(ra.windows[i].late == rb.windows[i].late);
    }
    // Перезапуск с начала дороже: тиков должно уйти больше.
    CHECK(rb.ticks > ra.ticks);
}

static void testCapsAtLimitAndNeighbours() {
    ToyLevel level;
    level.macro = {{100, true}, {110, false}, {400, true}};
    level.lo = {-100, -5, -100};
    level.hi = {100, 100, 100};
    level.delay = {0, 0, 0};
    level.endFrame = 800;
    Config cfg;
    cfg.maxShift = 30;
    RunResult r = run(level, cfg);
    CHECK(!r.aborted);
    // Ввод 0: рано ограничен maxShift, поздно ограничен соседом (110 - 100 - 1 = 9).
    CHECK(r.windows[0].early == 30 && r.windows[0].earlyLimit == 30);
    CHECK(r.windows[0].late == 9 && r.windows[0].lateLimit == 9);
    // Ввод 1: рано окно меньше соседа (5 < 9).
    CHECK(r.windows[1].early == 5 && r.windows[1].earlyLimit == 9);
    CHECK(r.windows[2].late == 30);
}

static void testDelayedDeathBeyondHorizonIsMissed() {
    // Смерть через 300 тиков после плохого клика при горизонте 240 не ловится:
    // окно получается шире настоящего. Это ограничение метода, тест его фиксирует.
    ToyLevel level;
    level.macro = {{100, true}, {500, false}};
    level.lo = {-3, -100};
    level.hi = {3, 100};
    level.delay = {300, 0};
    level.endFrame = 1200;
    Config cfg;
    RunResult r = run(level, cfg);
    CHECK(!r.aborted);
    CHECK(r.windows[0].early > 3);
    cfg.horizon = 400;
    r = run(level, cfg);
    CHECK(r.windows[0].early == 3 && r.windows[0].late == 3);
}

static void testAbortsWhenMacroDoesNotPass() {
    // Вводы раз в 300 тиков, ввод 10 вне своего окна: смерть на его тике + 7.
    ToyLevel level;
    for (int i = 0; i < 20; ++i) {
        level.macro.push_back(Input{100 + 300 * i, i % 2 == 0, false});
        level.lo.push_back(-10);
        level.hi.push_back(10);
        level.delay.push_back(7);
    }
    level.endFrame = 100 + 300 * 20;
    level.lo[10] = 2;
    level.hi[10] = 5;
    RunResult r = run(level, Config{});
    CHECK(r.aborted);
    const std::string deathTick = std::to_string(100 + 300 * 10 + 7);
    CHECK(r.reason.find("tick " + deathTick) != std::string::npos);
    if (r.reason.find(deathTick) == std::string::npos) std::fprintf(stderr, "  reason: %s\n", r.reason.c_str());
    // Окна до сломанного места успели измериться и верны.
    for (int i = 0; i < 10; ++i) CHECK(r.windows[i].status == Status::Ok && r.windows[i].early == 10);
    CHECK(r.windows[10].status != Status::Ok);
}

static void testDetectsImpreciseCheckpoints() {
    ToyLevel level = randomLevel(4, 20, 20);
    level.lo[3] = 0;  // окно ровно в один тик: сдвиг состояния на тик убивает
    level.hi[3] = 0;
    RunResult r = run(level, Config{}, Fault::Drift);
    CHECK(r.aborted);
    CHECK(r.reason.find("exact-restart") != std::string::npos);
}

static void testCheckpointsAvoidHeldButton() {
    // Чекпоинт, который теряет зажатую кнопку: с постановкой на отпущенной кнопке
    // всё измеряется верно, без неё проверка после восстановления ловит ошибку.
    ToyLevel level = randomLevel(9, 80, 40);
    RunResult r = run(level, Config{}, Fault::DropHold);
    CHECK(!r.aborted);
    if (r.aborted) std::fprintf(stderr, "  aborted: %s\n", r.reason.c_str());
    CHECK(r.heldCheckpoints == 0);
    checkWindows(level, r, Config{});
    size_t measured = 0;
    for (const Window& w : r.windows) measured += w.status == Status::Ok;
    CHECK(measured == level.macro.size());

    Config naive;
    naive.checkpointOnRelease = false;
    RunResult bad = run(level, naive, Fault::DropHold);
    CHECK(bad.aborted);
    CHECK(bad.heldCheckpoints > 0);
}

static void testStrideAndRange() {
    ToyLevel level = randomLevel(5, 60, 30);
    Config cfg;
    cfg.firstIndex = 10;
    cfg.lastIndex = 40;
    cfg.stride = 5;
    RunResult r = run(level, cfg);
    CHECK(!r.aborted);
    for (const Window& w : r.windows) {
        const bool target = w.index >= 10 && w.index <= 40 && (w.index - 10) % 5 == 0;
        CHECK((w.status == Status::Ok) == target);
    }
    checkWindows(level, r, cfg);
}

static void testTrialBudget() {
    ToyLevel level = randomLevel(11, 200, 50);
    RunResult r = run(level, Config{});
    CHECK(!r.aborted);
    const double perInput = static_cast<double>(r.trials) / level.macro.size();
    std::printf("trials per input: %.1f, ticks per input: %.0f\n", perInput,
                static_cast<double>(r.ticks) / level.macro.size());
    CHECK(perInput < 20.0);
}

static void testParseAndCsv() {
    std::string err;
    auto inputs = parseMacroText("# comment\ntps=240\n10 1\n20 0 0\n15 1 1\n", &err);
    CHECK(err.empty());
    CHECK(inputs.size() == 3);
    CHECK(inputs[2].frame == 15 && inputs[2].down && inputs[2].player2);
    auto bad = parseMacroText("tps=60\n1 1\n", &err);
    CHECK(bad.empty() && !err.empty());

    ToyLevel level;
    level.macro = {{50, true}, {90, false}};
    level.lo = {-4, -6};
    level.hi = {2, 8};
    level.delay = {0, 0};
    level.endFrame = 500;
    Controller ctl(level.macro, Config{});
    std::ostringstream csv;
    writeCsv(csv, ctl);
    const std::string s = csv.str();
    CHECK(s.rfind("# gddiff-probe v1 tps=240", 0) == 0);
    CHECK(s.find("index,frame,down,player2,early,late,early_limit,late_limit,trials,status") != std::string::npos);
    CHECK(s.find("0,50,1,0,") != std::string::npos);
}

int main() {
    testRecoversWindowsWithCheckpoints();
    testExactRestartGivesSameWindows();
    testCapsAtLimitAndNeighbours();
    testDelayedDeathBeyondHorizonIsMissed();
    testAbortsWhenMacroDoesNotPass();
    testDetectsImpreciseCheckpoints();
    testCheckpointsAvoidHeldButton();
    testStrideAndRange();
    testTrialBudget();
    testParseAndCsv();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
