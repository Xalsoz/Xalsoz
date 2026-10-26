#include "solve_core.hpp"

#include <algorithm>
#include <cstdio>
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
    if (!n.held && n.tNone - n.t0 - 1 > m_cfg.maxDelay) {
        // Препятствие далеко: сначала идём без клика, пока до него не останется maxDelay тиков.
        // Нужное состояние берём откатом к чекпоинту и проигрыванием до этого тика.
        // (При удержании кнопки не ждём: отпустить её обычно надо вскоре.)
        n.scanned = false;
        Run r;
        r.purpose = Purpose::Commit;
        r.waitOnly = true;
        r.target = n.tNone - m_cfg.maxDelay;
        r.limit = r.target;
        return beginRun(r);
    }
    n.dMax = static_cast<int>(std::min<int64_t>(m_cfg.maxDelay, n.tNone - n.t0 - 1));
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
    // Что считать подходящим кликом. Нажатие должно улучшать исход: игрок доживает дольше, чем
    // если ничего не делать. Отпускание часто ничего не меняет (у куба до следующего шипа
    // живёшь одинаково), но без него не нажать снова; поэтому у отпускания годятся все
    // клики, не ухудшающие исход.
    auto good = [&](int64_t t) { return n.held ? t >= n.tNone - m_cfg.slack : t > n.tNone + m_cfg.slack; };
    std::vector<std::pair<int, int64_t>> ev;  // измеренные (d, T) по возрастанию d
    for (int d = 1; d <= n.dMax; ++d)
        if (n.T[static_cast<size_t>(d)] != kUnset) ev.emplace_back(d, n.T[static_cast<size_t>(d)]);
    const int link = m_cfg.scanStride;  // соседние измеренные d склеиваем, если между ними не больше шага
    std::vector<Option> primary;
    size_t i = 0;
    while (i < ev.size()) {
        if (!good(ev[i].second)) {
            ++i;
            continue;
        }
        // Серия: подряд идущие подходящие d, у которых тик смерти меняется не больше чем на slack
        // между соседями. Разные «площадки» (например, ловушка и честный путь) остаются разными сериями.
        size_t end = i;
        while (end + 1 < ev.size() && good(ev[end + 1].second) && ev[end + 1].first - ev[end].first <= link &&
               std::llabs(ev[end + 1].second - ev[end].second) <= std::max<int64_t>(m_cfg.slack, 0) &&
               !(ev[end].second == kInf) == !(ev[end + 1].second == kInf))
            ++end;
        int64_t best = 0;
        for (size_t k = i; k <= end; ++k) best = std::max(best, ev[k].second);
        // Самый широкий отрезок серии, где смерть наступает не раньше best - slack.
        int bestLo = -1, bestHi = -1;
        size_t k = i;
        while (k <= end) {
            if (ev[k].second < best - m_cfg.slack) {
                ++k;
                continue;
            }
            size_t j = k;
            while (j + 1 <= end && ev[j + 1].second >= best - m_cfg.slack && ev[j + 1].first - ev[j].first <= link) ++j;
            if (bestLo < 0 || ev[j].first - ev[k].first > bestHi - bestLo) {
                bestLo = ev[k].first;
                bestHi = ev[j].first;
            }
            k = j + 1;
        }
        if (best == kInf || best - n.t0 >= m_cfg.deadMargin) {
            Option o;
            o.lo = n.t0 + bestLo;
            o.hi = n.t0 + bestHi;
            o.width = bestHi - bestLo + 1;
            // Нажатие в середину окна (так прощает больше всего). Отпускание в широком окне: вскоре
            // после начала, чтобы не держать кнопку без нужды.
            o.tick = n.held ? o.lo + std::min<int64_t>((o.width - 1) / 2, 6) : n.t0 + (bestLo + bestHi) / 2;
            o.score = best;
            primary.push_back(o);
        }
        i = end + 1;
    }
    std::stable_sort(primary.begin(), primary.end(), [](const Option& a, const Option& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.width > b.width;
    });
    if (n.held) {
        // Отпускание, не меняющее исход, равноценно в любой точке: оставляем самый широкий из таких
        // вариантов одним и в самом конце. Иначе в безвыходной позиции перебирались бы все сочетания.
        std::vector<Option> improving;
        const Option* widest = nullptr;
        for (const Option& o : primary) {
            if (o.score > n.tNone + m_cfg.slack) {
                improving.push_back(o);
            } else if (!widest || o.width > widest->width) {
                widest = &o;
            }
        }
        primary = improving;
        if (widest) primary.push_back(*widest);
    }
    n.options = primary;
#ifdef GDDIFF_DEBUG
    std::fprintf(stderr, "node t0=%lld held=%d tNone=%lld dMax=%d options:", (long long)n.t0, (int)n.held, (long long)n.tNone, n.dMax);
    for (auto& o : primary) std::fprintf(stderr, " [tick %lld w=%d score=%lld]", (long long)o.tick, o.width, (long long)o.score);
    std::fprintf(stderr, "\n");
#endif
    // Запасные точки внутри самых широких окон: на случай, если середина ведёт в тупик.
    for (size_t q = 0; q < primary.size() && q < 3; ++q) {
        const Option& o = primary[q];
        if (o.width < 6 || (n.held && o.score <= n.tNone + m_cfg.slack)) continue;
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
#ifdef GDDIFF_DEBUG
        std::fprintf(stderr, "choose: top t0=%lld held=%d opt=%zu/%zu waited=%d tNone=%lld dMax=%d scanned=%d\n", (long long)n.t0, (int)n.held, n.nextOption, n.options.size(), (int)n.waited, (long long)n.tNone, n.dMax, (int)n.scanned);
#endif
        if (n.nextOption >= n.options.size()) {
            if (n.held && n.scanned && !n.waited && n.tNone - n.t0 - 1 > n.dMax) {
                // Кнопку надо держать дольше (корабль, волна): отпускание сейчас не помогает.
                // Идём вперёд с зажатой кнопкой и решаем заново.
                n.waited = true;
                m_script.resize(n.scriptLen);  // отбросить клик прошлого варианта
                m_widths.resize(n.scriptLen);
                Run w;
                w.purpose = Purpose::Commit;
                w.waitOnly = true;
                w.target = n.t0 + std::max(1, m_cfg.maxDelay / 2);
                w.limit = w.target;
                return beginRun(w);
            }
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
