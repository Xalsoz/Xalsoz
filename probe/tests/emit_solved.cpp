// Решает игрушечный мир и пишет найденные клики в форматах мода (txt и gdr.json).
// Нужен тесту стыка C++ и Python. Аргументы: <каталог> <seed> <число требований>.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "core/solve_core.hpp"
#include "toy_world.hpp"

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    const std::string dir = argv[1];
    toyworld::World w = toyworld::randomWorld(static_cast<unsigned>(std::atoi(argv[2])), std::atoi(argv[3]), 5);
    toyworld::Result r = toyworld::solve(w, probe::SolveConfig{});
    if (!r.solved || !toyworld::replayPasses(w, r.macro)) return 1;
    {
        std::ofstream t(dir + "/solved.txt");
        probe::writeMacroText(t, r.macro);
    }
    {
        std::ofstream j(dir + "/solved.gdr.json");
        probe::writeMacroGdrJson(j, r.macro, 123, "Toy level");
    }
    for (const auto& in : r.macro) std::printf("%lld %d\n", static_cast<long long>(in.frame), in.down ? 1 : 0);
    return 0;
}
