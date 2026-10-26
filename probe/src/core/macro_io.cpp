#include "macro_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>

namespace probe {

namespace {

// ------------------------------------------------------------------ GDR 2

struct Reader {
    const std::vector<uint8_t>& d;
    size_t pos = 0;
    bool bad = false;

    size_t left() const { return d.size() - pos; }
    uint64_t varint() {
        uint64_t result = 0;
        for (int i = 0; i < 10; ++i) {
            if (pos >= d.size()) {
                bad = true;
                return 0;
            }
            const uint8_t b = d[pos++];
            result |= static_cast<uint64_t>(b & 0x7F) << (7 * i);
            if (!(b & 0x80)) return result;
        }
        bad = true;
        return 0;
    }
    std::string str() {
        const uint64_t n = varint();
        if (bad || n > left()) {
            bad = true;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(d.data() + pos), static_cast<size_t>(n));
        pos += static_cast<size_t>(n);
        return s;
    }
    double flt(size_t size) {
        if (left() < size) {
            bad = true;
            return 0;
        }
        uint8_t b[8];
        for (size_t i = 0; i < size; ++i) b[i] = d[pos + size - 1 - i];  // порядок байт от старшего к младшему
        pos += size;
        if (size == 4) {
            float f;
            std::memcpy(&f, b, 4);
            return f;
        }
        double v;
        std::memcpy(&v, b, 8);
        return v;
    }
    void skip(uint64_t n) {
        if (n > left()) {
            bad = true;
            return;
        }
        pos += static_cast<size_t>(n);
    }
};

// ------------------------------------------------------------------ JSON (достаточно для GDR)

struct Json {
    enum class Type { Null, Bool, Num, Str, Arr, Obj } type = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> arr;
    // Объект: ключи и значения в параллельных векторах (std::map от неполного типа не везде собирается).
    std::vector<std::string> keys;
    std::vector<Json> vals;
};

struct JsonParser {
    const std::string& t;
    size_t i = 0;
    std::string err;

    void ws() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i;
    }
    bool fail(const char* m) {
        if (err.empty()) err = std::string(m) + " at " + std::to_string(i);
        return false;
    }
    bool parseString(std::string& out) {
        if (i >= t.size() || t[i] != '"') return fail("expected string");
        ++i;
        out.clear();
        while (i < t.size() && t[i] != '"') {
            if (t[i] == '\\' && i + 1 < t.size()) {
                ++i;
                switch (t[i]) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'u':
                        i += 4;  // символы вне ASCII в именах не важны
                        out += '?';
                        break;
                    default: out += t[i];
                }
            } else {
                out += t[i];
            }
            ++i;
        }
        if (i >= t.size()) return fail("unterminated string");
        ++i;
        return true;
    }
    bool parse(Json& v, int depth = 0) {
        if (depth > 64) return fail("too deep");
        ws();
        if (i >= t.size()) return fail("unexpected end");
        const char c = t[i];
        if (c == '{') {
            v.type = Json::Type::Obj;
            ++i;
            ws();
            if (i < t.size() && t[i] == '}') return ++i, true;
            for (;;) {
                ws();
                std::string key;
                if (!parseString(key)) return false;
                ws();
                if (i >= t.size() || t[i] != ':') return fail("expected ':'");
                ++i;
                v.keys.push_back(key);
                v.vals.emplace_back();
                if (!parse(v.vals.back(), depth + 1)) return false;
                ws();
                if (i < t.size() && t[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < t.size() && t[i] == '}') return ++i, true;
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type = Json::Type::Arr;
            ++i;
            ws();
            if (i < t.size() && t[i] == ']') return ++i, true;
            for (;;) {
                v.arr.emplace_back();
                if (!parse(v.arr.back(), depth + 1)) return false;
                ws();
                if (i < t.size() && t[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < t.size() && t[i] == ']') return ++i, true;
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v.type = Json::Type::Str;
            return parseString(v.s);
        }
        if (t.compare(i, 4, "true") == 0) return v.type = Json::Type::Bool, v.b = true, i += 4, true;
        if (t.compare(i, 5, "false") == 0) return v.type = Json::Type::Bool, v.b = false, i += 5, true;
        if (t.compare(i, 4, "null") == 0) return v.type = Json::Type::Null, i += 4, true;
        char* end = nullptr;
        const double x = std::strtod(t.c_str() + i, &end);
        if (end == t.c_str() + i) return fail("unexpected character");
        v.type = Json::Type::Num;
        v.n = x;
        i = static_cast<size_t>(end - t.c_str());
        return true;
    }
};

const Json* field(const Json& o, std::initializer_list<const char*> keys) {
    if (o.type != Json::Type::Obj) return nullptr;
    for (const char* k : keys) {
        for (size_t i = 0; i < o.keys.size(); ++i)
            if (o.keys[i] == k) return &o.vals[i];
    }
    return nullptr;
}

bool truthy(const Json* j) {
    if (!j) return false;
    if (j->type == Json::Type::Bool) return j->b;
    if (j->type == Json::Type::Num) return j->n != 0;
    return false;
}

// Приводит кадры к 240 тикам в секунду.
void rescale(std::vector<Input>& macro, double fps, MacroInfo* info) {
    if (!(fps > 0) || std::fabs(fps - 240.0) < 1e-9) return;
    const double k = 240.0 / fps;
    for (Input& in : macro) in.frame = static_cast<int64_t>(std::llround(in.frame * k));
    if (info) info->note = "macro framerate " + std::to_string(fps) + " converted to 240 ticks per second";
}

void finish(std::vector<Input>& macro) {
    std::stable_sort(macro.begin(), macro.end(), [](const Input& a, const Input& b) {
        if (a.frame != b.frame) return a.frame < b.frame;
        return a.player2 < b.player2;
    });
}

}  // namespace

std::vector<Input> parseGdr2(const std::vector<uint8_t>& bytes, MacroInfo* info, std::string* error) {
    auto err = [&](const std::string& m) {
        if (error) *error = m;
        return std::vector<Input>{};
    };
    if (bytes.size() < 4 || std::memcmp(bytes.data(), "GDR", 3) != 0) return err("not a GDR file");
    Reader r{bytes, 0, false};
    r.pos = 3;
    const uint64_t version = r.varint();
    if (version != 2) return err("GDR format version " + std::to_string(version) + " is not supported (need 2)");
    const std::string tag = r.str();
    r.str();  // автор
    r.str();  // описание
    r.flt(4);  // длительность
    r.varint();  // версия игры
    const double fps = r.flt(8);
    r.varint();  // seed
    r.varint();  // монеты
    r.varint();  // ldm
    const bool platformer = r.varint() != 0;
    const std::string bot = r.str();
    r.varint();  // версия бота
    const uint64_t levelId = r.varint();
    const std::string levelName = r.str();
    if (r.bad) return err("GDR header is truncated or damaged");
    if (platformer) return err("platformer macros are not supported");
    r.skip(r.varint());  // расширение всего макроса
    const uint64_t deaths = r.varint();
    for (uint64_t i = 0; i < deaths && !r.bad; ++i) r.varint();
    r.varint();  // число входов
    uint64_t p1Left = r.varint();
    if (r.bad) return err("GDR file is truncated or damaged");

    std::vector<Input> macro;
    bool p2 = p1Left == 0;
    int64_t prev = 0;
    while (r.left() > 0) {
        const uint64_t packed = r.varint();
        if (r.bad) return err("GDR inputs are truncated or damaged");
        const int64_t frame = prev + static_cast<int64_t>(packed >> 1);
        macro.push_back(Input{frame, (packed & 1) != 0, p2});
        prev = frame;
        if (!tag.empty()) r.skip(r.varint());  // расширение входа (позиции и т.п.)
        if (r.bad) return err("GDR input extension is truncated or damaged");
        if (p1Left > 0 && --p1Left == 0) {
            prev = 0;
            p2 = true;
        }
    }
    finish(macro);
    if (info) {
        info->fps = std::isfinite(fps) && fps > 0 ? fps : 240.0;
        info->levelId = static_cast<int>(levelId);
        info->levelName = levelName;
        info->bot = bot;
    }
    rescale(macro, std::isfinite(fps) && fps > 0 ? fps : 240.0, info);
    return macro;
}

namespace {

std::vector<Input> macroFromValue(const Json& root, MacroInfo* info, std::string* error) {
    auto err = [&](const std::string& m) {
        if (error) *error = m;
        return std::vector<Input>{};
    };
    const Json* inputs = field(root, {"inputs"});
    if (!inputs || inputs->type != Json::Type::Arr) return err("JSON has no inputs list");
    std::vector<Input> macro;
    int skipped = 0;
    for (const Json& item : inputs->arr) {
        const Json* frame = field(item, {"frame", "f"});
        const Json* down = field(item, {"down", "d", "hold"});
        if (!frame || frame->type != Json::Type::Num || !down) return err("a JSON input has no frame or down");
        const Json* btn = field(item, {"btn", "button"});
        if (btn && btn->type == Json::Type::Num && static_cast<int>(btn->n) != 1) {
            ++skipped;
            continue;
        }
        macro.push_back(Input{static_cast<int64_t>(frame->n), truthy(down), truthy(field(item, {"2p", "player2", "p2"}))});
    }
    finish(macro);
    double fps = 240.0;
    if (const Json* f = field(root, {"framerate", "fps", "tps"}); f && f->type == Json::Type::Num && f->n > 0) fps = f->n;
    if (info) {
        info->fps = fps;
        if (const Json* lv = field(root, {"level"}); lv && lv->type == Json::Type::Obj) {
            if (const Json* id = field(*lv, {"id"}); id && id->type == Json::Type::Num) info->levelId = static_cast<int>(id->n);
            if (const Json* nm = field(*lv, {"name"}); nm && nm->type == Json::Type::Str) info->levelName = nm->s;
        }
        if (const Json* b = field(root, {"bot"})) {
            if (b->type == Json::Type::Obj) {
                if (const Json* nm = field(*b, {"name"}); nm && nm->type == Json::Type::Str) info->bot = nm->s;
            } else if (b->type == Json::Type::Str) {
                info->bot = b->s;
            }
        }
    }
    rescale(macro, fps, info);
    if (skipped && info) info->note += (info->note.empty() ? "" : "; ") + std::to_string(skipped) + " non-jump inputs skipped";
    return macro;
}

// ------------------------------------------------------------------ MessagePack (GDR 1, .gdr)
// Нужное подмножество: nil, bool, целые, float, строки, bin, массивы, словари. Расширения (ext) пропускаются.

struct MsgPack {
    const std::vector<uint8_t>& d;
    size_t i = 0;
    std::string err;

    bool fail(const char* m) {
        if (err.empty()) err = std::string(m) + " at " + std::to_string(i);
        return false;
    }
    bool need(size_t n) { return n <= d.size() - i || fail("unexpected end"); }
    uint64_t be(int n) {
        uint64_t v = 0;
        for (int k = 0; k < n; ++k) v = (v << 8) | d[i + k];
        i += n;
        return v;
    }
    bool str(size_t n, Json& v) {
        if (!need(n)) return false;
        v.type = Json::Type::Str;
        v.s.assign(reinterpret_cast<const char*>(d.data() + i), n);
        i += n;
        return true;
    }
    bool arr(size_t n, Json& v, int depth) {
        v.type = Json::Type::Arr;
        if (n > d.size() - i) return fail("bad array size");
        v.arr.resize(n);
        for (size_t k = 0; k < n; ++k)
            if (!parse(v.arr[k], depth + 1)) return false;
        return true;
    }
    bool map(size_t n, Json& v, int depth) {
        v.type = Json::Type::Obj;
        if (n > d.size() - i) return fail("bad map size");
        v.keys.resize(n);
        v.vals.resize(n);
        for (size_t k = 0; k < n; ++k) {
            Json key;
            if (!parse(key, depth + 1)) return false;
            if (key.type != Json::Type::Str) return fail("non-string map key");
            v.keys[k] = key.s;
            if (!parse(v.vals[k], depth + 1)) return false;
        }
        return true;
    }
    bool num(Json& v, double x) {
        v.type = Json::Type::Num;
        v.n = x;
        return true;
    }
    bool parse(Json& v, int depth = 0) {
        if (depth > 64) return fail("too deep");
        if (!need(1)) return false;
        const uint8_t t = d[i++];
        if (t <= 0x7f) return num(v, t);
        if (t >= 0xe0) return num(v, static_cast<int8_t>(t));
        if (t >= 0xa0 && t <= 0xbf) return str(t & 0x1f, v);
        if (t >= 0x90 && t <= 0x9f) return arr(t & 0x0f, v, depth);
        if (t >= 0x80 && t <= 0x8f) return map(t & 0x0f, v, depth);
        switch (t) {
            case 0xc0: v.type = Json::Type::Null; return true;
            case 0xc2: v.type = Json::Type::Bool; v.b = false; return true;
            case 0xc3: v.type = Json::Type::Bool; v.b = true; return true;
            case 0xca: {
                if (!need(4)) return false;
                uint32_t u = static_cast<uint32_t>(be(4));
                float f;
                std::memcpy(&f, &u, 4);
                return num(v, f);
            }
            case 0xcb: {
                if (!need(8)) return false;
                uint64_t u = be(8);
                double f;
                std::memcpy(&f, &u, 8);
                return num(v, f);
            }
            case 0xcc: if (!need(1)) return false; return num(v, static_cast<double>(be(1)));
            case 0xcd: if (!need(2)) return false; return num(v, static_cast<double>(be(2)));
            case 0xce: if (!need(4)) return false; return num(v, static_cast<double>(be(4)));
            case 0xcf: if (!need(8)) return false; return num(v, static_cast<double>(be(8)));
            case 0xd0: if (!need(1)) return false; return num(v, static_cast<int8_t>(be(1)));
            case 0xd1: if (!need(2)) return false; return num(v, static_cast<int16_t>(be(2)));
            case 0xd2: if (!need(4)) return false; return num(v, static_cast<int32_t>(be(4)));
            case 0xd3: if (!need(8)) return false; return num(v, static_cast<double>(static_cast<int64_t>(be(8))));
            case 0xd9: case 0xc4: { if (!need(1)) return false; size_t n = be(1); return str(n, v); }
            case 0xda: case 0xc5: { if (!need(2)) return false; size_t n = be(2); return str(n, v); }
            case 0xdb: case 0xc6: { if (!need(4)) return false; size_t n = be(4); return str(n, v); }
            case 0xdc: if (!need(2)) return false; return arr(be(2), v, depth);
            case 0xdd: if (!need(4)) return false; return arr(be(4), v, depth);
            case 0xde: if (!need(2)) return false; return map(be(2), v, depth);
            case 0xdf: if (!need(4)) return false; return map(be(4), v, depth);
            default: return fail("unsupported MessagePack type");
        }
    }
};

}  // namespace

std::vector<Input> parseGdrJson(const std::string& text, MacroInfo* info, std::string* error) {
    Json root;
    JsonParser p{text, 0, {}};
    if (!p.parse(root)) {
        if (error) *error = "bad JSON: " + p.err;
        return {};
    }
    return macroFromValue(root, info, error);
}

std::vector<Input> parseGdr1(const std::vector<uint8_t>& bytes, MacroInfo* info, std::string* error) {
    Json root;
    MsgPack p{bytes, 0, {}};
    if (!p.parse(root)) {
        if (error) *error = "bad MessagePack (GDR 1): " + p.err;
        return {};
    }
    return macroFromValue(root, info, error);
}

std::vector<Input> parseXd(const std::string& text, MacroInfo* info, std::string* error) {
    // Формат xdBot 1.x/2.x (.xd), по Macro::XDtoGDR: первая строка "<fps>" или "android",
    // дальше "кадр|нажато|кнопка|игрок2|только_позиция|...". Строки с позицией пропускаются.
    auto err = [&](const std::string& m) {
        if (error) *error = m;
        return std::vector<Input>{};
    };
    std::vector<Input> macro;
    double mult = 1.0;
    int skipped = 0;
    size_t pos = 0, lineNo = 0;
    while (pos <= text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        ++lineNo;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        std::vector<std::string> f;
        size_t a = 0;
        while (true) {
            size_t b = line.find('|', a);
            f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        char* endp = nullptr;
        if (f.size() < 4) {
            if (f[0] == "android") {
                mult = 4.0;
                if (info) info->fps = 60.0;
            } else {
                double fps = std::strtod(f[0].c_str(), &endp);
                if (endp == f[0].c_str() || !(fps > 0)) return err(".xd line " + std::to_string(lineNo) + ": bad framerate");
                mult = 240.0 / fps;
                if (info) info->fps = fps;
            }
            continue;
        }
        if (f.size() < 5) return err(".xd line " + std::to_string(lineNo) + ": too few fields");
        const double frame = std::strtod(f[0].c_str(), &endp);
        if (endp == f[0].c_str()) return err(".xd line " + std::to_string(lineNo) + ": bad frame");
        if (f[4] == "1") continue;  // запись позиции, не ввод
        if (std::atoi(f[2].c_str()) != 1) {
            ++skipped;
            continue;
        }
        macro.push_back(Input{static_cast<int64_t>(std::llround(frame * mult)), f[1] == "1", f[3] == "1"});
    }
    finish(macro);
    if (info) {
        info->bot = "xdBot (.xd)";
        info->note = "frames converted to 240 ticks per second";
        if (skipped) info->note += "; " + std::to_string(skipped) + " non-jump inputs skipped";
    }
    return macro;
}

std::vector<Input> parseMacroBytes(const std::vector<uint8_t>& bytes, MacroInfo* info, std::string* error) {
    if (bytes.empty()) {
        if (error) *error = "empty file";
        return {};
    }
    if (bytes.size() >= 3 && std::memcmp(bytes.data(), "GDR", 3) == 0) return parseGdr2(bytes, info, error);
    const uint8_t b0 = bytes[0];
    if ((b0 >= 0x80 && b0 <= 0x8f) || b0 == 0xde || b0 == 0xdf) return parseGdr1(bytes, info, error);
    std::string text(bytes.begin(), bytes.end());
    if (text.size() >= 3 && static_cast<uint8_t>(text[0]) == 0xEF) text.erase(0, 3);  // BOM
    size_t k = text.find_first_not_of(" \t\r\n");
    if (k != std::string::npos && text[k] == '{') return parseGdrJson(text, info, error);
    // .xd: первая строка число или "android", в следующих есть разделитель '|'.
    if (text.find('|') != std::string::npos) return parseXd(text, info, error);
    return parseMacroText(text, error);
}

}  // namespace probe
