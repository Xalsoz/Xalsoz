#include "macro_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
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
    std::map<std::string, Json> obj;
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
                if (!parse(v.obj[key], depth + 1)) return false;
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
        auto it = o.obj.find(k);
        if (it != o.obj.end()) return &it->second;
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

std::vector<Input> parseGdrJson(const std::string& text, MacroInfo* info, std::string* error) {
    auto err = [&](const std::string& m) {
        if (error) *error = m;
        return std::vector<Input>{};
    };
    Json root;
    JsonParser p{text, 0, {}};
    if (!p.parse(root)) return err("bad JSON: " + p.err);
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

std::vector<Input> parseMacroBytes(const std::vector<uint8_t>& bytes, MacroInfo* info, std::string* error) {
    if (bytes.size() >= 3 && std::memcmp(bytes.data(), "GDR", 3) == 0) return parseGdr2(bytes, info, error);
    std::string text(bytes.begin(), bytes.end());
    if (text.size() >= 3 && static_cast<uint8_t>(text[0]) == 0xEF) text.erase(0, 3);  // BOM
    size_t k = text.find_first_not_of(" \t\r\n");
    if (k != std::string::npos && text[k] == '{') return parseGdrJson(text, info, error);
    return parseMacroText(text, error);
}

}  // namespace probe
