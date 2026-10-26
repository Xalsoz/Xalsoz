#include "probe_core.hpp"

#include <algorithm>
#include <limits>
#include <ostream>
#include <sstream>
#include <utility>

// Сообщения на английском: шрифты GD не содержат кириллицы.

namespace probe {

namespace {
constexpr size_t kNoActive = std::numeric_limits<size_t>::max();
}

Controller::Controller(std::vector<Input> macro, Config config) : m_macro(std::move(macro)), m_cfg(config) {
    std::stable_sort(m_macro.begin(), m_macro.end(), [](const Input& a, const Input& b) { return a.frame < b.frame; });
    m_cfg.maxShift = std::max(0, m_cfg.maxShift);
    m_cfg.horizon = std::max(1, m_cfg.horizon);
    m_cfg.stride = std::max(1, m_cfg.stride);
    m_cfg.minCheckpointFrame = std::max(1, m_cfg.minCheckpointFrame);

    m_windows.resize(m_macro.size());
    for (size_t i = 0; i < m_macro.size(); ++i) {
        m_windows[i].index = i;
        m_windows[i].input = m_macro[i];
    }
    const int64_t n = static_cast<int64_t>(m_macro.size());
    const int64_t first = std::max<int64_t>(0, m_cfg.firstIndex);
    const int64_t last = m_cfg.lastIndex < 0 ? n - 1 : std::min<int64_t>(n - 1, m_cfg.lastIndex);
    for (int64_t i = first; i <= last; i += m_cfg.stride) m_targets.push_back(static_cast<size_t>(i));

    planTarget();
    if (m_targetPos >= m_targets.size()) m_phase = Phase::Finished;
}

size_t Controller::activeIndex() const {
    return m_phase == Phase::Trial ? m_targets[m_targetPos] : kNoActive;
}

void Controller::collect(int64_t fromExclusive, int64_t toInclusive, std::vector<Input>& out) const {
    if (toInclusive <= fromExclusive) return;
    const size_t active = activeIndex();
    const int64_t lo = fromExclusive + 1 - m_cfg.maxShift;
    const int64_t hi = toInclusive + m_cfg.maxShift;
    auto it = std::lower_bound(m_macro.begin(), m_macro.end(), lo, [](const Input& in, int64_t f) { return in.frame < f; });
    std::vector<std::pair<int64_t, size_t>> hits;
    for (; it != m_macro.end() && it->frame <= hi; ++it) {
        const size_t k = static_cast<size_t>(it - m_macro.begin());
        const int64_t ef = it->frame + (k == active ? m_shift : 0);
        if (ef > fromExclusive && ef <= toInclusive) hits.emplace_back(ef, k);
    }
    std::sort(hits.begin(), hits.end());
    for (const auto& [ef, k] : hits) {
        Input in = m_macro[k];
        in.frame = ef;
        out.push_back(in);
    }
}

bool Controller::heldBefore(int64_t tick, bool player2, int64_t* pressFrame) const {
    // Состояние кнопки в момент установки чекпоинта на тике tick: учитываются
    // вводы строго до tick (вводы самого тика нажимаются после установки).
    auto it = std::lower_bound(m_macro.begin(), m_macro.end(), tick, [](const Input& in, int64_t f) { return in.frame < f; });
    while (it != m_macro.begin()) {
        --it;
        if (it->player2 != player2) continue;
        if (pressFrame) *pressFrame = it->frame;
        return it->down;
    }
    return false;
}

int64_t Controller::releasedTickAtOrBefore(int64_t tick) const {
    // Самый поздний тик <= tick, на котором ни одна кнопка не зажата: начало
    // текущего удержания (тик нажатия), с повтором для второго игрока.
    int64_t t = tick;
    for (int iter = 0; iter < 16; ++iter) {
        int64_t earliest = t;
        bool any = false;
        for (bool p2 : {false, true}) {
            int64_t press = t;
            if (heldBefore(t, p2, &press)) {
                any = true;
                earliest = std::min(earliest, press);
            }
        }
        if (!any) return t;
        t = earliest;
    }
    return -1;
}

void Controller::planTarget() {
    const int64_t n = static_cast<int64_t>(m_macro.size());
    while (m_targetPos < m_targets.size()) {
        Window& w = current();
        const auto i = static_cast<int64_t>(w.index);
        const int64_t f = w.input.frame;
        int64_t early = m_cfg.maxShift;
        int64_t late = m_cfg.maxShift;
        // Сдвинутый ввод не может перейти соседний ввод.
        if (i > 0) early = std::min(early, std::max<int64_t>(0, f - m_macro[i - 1].frame - 1));
        if (i + 1 < n) late = std::min(late, std::max<int64_t>(0, m_macro[i + 1].frame - f - 1));
        early = std::min(early, std::max<int64_t>(0, f - m_cfg.minCheckpointFrame));
        w.earlyLimit = static_cast<int>(early);
        w.lateLimit = static_cast<int>(late);

        // Чекпоинт нужен не позже самого раннего тика, где может оказаться сдвинутый ввод.
        const int64_t earliest = f - early;
        int64_t planned = earliest;
        if (!m_cfg.exactRestart && m_cfg.checkpointOnRelease) {
            // Чекпоинты GD могут не восстанавливать зажатую кнопку: ставим их там, где она отпущена.
            const int64_t released = releasedTickAtOrBefore(earliest);
            if (released >= m_cfg.minCheckpointFrame) planned = released;
        }
        if (!m_cfg.exactRestart && m_haveCheckpoint && planned < m_checkpointFrame) planned = m_checkpointFrame;

        const bool tooEarly = planned < m_cfg.minCheckpointFrame;
        const bool behindCheckpoint = !m_cfg.exactRestart && m_haveCheckpoint && earliest < m_checkpointFrame;
        if (tooEarly || behindCheckpoint) {
            w.status = Status::Skipped;
            ++m_targetPos;
            continue;
        }
        m_planned = planned;
        return;
    }
}

void Controller::enterBaseline(bool afterRestore) {
    m_phase = Phase::Trial;
    m_side = Side::Baseline;
    m_baselineAfterRestore = afterRestore;
    m_shift = 0;
    m_trialEnd = current().input.frame + m_cfg.horizon;
}

TickOutput Controller::tick(int64_t frame, bool died, bool ended) {
    TickOutput out;
    m_tickFrame = frame;
    if (m_phase == Phase::Finished) {
        out.command = Command::Finish;
        return out;
    }
    if (m_phase == Phase::Aborted) {
        out.command = Command::Abort;
        out.message = m_abortReason;
        return out;
    }

    if (m_phase == Phase::Advance) {
        if (died) {
            std::ostringstream msg;
            msg << "Player died at tick " << frame << " while replaying the original macro before input #"
                << current().index << ". The macro does not pass here";
            if (!m_cfg.exactRestart) msg << ", or checkpoints restore the state imprecisely (try exact-restart)";
            return abort(msg.str());
        }
        if (ended) return finish();
        while (frame >= m_planned) {
            Window& w = current();
            if (frame > m_planned) {
                // Нужный тик пропущен: сужаем ранний предел или пропускаем цель.
                const int64_t early = w.input.frame - frame;
                const bool behind = !m_cfg.exactRestart && m_haveCheckpoint && frame <= m_checkpointFrame;
                if (early < 0 || behind) {
                    w.status = Status::Skipped;
                    ++m_targetPos;
                    if (m_targetPos >= m_targets.size()) return finish();
                    planTarget();
                    if (m_targetPos >= m_targets.size()) return finish();
                    continue;
                }
                w.earlyLimit = std::min<int>(w.earlyLimit, static_cast<int>(early));
                m_planned = frame;
            }
            if (!m_cfg.exactRestart) {
                out.command = Command::MarkCheckpoint;
                m_haveCheckpoint = true;
                m_checkpointFrame = frame;
                m_markLastFrame = m_lastFrame;
            }
            // Базовая проба идёт без перезапуска: это просто продолжение исходного маршрута.
            enterBaseline(false);
            break;
        }
        collect(m_lastFrame, frame, out.press);
        m_lastFrame = frame;
        return out;
    }

    // Phase::Trial
    if (died) return onOutcome(false);
    if (ended || frame >= m_trialEnd) return onOutcome(true);
    collect(m_lastFrame, frame, out.press);
    m_lastFrame = frame;
    return out;
}

TickOutput Controller::afterRestore(int64_t frame) {
    TickOutput out;
    if (done()) {
        out.command = aborted() ? Command::Abort : Command::Finish;
        out.message = m_abortReason;
        return out;
    }
    if (m_cfg.exactRestart) {
        m_lastFrame = -1;
        return out;
    }
    if (!m_haveCheckpoint) return abort("Restore requested without a checkpoint");
    if (frame != m_checkpointFrame) {
        std::ostringstream msg;
        msg << "After restoring the checkpoint the tick is " << frame << ", expected " << m_checkpointFrame
            << ". Change frame-source in the mod settings or use exact-restart";
        return abort(msg.str());
    }
    // Следующая цель использует тот же чекпоинт: базовая проба начинается прямо отсюда.
    if (m_phase == Phase::Advance && m_planned == m_checkpointFrame) enterBaseline(true);
    // Клики тика чекпоинта были нажаты после его установки: нажимаем их снова.
    m_lastFrame = m_markLastFrame;
    collect(m_lastFrame, frame, out.press);
    m_lastFrame = frame;
    return out;
}

bool Controller::startSide(Side side) {
    Window& w = current();
    m_side = side;
    m_lastPass = 0;
    m_firstFail = -1;
    const int limit = side == Side::Early ? w.earlyLimit : w.lateLimit;
    if (limit == 0) {
        if (side == Side::Early) {
            w.early = 0;
            return startSide(Side::Late);
        }
        w.late = 0;
        return false;
    }
    m_magnitude = 1;
    m_shift = side == Side::Early ? -m_magnitude : m_magnitude;
    m_trialEnd = w.input.frame + std::max(0, m_shift) + m_cfg.horizon;
    return true;
}

bool Controller::prepareNextTrial(bool lastPassed) {
    Window& w = current();
    if (lastPassed)
        m_lastPass = m_magnitude;
    else
        m_firstFail = m_magnitude;
    const int limit = m_side == Side::Early ? w.earlyLimit : w.lateLimit;

    int next;
    bool sideDone = false;
    if (m_firstFail < 0) {
        // Разгон: 1, 2, 4, 8... до первой смерти или до предела.
        if (m_lastPass >= limit)
            sideDone = true;
        else
            next = std::min(limit, std::max(1, m_lastPass * 2));
    } else {
        next = (m_lastPass + m_firstFail) / 2;
        if (next <= m_lastPass) sideDone = true;
    }

    if (sideDone) {
        if (m_side == Side::Early) {
            w.early = m_lastPass;
            return startSide(Side::Late);
        }
        w.late = m_lastPass;
        return false;
    }
    m_magnitude = next;
    m_shift = m_side == Side::Early ? -m_magnitude : m_magnitude;
    m_trialEnd = w.input.frame + std::max(0, m_shift) + m_cfg.horizon;
    return true;
}

TickOutput Controller::onOutcome(bool passed) {
    Window& w = current();
    ++w.trials;
    ++m_totalTrials;
    switch (m_side) {
        case Side::Baseline:
            if (!passed) {
                std::ostringstream msg;
                msg << "The original macro dies at tick " << m_tickFrame << " (while checking input #" << w.index
                    << " at tick " << w.input.frame << ").";
                if (m_baselineAfterRestore)
                    msg << " This run started from a checkpoint: checkpoints may be imprecise, try exact-restart";
                else
                    msg << " Check that the macro passes the level";
                return abort(msg.str());
            }
            if (!m_cfg.exactRestart && m_cfg.verifyRestore && !m_baselineAfterRestore) {
                m_side = Side::Verify;
                m_shift = 0;
                m_trialEnd = w.input.frame + m_cfg.horizon;
                return restore();
            }
            break;
        case Side::Verify:
            if (!passed) {
                std::ostringstream msg;
                msg << "The original macro passes, but dies after restoring the checkpoint at input #" << w.index
                    << ". Checkpoints are imprecise here: enable exact-restart";
                return abort(msg.str());
            }
            break;
        case Side::Early:
        case Side::Late:
            if (prepareNextTrial(passed)) return restore();
            return moveToNextTarget();
    }
    if (startSide(Side::Early)) return restore();
    return moveToNextTarget();
}

TickOutput Controller::moveToNextTarget() {
    current().status = Status::Ok;
    ++m_targetPos;
    m_shift = 0;
    m_phase = Phase::Advance;
    if (m_targetPos >= m_targets.size()) return finish();
    planTarget();
    if (m_targetPos >= m_targets.size()) return finish();
    return restore();
}

TickOutput Controller::restore() {
    TickOutput out;
    out.command = Command::RestoreCheckpoint;
    return out;
}

TickOutput Controller::abort(std::string message) {
    m_phase = Phase::Aborted;
    m_abortReason = std::move(message);
    TickOutput out;
    out.command = Command::Abort;
    out.message = m_abortReason;
    return out;
}

TickOutput Controller::finish() {
    m_phase = Phase::Finished;
    TickOutput out;
    out.command = Command::Finish;
    return out;
}

size_t Controller::measuredCount() const {
    return static_cast<size_t>(
        std::count_if(m_windows.begin(), m_windows.end(), [](const Window& w) { return w.status == Status::Ok; }));
}

std::string Controller::progressText() const {
    std::ostringstream s;
    s << "Measured " << measuredCount() << "/" << m_targets.size() << " inputs, " << m_totalTrials << " trials";
    return s.str();
}

std::vector<Input> parseMacroText(const std::string& text, std::string* error) {
    std::vector<Input> inputs;
    std::istringstream lines(text);
    std::string line;
    int lineNo = 0;
    auto fail = [&](const std::string& what) {
        if (error) *error = "line " + std::to_string(lineNo) + ": " + what;
        return std::vector<Input>{};
    };
    while (std::getline(lines, line)) {
        ++lineNo;
        if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        if (auto eq = line.find('='); eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            key.erase(std::remove_if(key.begin(), key.end(), [](char c) { return c == ' ' || c == '\t'; }), key.end());
            if (key == "tps" || key == "fps" || key == "framerate") {
                double value = 0;
                std::istringstream(line.substr(eq + 1)) >> value;
                if (value != 240.0) return fail("only 240 tps macros are supported, convert with gddiff probe-prepare");
            }
            continue;
        }
        std::istringstream fields(line);
        long long frame = 0;
        int down = 0, p2 = 0;
        if (!(fields >> frame >> down)) return fail("expected '<frame> <1|0> [<player2 1|0>]'");
        fields >> p2;
        inputs.push_back(Input{frame, down != 0, p2 != 0});
    }
    if (error) error->clear();
    return inputs;
}

const char* statusName(Status status) {
    switch (status) {
        case Status::Ok: return "ok";
        case Status::Skipped: return "skipped";
        case Status::NotMeasured: return "not_measured";
    }
    return "unknown";
}

void writeCsv(std::ostream& out, const Controller& controller, int tps) {
    const Config& cfg = controller.config();
    out << "# gddiff-probe v1 tps=" << tps << " max_shift=" << cfg.maxShift << " horizon=" << cfg.horizon
        << " restore=" << (cfg.exactRestart ? "restart" : "checkpoint") << " complete=" << (controller.aborted() ? 0 : 1)
        << "\n";
    out << "index,frame,down,player2,early,late,early_limit,late_limit,trials,status\n";
    for (const Window& w : controller.windows()) {
        out << w.index << ',' << w.input.frame << ',' << (w.input.down ? 1 : 0) << ',' << (w.input.player2 ? 1 : 0) << ','
            << w.early << ',' << w.late << ',' << w.earlyLimit << ',' << w.lateLimit << ',' << w.trials << ','
            << statusName(w.status) << "\n";
    }
}

}  // namespace probe
