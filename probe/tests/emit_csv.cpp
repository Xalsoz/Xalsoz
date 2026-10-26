// Прогоняет ядро на игрушечном уровне и пишет CSV в формате мода.
// Нужен тесту стыка C++ и Python (tests/test_probe.py).
// Аргументы: <путь к csv> <seed> <число вводов>; печатает ожидаемые окна в stdout.

#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "core/probe_core.hpp"
#include "toy_sim.hpp"

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    const unsigned seed = static_cast<unsigned>(std::atoi(argv[2]));
    const int n = std::atoi(argv[3]);
    toy::ToyLevel level = toy::randomLevel(seed, n, 40);
    probe::Config cfg;
    probe::Controller ctl(level.macro, cfg);
    // Тот же цикл, что в toy::run, но с доступом к контроллеру для записи CSV.
    toy::ToySim sim(level);
    std::optional<toy::ToyState> checkpoint;
    for (int64_t guard = 0; guard < 50'000'000; ++guard) {
        const bool died = sim.step();
        probe::TickOutput out = ctl.tick(sim.s.frame, died, sim.ended());
        if (out.command == probe::Command::MarkCheckpoint) checkpoint = sim.s;
        for (const auto& in : out.press) sim.press(in);
        if (out.command == probe::Command::RestoreCheckpoint) {
            sim.s = *checkpoint;
            probe::TickOutput after = ctl.afterRestore(sim.s.frame);
            for (const auto& in : after.press) sim.press(in);
            if (after.command == probe::Command::Abort) out = after;
        }
        if (out.command == probe::Command::Finish || out.command == probe::Command::Abort) break;
    }
    std::ofstream csv(argv[1]);
    probe::writeCsv(csv, ctl);
    for (size_t k = 0; k < level.macro.size(); ++k)
        std::printf("%lld %d %d\n", static_cast<long long>(level.macro[k].frame), -level.lo[k], level.hi[k]);
    return ctl.aborted() ? 1 : 0;
}
