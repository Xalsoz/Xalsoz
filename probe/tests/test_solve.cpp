// Тесты автопрохождения на игрушечном мире (toy_world.hpp).

#include "core/solve_core.hpp"
#include "toy_world.hpp"

#include <cstdio>
#include <sstream>

using namespace toyworld;

static int g_failed = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(cond)) {                                                                    \
            ++g_failed;                                                                   \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                                 \
    } while (0)

static void testSolvesPlainLevels() {
    for (unsigned seed = 1; seed <= 15; ++seed) {
        World w = randomWorld(seed, 60, 0);
        Result r = solve(w, SolveConfig{});
        CHECK(r.solved);
        if (!r.solved) std::fprintf(stderr, "  seed %u: %s\n", seed, r.reason.c_str());
        CHECK(replayPasses(w, r.macro));
        CHECK(r.macro.size() == w.reqs.size());
        // Каждый клик в середине окна: ширина найденного окна близка к настоящей.
        for (size_t k = 0; k < r.macro.size() && k < w.reqs.size(); ++k) {
            const int trueW = static_cast<int>(w.reqs[k].B - w.reqs[k].A + 1);
            CHECK(r.macro[k].frame >= w.reqs[k].A && r.macro[k].frame <= w.reqs[k].B);
            CHECK(r.widths[k] <= trueW + 1);
            if (k % 2 == 0) CHECK(std::abs(r.widths[k] - trueW) <= 1);  // нажатия: окно найдено целиком
        }
    }
}

static void testBacktracksOutOfTraps() {
    int totalBacktracks = 0;
    for (unsigned seed = 1; seed <= 15; ++seed) {
        World w = randomWorld(seed, 60, 5);
        Result r = solve(w, SolveConfig{});
        CHECK(r.solved);
        if (!r.solved) std::fprintf(stderr, "  seed %u: %s\n", seed, r.reason.c_str());
        CHECK(replayPasses(w, r.macro));
        totalBacktracks += static_cast<int>(r.backtracks);
    }
    // Ловушки выглядят выгоднее честного пути, значит, без откатов не обойтись.
    CHECK(totalBacktracks > 0);
}

static void testCubeStyleReleaseIsHarmless() {
    // Как прыжок куба: отпускание ничего не решает, важны только нажатия. Раньше такие позиции
    // считались тупиком, потому что отпускание не «улучшало» исход.
    int totalBacktracks = 0;
    for (unsigned seed = 1; seed <= 15; ++seed) {
        World w = randomWorld(seed, 60, seed % 2 ? 5 : 0);
        w.cube = true;
        Result r = solve(w, SolveConfig{});
        CHECK(r.solved);
        if (!r.solved) std::fprintf(stderr, "  seed %u: %s\n", seed, r.reason.c_str());
        if (r.solved) CHECK(replayPasses(w, r.macro));
        totalBacktracks += static_cast<int>(r.backtracks);
        // Кнопку не держат дольше нужного: после нажатия отпускание вскоре.
        for (size_t k = 0; k + 1 < r.macro.size(); ++k)
            if (r.macro[k].down && !r.macro[k + 1].down) CHECK(r.macro[k + 1].frame - r.macro[k].frame <= 8);
    }
    (void)totalBacktracks;
}

static void testSmallAnchorBudget() {
    // Мало чекпоинтов: самые старые сбрасываются, откат возможен только вблизи.
    SolveConfig cfg;
    cfg.maxAnchors = 6;
    for (unsigned seed = 1; seed <= 8; ++seed) {
        World w = randomWorld(seed, 60, 5);
        Result r = solve(w, cfg);
        CHECK(r.maxStack <= 7);
        CHECK(r.solved);
        if (r.solved) CHECK(replayPasses(w, r.macro));
    }
}

static void testStrideOneAndTwoAgree() {
    for (unsigned seed = 1; seed <= 6; ++seed) {
        World w = randomWorld(seed, 40, 0);
        SolveConfig a, b;
        a.scanStride = 1;
        b.scanStride = 3;
        Result ra = solve(w, a), rb = solve(w, b);
        CHECK(ra.solved && rb.solved);
        CHECK(ra.macro.size() == rb.macro.size());
        // Грубый шаг с уточнением даёт ту же ширину окон, середина отличается не больше чем на тик.
        for (size_t k = 0; k < ra.macro.size() && k < rb.macro.size(); ++k) {
            CHECK(std::abs(ra.widths[k] - rb.widths[k]) <= 1);
            CHECK(std::abs(ra.macro[k].frame - rb.macro[k].frame) <= 1);
        }
        CHECK(rb.ticks < ra.ticks);
    }
}

static void testImpossibleLevelAbortsWithPrefix() {
    World w = randomWorld(3, 30, 0);
    // Окно третьего требования пустое: пройти нельзя.
    w.reqs[2].A = w.reqs[2].B + 5;
    Result r = solve(w, SolveConfig{});
    CHECK(r.aborted);
    CHECK(!r.solved);
    CHECK(r.reason.find("No route") != std::string::npos);
}

static void testTickBudget() {
    SolveConfig cfg;
    cfg.maxTicks = 2000;
    Result r = solve(randomWorld(1, 60, 0), cfg);
    CHECK(r.aborted);
    CHECK(r.reason.find("budget") != std::string::npos);
}

static void testMacroWriters() {
    std::vector<probe::Input> m{{10, true, false}, {20, false, false}};
    std::ostringstream t;
    probe::writeMacroText(t, m);
    CHECK(t.str() == "tps=240\n10 1 0\n20 0 0\n");
    std::ostringstream j;
    probe::writeMacroGdrJson(j, m, 5, "A\"B");
    CHECK(j.str().find("\"id\":5") != std::string::npos);
    CHECK(j.str().find("A\\\"B") != std::string::npos);
    CHECK(j.str().find("{\"frame\":20,\"btn\":1,\"2p\":false,\"down\":false}") != std::string::npos);
    std::string err;
    auto back = probe::parseMacroText(t.str(), &err);
    CHECK(err.empty() && back.size() == 2 && back[1].frame == 20 && !back[1].down);
}

int main() {
    testSolvesPlainLevels();
    testBacktracksOutOfTraps();
    testCubeStyleReleaseIsHarmless();
    testSmallAnchorBudget();
    testStrideOneAndTwoAgree();
    testImpossibleLevelAbortsWithPrefix();
    testTickBudget();
    testMacroWriters();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
