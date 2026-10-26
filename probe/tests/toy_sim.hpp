// Игрушечная «физика» для тестов ядра без игры.
//
// В игрушечном уровне у каждого ввода k есть известное окно [lo_k, hi_k] тиков
// относительно кадра макроса. Ввод вне окна убивает игрока не сразу, а через
// delay_k тиков (как прыжок, после которого врезаешься в шип позже). Если ввод
// не нажат до конца окна, смерть тоже наступает через delay_k. Отпускание без
// удержания тоже смертельно. Чекпоинт копирует состояние целиком (или с
// заданной ошибкой, для проверки защит).

#pragma once

#include "core/probe_core.hpp"

#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace toy {

using probe::Command;
using probe::Config;
using probe::Controller;
using probe::Input;
using probe::TickOutput;
using probe::Window;

struct ToyLevel {
    std::vector<Input> macro;
    std::vector<int> lo, hi, delay;
    int64_t endFrame = 0;
};

struct ToyState {
    int64_t frame = 0;
    size_t applied = 0;
    int64_t deathAt = -1;
    int drift = 0;  // ошибка восстановления: сдвиг всех последующих вводов
    bool held[2] = {false, false};
};

enum class Fault { None, Drift, DropHold };

struct ToySim {
    const ToyLevel& level;
    ToyState s;

    explicit ToySim(const ToyLevel& l) : level(l) {}

    void scheduleDeath(int64_t at) {
        if (s.deathAt < 0 || at < s.deathAt) s.deathAt = at;
    }

    // Физика одного тика. Возвращает true, если игрок умер на этом тике.
    bool step() {
        ++s.frame;
        // Пропущенный ввод: окно закончилось, а нажатия нет.
        if (s.applied < level.macro.size()) {
            const size_t k = s.applied;
            if (s.frame > level.macro[k].frame + level.hi[k]) scheduleDeath(level.macro[k].frame + level.hi[k] + 1 + level.delay[k]);
        }
        return s.deathAt >= 0 && s.frame >= s.deathAt;
    }

    void press(const Input& in) {
        // Отпускание без удержания в игрушке смертельно (как сломанный ввод).
        bool& held = s.held[in.player2 ? 1 : 0];
        if (!in.down && !held) scheduleDeath(in.frame);
        held = in.down;
        if (s.applied >= level.macro.size()) return;
        const size_t k = s.applied++;
        const int64_t offset = in.frame + s.drift - level.macro[k].frame;
        if (offset < level.lo[k] || offset > level.hi[k]) scheduleDeath(in.frame + level.delay[k]);
    }

    bool ended() const { return s.frame >= level.endFrame; }
};

struct RunResult {
    std::vector<Window> windows;
    bool aborted = false;
    std::string reason;
    int64_t ticks = 0;
    int64_t trials = 0;
    int heldCheckpoints = 0;  // чекпоинты, поставленные при зажатой кнопке
};

inline RunResult run(const ToyLevel& level, Config cfg, Fault fault = Fault::None) {
    Controller ctl(level.macro, cfg);
    ToySim sim(level);
    std::optional<ToyState> checkpoint;
    RunResult r;
    for (int64_t guard = 0; guard < 50'000'000; ++guard) {
        const bool died = sim.step();
        ++r.ticks;
        TickOutput out = ctl.tick(sim.s.frame, died, sim.ended());
        if (out.command == Command::MarkCheckpoint) {
            checkpoint = sim.s;
            if (sim.s.held[0] || sim.s.held[1]) ++r.heldCheckpoints;
        }
        for (const Input& in : out.press) sim.press(in);
        if (out.command == Command::RestoreCheckpoint) {
            if (cfg.exactRestart) {
                sim.s = ToyState{};
            } else {
                sim.s = *checkpoint;
                if (fault == Fault::Drift) sim.s.drift = 1;
                if (fault == Fault::DropHold) sim.s.held[0] = sim.s.held[1] = false;
            }
            TickOutput after = ctl.afterRestore(sim.s.frame);
            for (const Input& in : after.press) sim.press(in);
            if (after.command == Command::Abort) out = after;
        }
        if (out.command == Command::Finish || out.command == Command::Abort) {
            r.aborted = out.command == Command::Abort;
            r.reason = out.message;
            break;
        }
    }
    r.windows = ctl.windows();
    r.trials = ctl.totalTrials();
    return r;
}

inline ToyLevel randomLevel(unsigned seed, int n, int maxDelay) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> gap(3, 70), lo(-40, 0), hi(0, 40), delay(0, maxDelay);
    ToyLevel level;
    int64_t frame = 30;
    for (int i = 0; i < n; ++i) {
        frame += gap(rng);
        level.macro.push_back(Input{frame, i % 2 == 0, false});
        level.lo.push_back(lo(rng));
        level.hi.push_back(hi(rng));
        level.delay.push_back(delay(rng));
    }
    level.endFrame = frame + 300;
    return level;
}

}  // namespace toy
