#include "solve_core.hpp"

#include <algorithm>
#include <limits>
#include <ostream>
#include <sstream>

namespace probe {

namespace {
constexpr int64_t kInf = std::numeric_limits<int64_t>::max() / 4;
constexpr int64_t kUnset = -1;
}  // namespace

Solver::Solver(SolveConfig config) : m_cfg(config) {
    m_cfg.horizon = std::max(m_cfg.horizon, 10);
    m_cfg.maxDelay = std::max(m_cfg.maxDelay, 1);
    m_cfg.scanStride = std::max(m_cfg.scanStride, 1);
    m_cfg.slack = std::max(m_cfg.slack, 0);
    m_cfg.maxAnchors = std::max(m_cfg.maxAnchors, 2);
}

std::string Solver::progressText() const {
    std::ostringstream s;
    s << "solving: reached tick " << m_reached << ", " << m_script.size() << " inputs, " << m_backtracks
      << " backtracks, " << m_ticks << " ticks played";
    return s.str();
}

TickOutput Solver::flush(TickOutput out) {
    out.popCheckpoints += m_pendingPops;
    out.dropOldestCheckpoints += m_pendingDrops;
    m_pendingPops = 0;
    m_pendingDrops = 0;
    return out;
}

TickOutput Solver::abort(std::string message) {
    m_state = State::Aborted;
    m_run.active = false;
    m_abortReason = std::move(message);
    TickOutput out;
    out.command = Command::Abort;
    out.message = m_abortReason;
    return out;
}

TickOutput Solver::finish() {
    m_state = State::Finished;
    m_run.active = false;
    TickOutput out;
    out.command = Command::Finish;
    return out;
}

void Solver::noteProgress(int64_t t0) {
    m_reached = std::max(m_reached, t0);
    if (t0 > m_bestT0 + 60) {
        m_bestT0 = t0;
        m_best = m_script;
        m_bestWidths = m_widths;
    }
}

void Solver::applyInputs(int64_t frame, std::vector<Input>& press) {
    while (m_run.scriptPos < m_script.size() && m_script[m_run.scriptPos].frame <= frame) {
        if (m_script[m_run.scriptPos].frame == frame) press.push_back(m_script[m_run.scriptPos]);
        ++m_run.scriptPos;
    }
    if (m_run.extraTick == frame) press.push_back(Input{frame, m_run.extraDown, false});
}

TickOutput Solver::beginRun(Run run) {
    run.active = true;
    m_run = run;
    m_expectRestore = true;
    TickOutput out;
    out.command = Command::RestoreCheckpoint;
    return out;
}

TickOutput Solver::afterRestore(int64_t frame) {
    TickOutput out;
    if (done()) {
        out.command = aborted() ? Command::Abort : Command::Finish;
        out.message = m_abortReason;
        return out;
    }
    if (!m_expectRestore || !m_run.active || m_anchors.empty()) return out;
    m_expectRestore = false;
    Anchor& a = m_anchors.back();
    if (frame != a.tick) {
        // Счётчик тиков после загрузки чекпоинта не совпал с тем, что был при его постановке.
        // Подправляем, но считаем: если таких случаев много, проверь frame-source и frame-offset.
        a.tick = frame;
        ++m_rebases;
    }
    m_lastFrame = frame;
    m_run.scriptPos = static_cast<size_t>(
        std::lower_bound(m_script.begin(), m_script.end(), frame,
                         [](const Input& in, int64_t f) { return in.frame < f; }) -
        m_script.begin());
    if (a.held) out.press.push_back(Input{frame, true, false});
    applyInputs(frame, out.press);
    return out;
}

TickOutput Solver::tick(int64_t frame, bool died, bool ended) {
    if (done()) {
        TickOutput out;
        out.command = aborted() ? Command::Abort : Command::Finish;
        out.message = m_abortReason;
        return out;
    }
    m_lastFrame = frame;
    if (++m_ticks > m_cfg.maxTicks) return abort("Tick budget exhausted. " + progressText());

    if (m_state == State::Boot) {
        if (frame < m_cfg.minCheckpointFrame) return {};
        m_state = State::Running;
        TickOutput out;
        makeNode(frame, true, false, out);
        return flush(out);
    }
    if (!m_run.active || m_expectRestore) return {};

    if (m_run.purpose == Purpose::Commit && !died && !ended && frame >= m_run.limit) return flush(reachTarget(frame));
    if (died || ended || frame >= m_run.limit) return flush(finishRun(died, ended, frame));

    TickOutput out;
    applyInputs(frame, out.press);
    return out;
}

TickOutput Solver::finishRun(bool died, bool ended, int64_t frame) {
    const Purpose purpose = m_run.purpose;
    m_run.active = false;
    switch (purpose) {
        case Purpose::Baseline: return onBaseline(died, ended, frame);
        case Purpose::Candidate: return onCandidate(died, ended, frame);
        case Purpose::Commit:
            if (ended && !died) return finish();
            // Клик, который пробой выживал, при проигрыше с чекпоинта убил игрока:
            // чекпоинт восстанавливается неточно. Берём следующий вариант.
            if (++m_replayFails >= m_cfg.maxReplayFailures)
                return abort("Replaying from a checkpoint keeps dying where the probe survived: checkpoints restore an inexact state. " +
                             progressText());
            return chooseNext();
    }
    return {};
}

TickOutput Solver::onBaseline(bool died, bool ended, int64_t frame) {
    if (ended && !died) return finish();
    if (!died) return advanceSafe(frame);
    Node& n = top();
    n.scanned = true;
    n.tNone = frame;
    if (n.tNone - n.t0 - 1 > m_cfg.maxDelay) {
        // Препятствие далеко: сначала идём без клика, пока до него не останется maxDelay тиков.
        // Нужное состояние берём откатом к чекпоинту и проигрыванием до этого тика.
        n.scanned = false;
        Run r;
        r.purpose = Purpose::Commit;
        r.waitOnly = true;
        r.target = n.tNone - m_cfg.maxDelay;
        r.limit = r.target;
        return beginRun(r);
    }
    n.dMax = static_cast<int>(n.tNone - n.t0 - 1);
    if (n.dMax < 1) return chooseNext();  // умрёт раньше, чем успеем что-то нажать: тупик
    n.T.assign(static_cast<size_t>(n.dMax) + 1, kUnset);
    n.pending.clear();
    for (int d = 1; d <= n.dMax; d += m_cfg.scanStride) n.pending.push_back(d);
    if (n.pending.back() != n.dMax) n.pending.push_back(n.dMax);
    n.pendingPos = 0;
    n.refining = false;
    return continueScan();
}

TickOutput Solver::onCandidate(bool died, bool ended, int64_t frame) {
    Node& n = top();
    n.T[static_cast<size_t>(m_run.d)] = (died && !ended) ? frame : kInf;
    return continueScan();
}

TickOutput Solver::continueScan() {
    Node& n = top();
    for (;;) {
        while (n.pendingPos < n.pending.size()) {
            const int d = n.pending[n.pendingPos++];
            if (n.T[static_cast<size_t>(d)] != kUnset) continue;
            Run r;
            r.purpose = Purpose::Candidate;
            r.d = d;
            r.extraTick = n.t0 + d;
            r.extraDown = !n.held;
            r.limit = r.extraTick + m_cfg.horizon;
            return beginRun(r);
        }
        if (!n.refining) {
            n.refining = true;
            n.pending.clear();
            n.pendingPos = 0;
            const int s = m_cfg.scanStride;
            if (s > 1) {
                auto improving = [&](int d) {
                    return d >= 1 && d <= n.dMax && n.T[static_cast<size_t>(d)] != kUnset &&
                           n.T[static_cast<size_t>(d)] > n.tNone + m_cfg.slack;
                };
                for (int d = 1; d <= n.dMax; ++d) {
                    if (n.T[static_cast<size_t>(d)] != kUnset) continue;
                    for (int e = std::max(1, d - s + 1); e <= std::min(n.dMax, d + s - 1); ++e) {
                        if (improving(e)) {
                            n.pending.push_back(d);
                            break;
                        }
                    }
                }
            }
            if (!n.pending.empty()) continue;
        }
        finalizeOptions(n);
        return chooseNext();
    }
}

void Solver::finalizeOptions(Node& n) {
    n.options.clear();
    n.nextOption = 0;
    auto improving = [&](int d) {
        return n.T[static_cast<size_t>(d)] != kUnset && n.T[static_cast<size_t>(d)] > n.tNone + m_cfg.slack;
    };
    std::vector<Option> primary;
    int d = 1;
    while (d <= n.dMax) {
        if (!improving(d)) {
            ++d;
            continue;
        }
        int end = d;
        while (end + 1 <= n.dMax && improving(end + 1)) ++end;
        int64_t best = 0;
        for (int k = d; k <= end; ++k) best = std::max(best, n.T[static_cast<size_t>(k)]);
        // Самый широкий отрезок внутри компоненты, где смерть наступает не раньше best - slack.
        int bestLo = -1, bestHi = -1;
        int k = d;
        while (k <= end) {
            if (n.T[static_cast<size_t>(k)] < best - m_cfg.slack) {
                ++k;
                continue;
            }
            int j = k;
            while (j + 1 <= end && n.T[static_cast<size_t>(j + 1)] >= best - m_cfg.slack) ++j;
            if (bestLo < 0 || j - k > bestHi - bestLo) {
                bestLo = k;
                bestHi = j;
            }
            k = j + 1;
        }
        if (best == kInf || best - n.t0 >= m_cfg.deadMargin) {
            Option o;
            o.lo = n.t0 + bestLo;
            o.hi = n.t0 + bestHi;
            o.width = bestHi - bestLo + 1;
            o.tick = n.t0 + (bestLo + bestHi) / 2;
            o.score = best;
            primary.push_back(o);
        }
        d = end + 1;
    }
    std::stable_sort(primary.begin(), primary.end(), [](const Option& a, const Option& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.width > b.width;
    });
    n.options = primary;
    // Запасные точки внутри самых широких окон: на случай, если середина ведёт в тупик.
    for (size_t i = 0; i < primary.size() && i < 3; ++i) {
        const Option& o = primary[i];
        if (o.width < 6) continue;
        for (int64_t t : {o.lo + o.width / 4, o.hi - o.width / 4}) {
            if (t == o.tick) continue;
            Option x = o;
            x.tick = t;
            n.options.push_back(x);
        }
    }
}

void Solver::popTopNode() {
    if (m_nodes.empty()) return;
    if (m_nodes.back().ownsAnchor && !m_anchors.empty()) {
        m_anchors.pop_back();
        ++m_pendingPops;
    }
    m_nodes.pop_back();
}

TickOutput Solver::chooseNext() {
    for (;;) {
        if (m_nodes.empty()) {
            return abort("No route found: stuck near tick " + std::to_string(m_reached) + ". " + progressText());
        }
        Node& n = top();
        if (n.nextOption >= n.options.size()) {
            if (++m_backtracks > m_cfg.maxBacktracks) return abort("Backtrack budget exhausted. " + progressText());
            popTopNode();
            continue;
        }
        const Option op = n.options[n.nextOption++];
        m_script.resize(n.scriptLen);
        m_widths.resize(n.scriptLen);
        const bool down = !n.held;
        m_script.push_back(Input{op.tick, down, false});
        m_widths.push_back(op.width);
        Run r;
        r.purpose = Purpose::Commit;
        r.target = down ? op.tick : op.tick + 1;  // после отпускания чекпоинт на следующем тике
        r.limit = r.target;
        return beginRun(r);
    }
}

TickOutput Solver::reachTarget(int64_t frame) {
    // Проигрывание дошло до нового узла. Чекпоинт ставится до нажатия этого тика.
    m_replayFails = 0;
    TickOutput out;
    applyInputs(frame, out.press);
    if (m_run.waitOnly) {
        // Шаг без клика: чекпоинт, пока кнопка отпущена (при долгом удержании всё равно).
        const bool held = !m_script.empty() && m_script.back().down;
        const bool mark = !held || frame - m_anchors.back().tick > m_cfg.maxTail;
        makeNode(frame, mark, held, out);
        return out;
    }
    makeNode(frame, true, false, out);
    return out;
}

TickOutput Solver::advanceSafe(int64_t frame) {
    // Горизонт пережит без клика: идём вперёд. Чекпоинт ставим, пока кнопка отпущена;
    // при долгом удержании (корабль, волна) всё же ставим, помня, что кнопка зажата.
    TickOutput out;
    const bool held = !m_nodes.empty() && top().held;
    const bool mark = !held || frame - m_anchors.back().tick > m_cfg.maxTail;
    makeNode(frame, mark, held, out);
    return flush(out);
}

void Solver::makeNode(int64_t frame, bool mark, bool anchorHeld, TickOutput& out) {
    // Предыдущий безопасный узел без вариантов не нужен: заменяем его новым.
    if (!m_nodes.empty() && !top().scanned && (mark || !top().ownsAnchor)) popTopNode();

    Node n;
    n.t0 = frame;
    n.held = !m_script.empty() && m_script.back().down;
    n.scriptLen = m_script.size();
    if (mark) {
        Anchor a;
        a.id = m_nextAnchorId++;
        a.tick = frame;
        a.held = anchorHeld;
        m_anchors.push_back(a);
        n.anchorId = a.id;
        n.ownsAnchor = true;
        out.command = Command::MarkCheckpoint;
        if (static_cast<int>(m_anchors.size()) > m_cfg.maxAnchors) {
            const int64_t dropped = m_anchors.front().id;
            m_anchors.erase(m_anchors.begin());
            ++m_pendingDrops;
            while (!m_nodes.empty() && m_nodes.front().anchorId <= dropped) m_nodes.pop_front();
        }
    } else {
        n.anchorId = m_anchors.back().id;
    }
    m_nodes.push_back(std::move(n));
    noteProgress(frame);

    Run r;
    r.active = true;
    r.purpose = Purpose::Baseline;
    r.limit = frame + m_cfg.horizon;
    r.scriptPos = m_script.size();
    m_run = r;
    m_expectRestore = false;
}

void writeMacroText(std::ostream& out, const std::vector<Input>& macro) {
    out << "tps=240\n";
    for (const Input& in : macro) out << in.frame << ' ' << (in.down ? 1 : 0) << ' ' << (in.player2 ? 1 : 0) << '\n';
}

void writeMacroGdrJson(std::ostream& out, const std::vector<Input>& macro, int levelId, const std::string& levelName) {
    std::string name;
    for (char c : levelName) {
        if (c == '"' || c == '\\') name += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) name += c;
    }
    out << "{\"gameVersion\":2.2081,\"version\":1.0,\"framerate\":240.0,\"bot\":{\"name\":\"gddiff\",\"version\":\"0\"},"
        << "\"level\":{\"id\":" << levelId << ",\"name\":\"" << name << "\"},\"inputs\":[";
    for (size_t i = 0; i < macro.size(); ++i) {
        if (i) out << ',';
        out << "{\"frame\":" << macro[i].frame << ",\"btn\":1,\"2p\":" << (macro[i].player2 ? "true" : "false")
            << ",\"down\":" << (macro[i].down ? "true" : "false") << '}';
    }
    out << "]}\n";
}

}  // namespace probe
