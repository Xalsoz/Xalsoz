// Адаптер ядра измерителя к Geometry Dash через Geode.
//
// Что делает: при входе в уровень (если включено в настройках) читает
// probe_input.txt из папки сохранений мода, включает режим практики и ведёт
// ядро probe::Controller: подаёт клики, ставит и восстанавливает чекпоинты,
// ловит смерть. Результат пишет в probe_<levelID>.csv рядом с входным файлом.
//
// Написано по привязкам GD 2.2081 (Geode SDK 5) и по тому, как xdBot подаёт
// вводы. Ядро собрано и проверено тестами; этот файл собирается только вместе
// с Geode SDK и требует проверки в игре.
//
// Тексты для игрока на английском: шрифты GD не содержат кириллицы.

#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Notification.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "core/probe_core.hpp"

using namespace geode::prelude;

namespace {

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

struct Session {
    std::vector<probe::Input> macro;
    probe::Config config;
    std::unique_ptr<probe::Controller> controller;
    bool pending = false;    // уровень открыт, ждём устойчивого старта
    bool running = false;
    bool injecting = false;  // сейчас клик подаёт мод, а не игрок
    bool marking = false;    // сейчас чекпоинт ставит мод, а не игра
    bool died = false;
    int64_t lastTick = -1;
    float speed = 1.f;
    bool levelTimeSource = true;
    bool repairRestore = true;
    int64_t frameOffset = 0;
    int levelID = 0;
    int64_t notifiedTrials = 0;
    int64_t repairs = 0;
    Fingerprint markPrint;
    bool haveMarkPrint = false;
};

Session& session() {
    static Session s;
    return s;
}

std::filesystem::path inputPath() { return Mod::get()->getSaveDir() / "probe_input.txt"; }

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

std::string playerMismatch(char const* who, PlayerPrint const& a, PlayerPrint const& b) {
    std::ostringstream s;
    if (std::abs(a.x - b.x) > 1e-3f || std::abs(a.y - b.y) > 1e-3f)
        s << who << " position (" << a.x << ", " << a.y << ") vs (" << b.x << ", " << b.y << ")";
    else if (std::abs(a.vy - b.vy) > 1e-6)
        s << who << " y velocity " << a.vy << " vs " << b.vy;
    else if (std::abs(a.rotation - b.rotation) > 1e-2f)
        s << who << " rotation " << a.rotation << " vs " << b.rotation;
    else if (a.hold != b.hold)
        s << who << " held button";
    return s.str();
}

std::string fingerprintMismatch(Fingerprint const& a, Fingerprint const& b) {
    std::string d = playerMismatch("P1", a.p1, b.p1);
    if (d.empty()) d = playerMismatch("P2", a.p2, b.p2);
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

std::filesystem::path writeResults() {
    auto& s = session();
    auto dir = Mod::get()->getSaveDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto path = dir / ("probe_" + std::to_string(s.levelID) + ".csv");
    std::ofstream out(path);
    probe::writeCsv(out, *s.controller);
    return path;
}

void stopProbe(PlayLayer* pl, std::string const& error) {
    auto& s = session();
    if (!s.controller || !s.running) return;
    s.running = false;
    auto path = writeResults();
    if (pl) {
        pl->m_player1->releaseAllButtons();
        pl->m_player2->releaseAllButtons();
    }
    std::string summary = s.controller->progressText() + ", " + std::to_string(s.repairs) +
                          " checkpoint repairs. Saved to " + path.string();
    log::info("gddiff probe: {}", summary);
    if (error.empty()) {
        alert("Done. " + summary);
    } else {
        log::error("gddiff probe aborted: {}", error);
        alert("Stopped: " + error + "\n\n" + summary);
    }
}

void startProbe(PlayLayer* pl) {
    auto& s = session();
    if (!pl->m_isPracticeMode) pl->togglePracticeMode(true);
    while (pl->m_checkpointArray && pl->m_checkpointArray->count() > 0) pl->removeCheckpoint(false);
    s.controller = std::make_unique<probe::Controller>(s.macro, s.config);
    s.running = true;
    s.died = false;
    s.lastTick = -1;
    s.haveMarkPrint = false;
    s.notifiedTrials = 0;
    s.repairs = 0;
    notify("gddiff probe: measuring " + std::to_string(s.controller->targetCount()) + " inputs");
    // Практика без чекпоинтов: перезапуск с начала уровня.
    pl->resetLevel();
}

void markCheckpoint(PlayLayer* pl) {
    auto& s = session();
    unsigned before = pl->m_checkpointArray ? pl->m_checkpointArray->count() : 0;
    s.marking = true;
    pl->markCheckpoint();
    s.marking = false;
    unsigned after = pl->m_checkpointArray ? pl->m_checkpointArray->count() : 0;
    if (after <= before) {
        stopProbe(pl, "Could not place a practice checkpoint. Try exact-restart");
        return;
    }
    // Храним только последний чекпоинт: восстановление идёт к нему.
    while (pl->m_checkpointArray->count() > 1) pl->removeCheckpoint(true);
    s.markPrint = fingerprint(pl);
    s.haveMarkPrint = true;
}

void restore(PlayLayer* pl) {
    auto& s = session();
    pl->m_player1->releaseAllButtons();
    pl->m_player2->releaseAllButtons();
    // В практике без чекпоинтов это перезапуск с начала, с чекпоинтом это загрузка последнего.
    pl->resetLevel();
    s.died = false;
    int64_t tick = currentTick(pl);
    s.lastTick = tick;
    if (!s.config.exactRestart && s.haveMarkPrint) {
        std::string diff = fingerprintMismatch(s.markPrint, fingerprint(pl));
        if (!diff.empty()) {
            if (!s.repairRestore) {
                stopProbe(pl, "Checkpoint restored a different state (" + diff + "). Enable repair-restore or exact-restart");
                return;
            }
            // Ремонт; правильность затем проверяет повтор исходного клика после восстановления.
            applyPrint(pl->m_player1, s.markPrint.p1);
            applyPrint(pl->m_player2, s.markPrint.p2);
            ++s.repairs;
            log::debug("gddiff probe: repaired checkpoint state at tick {}: {}", tick, diff);
        }
    }
    probe::TickOutput after = s.controller->afterRestore(tick);
    for (auto const& in : after.press) press(pl, in);
    if (after.command == probe::Command::Abort) stopProbe(pl, after.message);
}

void handleOutput(PlayLayer* pl, probe::TickOutput const& out) {
    auto& s = session();
    if (out.command == probe::Command::MarkCheckpoint) {
        markCheckpoint(pl);
        if (!s.running) return;
    }
    for (auto const& in : out.press) press(pl, in);
    switch (out.command) {
        case probe::Command::RestoreCheckpoint: restore(pl); break;
        case probe::Command::Finish: stopProbe(pl, ""); return;
        case probe::Command::Abort: stopProbe(pl, out.message); return;
        default: break;
    }
    if (s.running && s.controller->totalTrials() - s.notifiedTrials >= 500) {
        s.notifiedTrials = s.controller->totalTrials();
        notify(s.controller->progressText(), NotificationIcon::Loading);
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
            startProbe(pl);
        }
        return;
    }
    if (!s.running) return;
    int64_t tick = currentTick(pl);
    bool died = s.died;
    s.died = false;
    if (tick == s.lastTick && !died) return;  // полутики 2.2081: один вызов ядра на тик
    s.lastTick = tick;
    handleOutput(pl, s.controller->tick(tick, died, pl->m_levelEndAnimationStarted));
}

}  // namespace

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
        if (s.running && !s.injecting) return;  // во время замера клики игрока игнорируются
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(ProbePlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        auto& s = session();
        s = Session{};
        if (!Mod::get()->getSettingValue<bool>("enabled")) return true;

        std::ifstream file(inputPath());
        if (!file) {
            notify("gddiff probe: no probe_input.txt in the mod save folder", NotificationIcon::Error);
            return true;
        }
        std::stringstream text;
        text << file.rdbuf();
        std::string error;
        s.macro = probe::parseMacroText(text.str(), &error);
        if (!error.empty() || s.macro.empty()) {
            notify("gddiff probe: bad probe_input.txt " + error, NotificationIcon::Error);
            return true;
        }

        auto mod = Mod::get();
        s.config.maxShift = static_cast<int>(mod->getSettingValue<int64_t>("max-shift"));
        s.config.horizon = static_cast<int>(mod->getSettingValue<int64_t>("horizon"));
        s.config.stride = static_cast<int>(mod->getSettingValue<int64_t>("stride"));
        s.config.firstIndex = static_cast<int>(mod->getSettingValue<int64_t>("first-index"));
        s.config.lastIndex = static_cast<int>(mod->getSettingValue<int64_t>("last-index"));
        s.config.exactRestart = mod->getSettingValue<bool>("exact-restart");
        s.config.verifyRestore = mod->getSettingValue<bool>("verify-restore");
        s.config.checkpointOnRelease = mod->getSettingValue<bool>("checkpoint-on-release");
        s.repairRestore = mod->getSettingValue<bool>("repair-restore");
        s.speed = static_cast<float>(mod->getSettingValue<double>("speed"));
        s.levelTimeSource = mod->getSettingValue<std::string>("frame-source") != "progress";
        s.frameOffset = mod->getSettingValue<int64_t>("frame-offset");
        s.levelID = level->m_levelID.value();
        s.pending = true;
        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto& s = session();
        // m_anticheatSpike игра «убивает» сама при старте: его пропускаем как есть.
        if (!s.running || object == m_anticheatSpike || (player != m_player1 && player != m_player2))
            return PlayLayer::destroyPlayer(player, object);
        // Смерть фиксируем, но не исполняем: ядро само вернёт игрока к чекпоинту.
        s.died = true;
    }

    void storeCheckpoint(CheckpointObject* checkpoint) {
        // Во время замера игра не должна добавлять свои чекпоинты (автоматические
        // или от объектов уровня): восстановление идёт к последнему.
        auto& s = session();
        if (s.running && !s.marking) return;
        PlayLayer::storeCheckpoint(checkpoint);
    }

    void levelComplete() {
        // Во время замера конец уровня означает выживание пробы, а не прохождение.
        if (session().running) return;
        PlayLayer::levelComplete();
    }

    void onQuit() {
        stopProbe(this, "Level closed before the probe finished");
        session() = Session{};
        PlayLayer::onQuit();
    }
};

class $modify(ProbeScheduler, CCScheduler) {
    void update(float dt) {
        auto& s = session();
        if (s.running && s.speed != 1.f) return CCScheduler::update(dt * s.speed);
        CCScheduler::update(dt);
    }
};
