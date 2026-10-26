// Совпадение C++ и Python (gddiff/features/refresh.py): значения посчитаны Python-версией.

#include "core/difficulty.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

static int g_failed = 0;
static int g_checks = 0;
#define NEAR(a, b)                                                                                       \
    do {                                                                                                 \
        ++g_checks;                                                                                      \
        if (std::fabs((a) - (b)) > 1e-9) {                                                               \
            ++g_failed;                                                                                  \
            std::fprintf(stderr, "%s:%d: %.12f != %.12f\n", __FILE__, __LINE__, double(a), double(b));   \
        }                                                                                                \
    } while (0)

int main() {
    const double inf = std::numeric_limits<double>::infinity();
    NEAR(probe::hitProbability(20.8, 60, 10), 0.6500801776185636);
    NEAR(probe::hitProbability(4.17, 60, 10), 0.14799360363755887);
    NEAR(probe::hitProbability(4.17, 240, 10), 0.16386245502481334);
    NEAR(probe::hitProbability(50.0, 144, 10), 0.9856166618251658);
    NEAR(probe::hitProbability(12.5, 144, 5), 0.7565365010177725);
    NEAR(probe::hitProbability(8.33, inf, 10), 0.3229558326234727);
    NEAR(probe::hitProbability(100.0, 60, 10), 0.9999959586989711);
    NEAR(probe::hitProbability(1.0, 240, 10), 0.04123951764050483);
    auto rows = probe::levelDifficulty({20.8, 4.17, 50.0, 12.5}, {60, 144, 240, inf}, 10);
    NEAR(rows[0].nats, 3.2218568669572347);
    NEAR(rows[1].nats, 2.983655364368104);
    NEAR(rows[2].nats, 2.9455892242557056);
    NEAR(rows[3].nats, 2.92686041580521);
    NEAR(rows[0].impossibleShare, 0.81235);
    NEAR(rows[2].impossibleShare, 0.0);
    NEAR(rows[1].worstP, 0.16015836141807727);
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
