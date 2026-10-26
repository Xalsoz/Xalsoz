// Тесты чтения макросов (macro_io): файл GDR 2 записан настоящей библиотекой GDReplayFormat.

#include "core/macro_io.hpp"
#include "core/solve_core.hpp"

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

static int g_failed = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(cond)) {                                                                    \
            ++g_failed;                                                                   \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                                 \
    } while (0)

static std::vector<uint8_t> fromHex(const std::string& h) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}

static const char* kLibraryFile = "474452020c50687973696373496e707574026d65016441480000c1ac01406e0000000000008906020000057864426f7403c096b10205475249454608b9600568656c6c6f026496010a0801100000000040000000400a0000000000000a1040f0000040000000400a000000000000cf041043e1000040000000400a0000000000000a1043e4c00040000000400a000000000000ffc0081047cd140040000000400a000000000000141047cd1b8040000000400a000000000000031047cd1c4040000000400a000000000000d2071047ce8b0040000000400a00000000000051104270000040000000400a00000000000064104307000040000000400a000000000000";

int main() {
    using namespace probe;
    // Файл из библиотеки: уровень GRIEF, два игрока, расширения входов, большие кадры.
    {
        MacroInfo info;
        std::string err;
        auto m = parseMacroBytes(fromHex(kLibraryFile), &info, &err);
        CHECK(err.empty());
        CHECK(info.levelId == 5000000 && info.levelName == "GRIEF" && info.bot == "xdBot" && info.fps == 240.0);
        const int64_t frames[] = {0, 5, 40, 90, 300, 305, 70000, 70010, 70011, 70500};
        const bool down[] = {true, false, true, false, true, false, true, false, true, false};
        const bool p2[] = {false, false, true, true, false, false, false, false, false, false};
        CHECK(m.size() == 10);
        for (size_t i = 0; i < m.size() && i < 10; ++i) {
            CHECK(m[i].frame == frames[i]);
            CHECK(m[i].down == down[i]);
            CHECK(m[i].player2 == p2[i]);
        }
    }
    // Обрыв файла и чужая версия.
    {
        auto bytes = fromHex(kLibraryFile);
        std::string err;
        std::vector<uint8_t> cut(bytes.begin(), bytes.begin() + 20);
        CHECK(parseMacroBytes(cut, nullptr, &err).empty() && err.find("truncated") != std::string::npos);
        std::vector<uint8_t> v1 = {'G', 'D', 'R', 1, 0};
        err.clear();
        CHECK(parseMacroBytes(v1, nullptr, &err).empty() && err.find("version") != std::string::npos);
    }
    // JSON: то, что пишет xdBot и наш мод; частота пересчитывается, не прыжковые кнопки пропускаются.
    {
        std::string j =
            "{\"framerate\":120,\"bot\":{\"name\":\"xdBot\",\"version\":\"1\"},\"level\":{\"id\":7,\"name\":\"A\\\"B\"},"
            "\"inputs\":[{\"frame\":10,\"btn\":1,\"2p\":false,\"down\":true},{\"frame\":11,\"btn\":2,\"down\":true},"
            "{\"frame\":20,\"btn\":1,\"down\":false}]}";
        MacroInfo info;
        std::string err;
        auto m = parseMacroBytes(std::vector<uint8_t>(j.begin(), j.end()), &info, &err);
        CHECK(err.empty());
        CHECK(m.size() == 2 && m[0].frame == 20 && m[0].down && m[1].frame == 40 && !m[1].down);
        CHECK(info.levelId == 7 && info.bot == "xdBot" && info.fps == 120.0);
        CHECK(info.note.find("converted") != std::string::npos && info.note.find("skipped") != std::string::npos);
        std::string bad = "{\"inputs\":[{\"frame\":1}]}";
        CHECK(parseMacroBytes(std::vector<uint8_t>(bad.begin(), bad.end()), nullptr, &err).empty());
        std::string broken = "{\"inputs\":[";
        err.clear();
        CHECK(parseMacroBytes(std::vector<uint8_t>(broken.begin(), broken.end()), nullptr, &err).empty() && !err.empty());
    }
    // Туда и обратно: JSON, который пишет мод, мод же и читает; текстовый формат тоже.
    {
        std::vector<Input> in{{10, true, false}, {20, false, false}, {35, true, true}, {50, false, true}};
        std::ostringstream js;
        writeMacroGdrJson(js, in, 99, "Lvl");
        std::string t = js.str();
        MacroInfo info;
        std::string err;
        auto back = parseMacroBytes(std::vector<uint8_t>(t.begin(), t.end()), &info, &err);
        CHECK(err.empty() && back.size() == 4 && info.levelId == 99 && info.levelName == "Lvl");
        for (size_t i = 0; i < back.size() && i < 4; ++i) CHECK(back[i].frame == in[i].frame && back[i].down == in[i].down && back[i].player2 == in[i].player2);
        std::ostringstream tx;
        writeMacroText(tx, in);
        std::string tt = tx.str();
        auto back2 = parseMacroBytes(std::vector<uint8_t>(tt.begin(), tt.end()), nullptr, &err);
        CHECK(back2.size() == 4 && back2[2].player2);
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
