// Адаптер ядер к Geometry Dash через Geode.
//
// Два режима, оба в режиме практики:
//   Solve   : ядро probe::Solver само проходит уровень (ищет клики, откатываясь
//             по чекпоинтам), затем, если включено, замеряет окна найденного маршрута;
//   Measure : ядро probe::Controller берёт готовый макрос из probe_input.txt и мерит
//             окно каждого клика.
// Управление: кнопка «gddiff» в меню паузы открывает окно со стартом, остановкой,
// ходом работы и итогом. При желании замер макроса стартует сам при входе в уровень
// (настройка «Measure probe_input.txt on level start»).
//
// Написано по привязкам GD 2.2081 (Geode SDK 5) и по тому, как xdBot подаёт вводы.
// Ядра собраны и проверены тестами; этот файл проверен на типы против заголовков
// Geode и требует проверки в игре.
//
// Тексты для игрока на английском: шрифты GD не содержат кириллицы.

#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/Popup.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "core/difficulty.hpp"
#include "core/probe_core.hpp"
#include "core/solve_core.hpp"

using namespace geode::prelude;

namespace {

constexpr double kTickMs = 1000.0 / 240.0;

// Снимок состояния игрока: им проверяем и при необходимости чиним то, что
// восстановил чекпоинт (стандартные чекпоинты GD сохраняют игрока не полностью).
struct PlayerPrint {
    float x = 0.f;
    float y = 0.f;
    float rotation = 0.f;
    double vy = 0.0;
    bool hold = false;
};

struct Fingerprint {
    PlayerPrint p1;
    PlayerPrint p2;
};

enum class Phase { None, Solving, Measuring };

struct Session {
    std::vector<probe::Input> macro;
    probe::Config config;
    probe::SolveConfig solveConfig;
    std::unique_ptr<probe::Controller> controller;
    std::unique_ptr<probe::Solver> solver;
    Phase phase = Phase::None;
    bool measureAfterSolve = true;
    bool pending = false;    // уровень открыт, ждём устойчивого старта (запуск по настройке)
    bool injecting = false;  // сейчас клик подаёт мод, а не игрок
    bool marking = false;    // сейчас чекпоинт ставит мод, а не игра
    bool died = false;
    int64_t lastTick = -1;
    float speed = 1.f;
    bool levelTimeSource = true;
    bool repairRestore = true;
    int64_t frameOffset = 0;
    double sigmaMs = 10.0;
    int levelID = 0;
    std::string levelName;
    int64_t notifiedAt = 0;
    int64_t repairs = 0;
    std::vector<Fingerprint> prints;  // по одному на каждый чекпоинт мода (стек)

    bool running() const { return phase != Phase::None; }
};

Session& session() {
    static Session s;
    return s;
}

// Итог последнего запуска: переживает смену уровня и показывается в окне.
struct LastRun {
    std::vector<std::string> lines;
};

LastRun& lastRun() {
    static LastRun r;
    return r;
}

std::filesystem::path saveDir() { return Mod::get()->getSaveDir(); }
std::filesystem::path inputPath() { return saveDir() / "probe_input.txt"; }

void notify(std::string const& text, NotificationIcon icon = NotificationIcon::Info) {
    Notification::create(text, icon)->show();
}

void alert(std::string const& text) { FLAlertLayer::create("gddiff probe", text.c_str(), "OK")->show(); }

int64_t currentTick(PlayLayer* pl) {
    auto& s = session();
    // Как xdBot: для его макросов тик считается по времени уровня.
    int64_t tick = s.levelTimeSource ? static_cast<int64_t>(pl->m_gameState.m_levelTime * 240.0) + 1
                                     : static_cast<int64_t>(pl->m_gameState.m_currentProgress);
    return tick - s.frameOffset;
}

bool holding(PlayerObject* p) {
    if (!p) return false;
    auto it = p->m_holdingButtons.find(1);
    return it != p->m_holdingButtons.end() && it->second;
}

PlayerPrint playerPrint(PlayerObject* p) {
    PlayerPrint f;
    f.x = p->m_position.x;
    f.y = p->m_position.y;
    f.rotation = p->getRotation();
    f.vy = p->m_yVelocity;
    f.hold = holding(p);
    return f;
}

Fingerprint fingerprint(PlayLayer* pl) { return {playerPrint(pl->m_player1), playerPrint(pl->m_player2)}; }

std::string playerMismatch(char const* who, PlayerPrint const& a, PlayerPrint const& b, bool compareHold) {
    std::ostringstream s;
    if (std::abs(a.x - b.x) > 1e-3f || std::abs(a.y - b.y) > 1e-3f)
        s << who << " position (" << a.x << ", " << a.y << ") vs (" << b.x << ", " << b.y << ")";
    else if (std::abs(a.vy - b.vy) > 1e-6)
        s << who << " y velocity " << a.vy << " vs " << b.vy;
    else if (std::abs(a.rotation - b.rotation) > 1e-2f)
        s << who << " rotation " << a.rotation << " vs " << b.rotation;
    else if (compareHold && a.hold != b.hold)
        s << who << " held button";
    return s.str();
}

std::string fingerprintMismatch(Fingerprint const& a, Fingerprint const& b, bool compareHold) {
    std::string d = playerMismatch("P1", a.p1, b.p1, compareHold);
    if (d.empty()) d = playerMismatch("P2", a.p2, b.p2, compareHold);
    return d;
}

void applyPrint(PlayerObject* p, PlayerPrint const& f) {
    // Свой минимальный ремонт: позиция, поворот, вертикальная скорость.
    p->setPosition(ccp(f.x, f.y));
    p->m_position = ccp(f.x, f.y);
    p->setRotation(f.rotation);
    p->m_yVelocity = f.vy;
}

void press(PlayLayer* pl, probe::Input const& in) {
    // Как в xdBot: третий аргумент несёт флаг второго игрока, с учётом
    // настройки «поменять управление двух игроков».
    bool p2 = in.player2;
    if (!pl->m_levelSettings->m_platformerMode && GameManager::get()->getGameVariable("0010")) p2 = !p2;
    auto& s = session();
    s.injecting = true;
    pl->handleButton(in.down, 1, p2);
    s.injecting = false;
}

// ---------------------------------------------------------------- настройки

void loadSettings(Session& s) {
    auto mod = Mod::get();
    s.config = probe::Config{};
    s.config.maxShift = static_cast<int>(mod->getSettingValue<int64_t>("max-shift"));
    s.config.horizon = static_cast<int>(mod->getSettingValue<int64_t>("horizon"));
    s.config.stride = static_cast<int>(mod->getSettingValue<int64_t>("stride"));
    s.config.firstIndex = static_cast<int>(mod->getSettingValue<int64_t>("first-index"));
    s.config.lastIndex = static_cast<int>(mod->getSettingValue<int64_t>("last-index"));
    s.config.exactRestart = mod->getSettingValue<bool>("exact-restart");
    s.config.verifyRestore = mod->getSettingValue<bool>("verify-restore");
    s.config.checkpointOnRelease = mod->getSettingValue<bool>("checkpoint-on-release");
    s.solveConfig = probe::SolveConfig{};
    s.solveConfig.horizon = static_cast<int>(mod->getSettingValue<int64_t>("solve-horizon"));
    s.solveConfig.maxDelay = static_cast<int>(mod->getSettingValue<int64_t>("solve-max-delay"));
    s.solveConfig.scanStride = static_cast<int>(mod->getSettingValue<int64_t>("solve-stride"));
    s.solveConfig.slack = static_cast<int>(mod->getSettingValue<int64_t>("solve-slack"));
    s.solveConfig.maxAnchors = static_cast<int>(mod->getSettingValue<int64_t>("solve-max-checkpoints"));
    s.measureAfterSolve = mod->getSettingValue<bool>("measure-after-solve");
    s.repairRestore = mod->getSettingValue<bool>("repair-restore");
    s.speed = static_cast<float>(mod->getSettingValue<double>("speed"));
    s.sigmaMs = mod->getSettingValue<double>("sigma-ms");
    s.levelTimeSource = mod->getSettingValue<std::string>("frame-source") != "progress";
    s.frameOffset = mod->getSettingValue<int64_t>("frame-offset");
}

bool loadMacroFile(Session& s, std::string& problem) {
    std::ifstream file(inputPath());
    if (!file) {
        problem = "No probe_input.txt in the mod save folder (" + saveDir().string() + ")";
        return false;
    }
    std::stringstream text;
    text << file.rdbuf();
    std::string error;
    s.macro = probe::parseMacroText(text.str(), &error);
    if (!error.empty() || s.macro.empty()) {
        problem = "Bad probe_input.txt " + error;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- результаты

std::string fixed(double v, int digits) {
    std::ostringstream s;
    s.setf(std::ios::fixed);
    s.precision(digits);
    s << v;
    return s.str();
}

std::filesystem::path writeCsv() {
    auto& s = session();
    auto dir = saveDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto path = dir / ("probe_" + std::to_string(s.levelID) + ".csv");
    std::ofstream out(path);
    probe::writeCsv(out, *s.controller);
    return path;
}

std::filesystem::path writeSolved(bool partial) {
    auto& s = session();
    auto dir = saveDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string stem = std::string(partial ? "partial_" : "solved_") + std::to_string(s.levelID);
    auto macro = s.solver->macro();
    {
        std::ofstream out(dir / (stem + ".txt"));
        probe::writeMacroText(out, macro);
    }
    {
        std::ofstream out(dir / (stem + ".gdr.json"));
        probe::writeMacroGdrJson(out, macro, s.levelID, s.levelName);
    }
    return dir / (stem + ".txt");
}

void buildResultLines(std::string const& saved) {
    auto& s = session();
    auto& lines = lastRun().lines;
    lines.clear();
    std::vector<double> windows;
    struct Narrow {
        size_t index;
        int64_t frame;
        double ms;
        int early, late;
        bool lowerBound;
    };
    std::vector<Narrow> narrow;
    for (auto const& w : s.controller->windows()) {
        if (w.status != probe::Status::Ok) continue;
        const double ms = (w.early + w.late + 1) * kTickMs;
        windows.push_back(ms);
        narrow.push_back({w.index, w.input.frame, ms, w.early, w.late, w.early >= w.earlyLimit || w.late >= w.lateLimit});
    }
    lines.push_back(s.levelName + " (" + std::to_string(s.levelID) + "): " + std::to_string(windows.size()) +
                    " clicks measured");
    if (windows.empty()) {
        lines.push_back("No windows were measured.");
        lines.push_back(saved);
        return;
    }
    const double inf = std::numeric_limits<double>::infinity();
    auto rows = probe::levelDifficulty(windows, {60.0, 144.0, 240.0, inf}, s.sigmaMs);
    std::string diff = "Difficulty (higher = harder): ";
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i == 2) {
            lines.push_back(diff);
            diff = "  ";
        }
        diff += (std::isfinite(rows[i].hz) ? fixed(rows[i].hz, 0) + " Hz " : std::string("between frames ")) +
                fixed(rows[i].nats, 1) + "   ";
    }
    lines.push_back(diff);
    std::sort(narrow.begin(), narrow.end(), [](Narrow const& a, Narrow const& b) { return a.ms < b.ms; });
    lines.push_back("Narrowest clicks:");
    for (size_t i = 0; i < narrow.size() && i < 5; ++i) {
        auto const& n = narrow[i];
        lines.push_back("  #" + std::to_string(n.index) + " tick " + std::to_string(n.frame) + ": " +
                        (n.lowerBound ? ">=" : "") + fixed(n.ms, 1) + " ms (-" + std::to_string(n.early) + "/+" +
                        std::to_string(n.late) + ")");
    }
    lines.push_back(saved);
}

// ---------------------------------------------------------------- запуск и остановка

void beginPractice(PlayLayer* pl) {
    auto& s = session();
    if (!pl->m_isPracticeMode) pl->togglePracticeMode(true);
    while (pl->m_checkpointArray && pl->m_checkpointArray->count() > 0) pl->removeCheckpoint(false);
    s.prints.clear();
    s.died = false;
    s.lastTick = -1;
    s.repairs = 0;
    s.notifiedAt = 0;
}

void releaseButtons(PlayLayer* pl) {
    if (!pl) return;
    pl->m_player1->releaseAllButtons();
    pl->m_player2->releaseAllButtons();
}

void startMeasure(PlayLayer* pl) {
    auto& s = session();
    s.controller = std::make_unique<probe::Controller>(s.macro, s.config);
    s.phase = Phase::Measuring;
    beginPractice(pl);
    notify("gddiff: measuring " + std::to_string(s.controller->targetCount()) + " clicks");
    // Практика без чекпоинтов: перезапуск с начала уровня.
    pl->resetLevel();
}

void startSolve(PlayLayer* pl) {
    auto& s = session();
    s.solver = std::make_unique<probe::Solver>(s.solveConfig);
    s.controller.reset();
    s.phase = Phase::Solving;
    beginPractice(pl);
    notify("gddiff: solving the level");
    pl->resetLevel();
}

void finishMeasure(PlayLayer* pl, std::string const& error) {
    auto& s = session();
    if (s.phase != Phase::Measuring || !s.controller) return;
    s.phase = Phase::None;
    auto path = writeCsv();
    releaseButtons(pl);
    std::string summary = s.controller->progressText() + ", " + std::to_string(s.repairs) +
                          " checkpoint repairs. Saved to " + path.string();
    log::info("gddiff probe: {}", summary);
    buildResultLines("Saved: " + path.string());
    if (!error.empty()) lastRun().lines.insert(lastRun().lines.begin(), "Stopped: " + error);
    if (error.empty()) {
        alert("Done. " + summary);
    } else {
        log::error("gddiff probe aborted: {}", error);
        alert("Stopped: " + error + "\n\n" + summary);
    }
}

void finishSolve(PlayLayer* pl, std::string const& error) {
    auto& s = session();
    if (s.phase != Phase::Solving || !s.solver) return;
    const bool solved = s.solver->solved();
    auto path = writeSolved(!solved);
    releaseButtons(pl);
    std::string summary = s.solver->progressText() + ". Clicks saved to " + path.string();
    log::info("gddiff solve: {}", summary);
    if (!solved) {
        s.phase = Phase::None;
        lastRun().lines = {"Solver stopped: " + (error.empty() ? std::string("unknown") : error),
                           "Partial route saved: " + path.string()};
        alert("Solver stopped: " + error + "\n\nThe route found so far is saved to " + path.string());
        return;
    }
    s.macro = s.solver->macro();
    if (!s.measureAfterSolve) {
        s.phase = Phase::None;
        lastRun().lines = {"Level solved: " + std::to_string(s.macro.size()) + " clicks", "Saved: " + path.string()};
        alert("Level solved. " + summary);
        return;
    }
    notify("gddiff: solved, now measuring windows");
    log::info("gddiff solve: {} inputs, measuring", s.macro.size());
    startMeasure(pl);
}

void stopRun(PlayLayer* pl, std::string const& reason) {
    auto& s = session();
    if (s.phase == Phase::Solving) {
        if (s.solver && !s.solver->done()) {
            // Остановили вручную: сохраняем найденное как есть.
            auto path = writeSolved(true);
            releaseButtons(pl);
            s.phase = Phase::None;
            lastRun().lines = {"Stopped: " + reason, "Partial route saved: " + path.string()};
            alert("Stopped: " + reason + "\n\nPartial route saved to " + path.string());
        }
    } else if (s.phase == Phase::Measuring) {
        finishMeasure(pl, reason);
    }
}

// ---------------------------------------------------------------- чекпоинты

void markCheckpoint(PlayLayer* pl) {
    auto& s = session();
    unsigned before = pl->m_checkpointArray ? pl->m_checkpointArray->count() : 0;
    s.marking = true;
    pl->markCheckpoint();
    s.marking = false;
    unsigned after = pl->m_checkpointArray ? pl->m_checkpointArray->count() : 0;
    if (after <= before) {
        stopRun(pl, "Could not place a practice checkpoint. Try exact-restart");
        return;
    }
    if (s.phase == Phase::Measuring) {
        // Замер хранит только последний чекпоинт: восстановление идёт к нему.
        while (pl->m_checkpointArray->count() > 1) pl->removeCheckpoint(true);
        s.prints.clear();
    }
    s.prints.push_back(fingerprint(pl));
}

probe::TickOutput coreAfterRestore(int64_t tick) {
    auto& s = session();
    return s.phase == Phase::Solving ? s.solver->afterRestore(tick) : s.controller->afterRestore(tick);
}

void failRun(PlayLayer* pl, std::string const& message) {
    auto& s = session();
    if (s.phase == Phase::Solving) {
        finishSolve(pl, message);
    } else {
        finishMeasure(pl, message);
    }
}

void restore(PlayLayer* pl) {
    auto& s = session();
    releaseButtons(pl);
    // В практике без чекпоинтов это перезапуск с начала, с чекпоинтом это загрузка последнего.
    pl->resetLevel();
    s.died = false;
    int64_t tick = currentTick(pl);
    s.lastTick = tick;
    const bool exact = s.phase == Phase::Measuring && s.config.exactRestart;
    if (!exact && !s.prints.empty()) {
        // У замера макроса проверяем и удержание; у решателя кнопка всегда отпущена или помечена в чекпоинте.
        std::string diff = fingerprintMismatch(s.prints.back(), fingerprint(pl), false);
        if (!diff.empty()) {
            if (!s.repairRestore) {
                failRun(pl, "Checkpoint restored a different state (" + diff + "). Enable repair-restore or exact-restart");
                return;
            }
            // Ремонт; правильность затем проверяет повтор клика (замер) или повторная проба (решатель).
            applyPrint(pl->m_player1, s.prints.back().p1);
            applyPrint(pl->m_player2, s.prints.back().p2);
            ++s.repairs;
            log::debug("gddiff: repaired checkpoint state at tick {}: {}", tick, diff);
        }
    }
    probe::TickOutput after = coreAfterRestore(tick);
    for (auto const& in : after.press) press(pl, in);
    if (after.command == probe::Command::Abort) failRun(pl, after.message);
}

void handleOutput(PlayLayer* pl, probe::TickOutput const& out) {
    auto& s = session();
    for (int i = 0; i < out.popCheckpoints; ++i) {
        if (pl->m_checkpointArray && pl->m_checkpointArray->count() > 0) pl->removeCheckpoint(false);
        if (!s.prints.empty()) s.prints.pop_back();
    }
    if (out.command == probe::Command::MarkCheckpoint) {
        markCheckpoint(pl);
        if (!s.running()) return;
    }
    for (auto const& in : out.press) press(pl, in);
    switch (out.command) {
        case probe::Command::RestoreCheckpoint: restore(pl); break;
        case probe::Command::Finish:
            if (s.phase == Phase::Solving) {
                finishSolve(pl, "");
            } else {
                finishMeasure(pl, "");
            }
            return;
        case probe::Command::Abort: failRun(pl, out.message); return;
        default: break;
    }
    if (!s.running()) return;
    for (int i = 0; i < out.dropOldestCheckpoints; ++i) {
        if (pl->m_checkpointArray && pl->m_checkpointArray->count() > 1) pl->removeCheckpoint(true);
        if (!s.prints.empty()) s.prints.erase(s.prints.begin());
    }
    const int64_t work = s.phase == Phase::Solving ? s.solver->ticksUsed() : s.controller->totalTrials() * 4000;
    const int64_t step = s.phase == Phase::Solving ? 200000 : 2000000;
    if (work - s.notifiedAt >= step) {
        s.notifiedAt = work;
        notify(s.phase == Phase::Solving ? s.solver->progressText() : s.controller->progressText(),
               NotificationIcon::Loading);
    }
}

void afterCommands(GJBaseGameLayer* layer) {
    auto pl = PlayLayer::get();
    if (!pl || static_cast<GJBaseGameLayer*>(pl) != layer) return;
    auto& s = session();
    if (s.pending) {
        // Как xdBot: первая попытка после загрузки уровня ненадёжна по времени,
        // поэтому начинаем после пары тиков с перезапуска.
        if (currentTick(pl) > 2) {
            s.pending = false;
            startMeasure(pl);
        }
        return;
    }
    if (!s.running()) return;
    int64_t tick = currentTick(pl);
    bool died = s.died;
    s.died = false;
    if (tick == s.lastTick && !died) return;  // полутики 2.2081: один вызов ядра на тик
    s.lastTick = tick;
    const bool ended = pl->m_levelEndAnimationStarted;
    handleOutput(pl, s.phase == Phase::Solving ? s.solver->tick(tick, died, ended) : s.controller->tick(tick, died, ended));
}

// ---------------------------------------------------------------- окно управления

void resumeGame(PlayLayer* pl) {
    auto scene = CCDirector::sharedDirector()->getRunningScene();
    if (scene) {
        if (auto pause = scene->getChildByType<PauseLayer>(0)) {
            pause->onResume(nullptr);
            return;
        }
    }
    if (pl && pl->m_isPaused) pl->resume();
}

void uiStart(bool solve) {
    auto pl = PlayLayer::get();
    if (!pl) return;
    auto& s = session();
    if (s.running()) {
        notify("gddiff: already running, press Stop first", NotificationIcon::Warning);
        return;
    }
    if (pl->m_levelSettings->m_platformerMode) {
        alert("Platformer levels are not supported.");
        return;
    }
    loadSettings(s);
    s.levelID = pl->m_level->m_levelID.value();
    s.levelName = pl->m_level->m_levelName;
    if (!solve) {
        std::string problem;
        if (!loadMacroFile(s, problem)) {
            alert(problem);
            return;
        }
    }
    resumeGame(pl);
    if (solve) {
        startSolve(pl);
    } else {
        startMeasure(pl);
    }
}

void uiStop() {
    auto pl = PlayLayer::get();
    auto& s = session();
    if (!s.running()) return;
    stopRun(pl, "stopped by the player");
}

std::vector<std::string> statusLines() {
    auto& s = session();
    std::vector<std::string> lines;
    if (s.phase == Phase::Solving && s.solver) {
        lines.push_back("Solving the level by itself...");
        lines.push_back(s.solver->progressText());
        if (s.solver->tickRebases() > 0)
            lines.push_back("Tick counter shifted after " + std::to_string(s.solver->tickRebases()) + " restores");
    } else if (s.phase == Phase::Measuring && s.controller) {
        lines.push_back("Measuring click windows...");
        lines.push_back(s.controller->progressText());
    } else {
        lines.push_back("Idle. Pick an action:");
        for (auto const& l : lastRun().lines) lines.push_back(l);
    }
    return lines;
}

}  // namespace

class GddiffPopup : public geode::Popup {
protected:
    std::vector<CCLabelBMFont*> m_lines;

    bool init() {
        if (!Popup::init(400.f, 280.f)) return false;
        this->setTitle("gddiff window probe");

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(menu);

        auto addButton = [&](char const* text, float x, SEL_MenuHandler handler) {
            auto spr = ButtonSprite::create(text, 100, true, "bigFont.fnt", "GJ_button_01.png", 30.f, .45f);
            auto btn = CCMenuItemSpriteExtra::create(spr, this, handler);
            btn->setPosition({x, 232.f});
            menu->addChild(btn);
        };
        addButton("Solve + measure", 72.f, menu_selector(GddiffPopup::onSolve));
        addButton("Measure macro", 200.f, menu_selector(GddiffPopup::onMeasure));
        addButton("Stop", 328.f, menu_selector(GddiffPopup::onStopRun));

        for (int i = 0; i < 11; ++i) {
            auto label = CCLabelBMFont::create("", "chatFont.fnt");
            label->setAnchorPoint({0.f, 0.5f});
            label->setPosition({16.f, 195.f - i * 16.f});
            label->setScale(.62f);
            m_mainLayer->addChild(label);
            m_lines.push_back(label);
        }
        this->schedule(schedule_selector(GddiffPopup::refresh), 0.5f);
        this->refresh(0.f);
        return true;
    }

    void refresh(float) {
        auto lines = statusLines();
        for (size_t i = 0; i < m_lines.size(); ++i) {
            std::string text = i < lines.size() ? lines[i] : std::string();
            if (text.size() > 74) text = text.substr(0, 71) + "...";
            m_lines[i]->setString(text.c_str());
        }
    }

    void onSolve(CCObject*) {
        this->onClose(nullptr);
        uiStart(true);
    }
    void onMeasure(CCObject*) {
        this->onClose(nullptr);
        uiStart(false);
    }
    void onStopRun(CCObject*) {
        uiStop();
        this->refresh(0.f);
    }

public:
    static GddiffPopup* create() {
        auto ret = new GddiffPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class $modify(ProbePauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto spr = ButtonSprite::create("gddiff", 60, true, "bigFont.fnt", "GJ_button_04.png", 30.f, .5f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(ProbePauseLayer::onGddiff));
        btn->setID("gddiff-button"_spr);
        if (auto menu = this->getChildByID("right-button-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            auto fallback = CCMenu::create();
            fallback->addChild(btn);
            auto size = CCDirector::sharedDirector()->getWinSize();
            fallback->setPosition({size.width - 50.f, 40.f});
            this->addChild(fallback);
        }
    }

    void onGddiff(CCObject*) {
        if (auto popup = GddiffPopup::create()) popup->show();
    }
};

class $modify(ProbeGameLayer, GJBaseGameLayer) {
#if GEODE_COMP_GD_VERSION >= 22081
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        afterCommands(this);
    }
#else
    void processCommands(float dt) {
        GJBaseGameLayer::processCommands(dt);
        afterCommands(this);
    }
#endif

    void handleButton(bool down, int button, bool isPlayer1) {
        auto& s = session();
        if (s.running() && !s.injecting) return;  // во время работы клики игрока игнорируются
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(ProbePlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        auto& s = session();
        s = Session{};
        s.levelID = level->m_levelID.value();
        s.levelName = level->m_levelName;
        if (!Mod::get()->getSettingValue<bool>("enabled")) return true;

        loadSettings(s);
        std::string problem;
        if (!loadMacroFile(s, problem)) {
            notify("gddiff: " + problem, NotificationIcon::Error);
            return true;
        }
        s.pending = true;
        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto& s = session();
        // m_anticheatSpike игра «убивает» сама при старте: его пропускаем как есть.
        if (!s.running() || object == m_anticheatSpike || (player != m_player1 && player != m_player2))
            return PlayLayer::destroyPlayer(player, object);
        // Смерть фиксируем, но не исполняем: ядро само вернёт игрока к чекпоинту.
        s.died = true;
    }

    void storeCheckpoint(CheckpointObject* checkpoint) {
        // Во время работы игра не должна добавлять свои чекпоинты (автоматические
        // или от объектов уровня): восстановление идёт к последнему.
        auto& s = session();
        if (s.running() && !s.marking) return;
        PlayLayer::storeCheckpoint(checkpoint);
    }

    void levelComplete() {
        // Во время работы конец уровня означает выживание пробы, а не прохождение.
        if (session().running()) return;
        PlayLayer::levelComplete();
    }

    void onQuit() {
        stopRun(this, "Level closed before the run finished");
        session() = Session{};
        PlayLayer::onQuit();
    }
};

class $modify(ProbeScheduler, CCScheduler) {
    void update(float dt) {
        auto& s = session();
        if (s.running() && s.speed != 1.f) return CCScheduler::update(dt * s.speed);
        CCScheduler::update(dt);
    }
};
