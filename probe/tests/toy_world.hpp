// Игрушечный мир для тестов автопрохождения (solve_core).
//
// Уровень это цепочка обязательных переключений кнопки (нажать, отпустить,
// нажать, ...). У k-го есть окно [A, B]; у препятствия k момент O > B: если к
// нему нужное переключение не сделано вовремя или сделано не в окне, игрок
// умирает в момент O. Так же устроены шипы: смерть наступает у шипа, а не в
// момент неудачного прыжка.
//
// Ловушки: у части требований два окна. Широкое ведёт в тупик (смерть у
// «поздней» ловушки, которая наступает позже, чем у следующего честного
// препятствия, поэтому по времени смерти ловушка выглядит выгоднее), узкое
// ведёт дальше. Поиск должен сначала попасть в ловушку, затем откатиться.

#pragma once

#include "core/solve_core.hpp"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace toyworld {

using probe::Command;
using probe::Input;
using probe::SolveConfig;
using probe::Solver;
using probe::TickOutput;

struct Req {
    int64_t A = 0, B = 0, O = 0;
    bool branch = false;       // у требования два окна: [A,B] ведёт в ловушку, [A1,B1] дальше
    int64_t A1 = 0, B1 = 0;
    int64_t trapO = 0;
};

struct World {
    std::vector<Req> reqs;
    int64_t endFrame = 0;
};

struct State {
    int64_t frame = 0;
    size_t k = 0;
    bool held = false;
    int64_t deathAt = -1;
};

struct Sim {
    const World& w;
    State s;
    explicit Sim(const World& world) : w(world) {}

    void kill(int64_t at) {
        if (s.deathAt < 0 || at < s.deathAt) s.deathAt = at;
    }
    bool step() {
        ++s.frame;
        if (s.k < w.reqs.size()) {
            const Req& r = w.reqs[s.k];
            const int64_t last = r.branch ? std::max(r.B, r.B1) : r.B;
            if (s.frame > last) kill(r.O);
        }
        return s.deathAt >= 0 && s.frame >= s.deathAt;
    }
    void press(const Input& in) {
        if (in.down == s.held) return;  // лишнее нажатие ничего не меняет
        s.held = in.down;
        if (s.k >= w.reqs.size()) return;
        const Req& r = w.reqs[s.k];
        const int64_t f = in.frame;
        if (r.branch && f >= r.A && f <= r.B) {
            ++s.k;
            kill(r.trapO);
        } else if (r.branch && f >= r.A1 && f <= r.B1) {
            ++s.k;
        } else if (!r.branch && f >= r.A && f <= r.B) {
            ++s.k;
        } else {
            kill(r.O);
        }
    }
    bool ended() const { return s.frame >= w.endFrame; }
};

inline World randomWorld(unsigned seed, int n, int branchEvery) {
    std::mt19937 rng(seed);
    auto uni = [&](int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); };
    World w;
    int64_t t = 40;
    for (int k = 0; k < n; ++k) {
        Req r;
        r.A = t + uni(14, 70);
        r.B = r.A + uni(2, 30);
        int64_t last = r.B;
        if (branchEvery > 0 && k % branchEvery == branchEvery - 1) {
            r.branch = true;
            r.A1 = r.B + uni(6, 20);
            r.B1 = r.A1 + uni(3, 8);
            last = r.B1;
        }
        r.O = last + 1 + uni(2, 10);
        w.reqs.push_back(r);
        t = last;
    }
    for (size_t k = 0; k < w.reqs.size(); ++k) {
        Req& r = w.reqs[k];
        const int64_t nextO = k + 1 < w.reqs.size() ? w.reqs[k + 1].O : r.O + 100;
        r.trapO = nextO + 15;
    }
    w.endFrame = w.reqs.back().O + 120;
    return w;
}

struct Result {
    bool solved = false;
    bool aborted = false;
    std::string reason;
    std::vector<Input> macro;
    std::vector<int> widths;
    int64_t ticks = 0;
    int64_t backtracks = 0;
    size_t maxStack = 0;
    int64_t rebases = 0;
};

inline Result solve(const World& world, SolveConfig cfg) {
    Solver solver(cfg);
    Sim sim(world);
    std::vector<State> stack;
    Result r;
    auto handle = [&](TickOutput out) -> bool {
        for (int i = 0; i < out.popCheckpoints; ++i) stack.pop_back();
        if (out.command == Command::MarkCheckpoint) stack.push_back(sim.s);
        for (const Input& in : out.press) sim.press(in);
        bool stop = false;
        if (out.command == Command::RestoreCheckpoint) {
            sim.s = stack.back();
            TickOutput after = solver.afterRestore(sim.s.frame);
            for (const Input& in : after.press) sim.press(in);
            if (after.command == Command::Abort || after.command == Command::Finish) stop = true;
        }
        for (int i = 0; i < out.dropOldestCheckpoints; ++i) stack.erase(stack.begin());
        r.maxStack = std::max(r.maxStack, stack.size());
        if (out.command == Command::Finish || out.command == Command::Abort) stop = true;
        return stop;
    };
    for (int64_t guard = 0; guard < 300'000'000; ++guard) {
        const bool died = sim.step();
        ++r.ticks;
        if (handle(solver.tick(sim.s.frame, died, sim.ended()))) break;
    }
    r.solved = solver.solved();
    r.aborted = solver.aborted();
    r.reason = solver.abortReason();
    r.macro = solver.macro();
    r.widths = solver.widths();
    r.backtracks = solver.backtracks();
    r.rebases = solver.tickRebases();
    return r;
}

// Проигрывает найденные клики заново, без чекпоинтов. true, если уровень пройден без смерти.
inline bool replayPasses(const World& world, std::vector<Input> macro) {
    std::stable_sort(macro.begin(), macro.end(), [](const Input& a, const Input& b) { return a.frame < b.frame; });
    Sim sim(world);
    size_t pos = 0;
    for (int64_t guard = 0; guard < 50'000'000; ++guard) {
        if (sim.step()) return false;
        if (sim.ended()) return true;
        while (pos < macro.size() && macro[pos].frame <= sim.s.frame) {
            if (macro[pos].frame == sim.s.frame) sim.press(macro[pos]);
            ++pos;
        }
    }
    return false;
}

}  // namespace toyworld
