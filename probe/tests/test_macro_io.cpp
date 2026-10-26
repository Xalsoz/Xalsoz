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
    // GDR 1 (.gdr): MessagePack, записан настоящей библиотекой msgpack (Python), не нашим кодом.
    {
        const std::string hex =
        "8dab67616d6556657273696f6ecb4001a8c154c985f0ab6465736372697074696f6ea0a776657273696f6ecb3ff00000"
        "00000000a86475726174696f6ecb400c000000000000a3626f7482a46e616d65a57864426f74a776657273696f6ea776"
        "322e332e3131a56c6576656c82a26964ce004c4b40a46e616d65a54752494546a6617574686f72a26d65a47365656400"
        "a5636f696e7300a36c646dc2a96672616d6572617465cb406e000000000000a6696e707574739684a56672616d6564a3"
        "62746e01a23270c2a4646f776ec384a56672616d65cc82a362746e01a23270c2a4646f776ec284a56672616d65cd012c"
        "a362746e01a23270c3a4646f776ec384a56672616d65cd0136a362746e02a23270c2a4646f776ec384a56672616d65ce"
        "00011170a362746e01a23270c2a4646f776ec384a56672616d65ce0001117aa362746e01a23270c2a4646f776ec2aa66"
        "72616d6546697865739182a56672616d6505a2703182a178cb3ff8000000000000a179cb4004000000000000";
        std::vector<uint8_t> raw;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) raw.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
        MacroInfo info;
        std::string err;
        auto m = parseMacroBytes(raw, &info, &err);
        CHECK(err.empty());
        CHECK(m.size() == 5);  // кнопка 2 (платформер) пропущена
        if (m.size() == 5) {
            CHECK(m[0].frame == 100 && m[0].down && !m[0].player2);
            CHECK(m[1].frame == 130 && !m[1].down);
            CHECK(m[2].frame == 300 && m[2].down && m[2].player2);
            CHECK(m[3].frame == 70000 && m[3].down);  // uint32
            CHECK(m[4].frame == 70010 && !m[4].down);
        }
        CHECK(info.levelId == 5000000 && info.levelName == "GRIEF" && info.bot == "xdBot");
        CHECK(info.note.find("non-jump") != std::string::npos);
        // Оборванный файл: понятная ошибка, а не падение.
        for (size_t cut : {size_t(1), size_t(10), raw.size() / 2, raw.size() - 1}) {
            std::vector<uint8_t> part(raw.begin(), raw.begin() + cut);
            std::string e2;
            CHECK(parseMacroBytes(part, nullptr, &e2).empty() && !e2.empty());
        }
    }
    // xdBot .xd: первая строка частота, дальше кадр|нажато|кнопка|игрок2|позиция|...
    {
        const std::string xd =
            "60\n"
            "10|1|1|0|0|0|0|0|0|0|0|0|0|0|0|0\n"
            "10|0|0|0|1|100.5|200.5|0|0|0|0|100|200|0|0|0\n"
            "13|0|1|0|0|0|0|0|0|0|0|0|0|0|0|0\n"
            "20|1|1|1|0|0|0|0|0|0|0|0|0|0|0|0\r\n";
        MacroInfo info;
        std::string err;
        std::vector<uint8_t> raw(xd.begin(), xd.end());
        auto m = parseMacroBytes(raw, &info, &err);
        CHECK(err.empty() && m.size() == 3);
        if (m.size() == 3) {
            CHECK(m[0].frame == 40 && m[0].down && !m[0].player2);  // 60 к/с -> 240 тиков
            CHECK(m[1].frame == 52 && !m[1].down);
            CHECK(m[2].frame == 80 && m[2].down && m[2].player2);
        }
        CHECK(info.fps == 60.0);
        const std::string andr = "android\n5|1|1|0|0\n";
        err.clear();
        auto a = parseMacroBytes(std::vector<uint8_t>(andr.begin(), andr.end()), nullptr, &err);
        CHECK(a.size() == 1 && a[0].frame == 20);
        const std::string badxd = "60\nabc|1|1|0|0\n";
        err.clear();
        CHECK(parseMacroBytes(std::vector<uint8_t>(badxd.begin(), badxd.end()), nullptr, &err).empty() && !err.empty());
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
