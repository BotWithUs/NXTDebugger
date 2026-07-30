#include "GameVal.h"

#include "log/Log.h"

#include <Windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

namespace nxtdbg::gameval
{

void GameVal::Init()
{
    wchar_t path[MAX_PATH] = {};
    DWORD   n              = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        log::LogWarn(L"GameVal: could not resolve exe path; symbolic names disabled");
        resDir_.clear();
        return;
    }
    std::wstring exe(path, n);
    size_t       slash = exe.find_last_of(L"\\/");
    std::wstring dir   = (slash == std::wstring::npos) ? std::wstring() : exe.substr(0, slash + 1);
    resDir_            = dir + L"resources\\gameval\\";
}

namespace
{

bool ReadFileUtf8(const std::wstring &path, std::string &out)
{
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f)
    {
        return false;
    }
    f.seekg(0, std::ios::end);
    std::streamoff len = f.tellg();
    if (len < 0)
    {
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(len));
    if (len > 0)
    {
        f.read(out.data(), len);
    }
    return static_cast<bool>(f);
}

}   // namespace

GameVal::Table &GameVal::LoadType(const std::string &gamevalType)
{
    Table &t = tables_[gamevalType];
    if (t.loaded)
    {
        return t;
    }
    t.loaded = true;   // mark first so a missing / malformed file isn't retried every frame

    if (resDir_.empty())
    {
        return t;
    }

    std::wstring path = resDir_;
    for (char c : gamevalType)
    {
        path.push_back(static_cast<wchar_t>(c));
    }
    path += L".json";

    std::string content;
    if (!ReadFileUtf8(path, content))
    {
        return t;   // file absent — leave the table empty (no name, same as before)
    }
    // Defensive: strip a UTF-8 BOM if one slipped in (the Python dump writes none).
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF)
    {
        content.erase(0, 3);
    }

    // Lightweight scan of the entries object: { "<intId>": "<NAME>", ... }.
    // Values are symbolic identifiers (A-Z, 0-9, '_') with no embedded quotes,
    // so a direct scan is correct and far cheaper than building a full JSON DOM
    // for the multi-MB tables (component 5.9 MB, loc 6.5 MB, model 6.7 MB) — the
    // DOM build was the source of the one-time interface-tree scroll hitch.
    const char *p   = content.c_str();
    const char *end = p + content.size();
    const char *e   = std::strstr(p, "\"entries\"");
    if (!e)
    {
        return t;
    }
    p = e + 9;   // past the "entries" key
    while (p < end && *p != '{') ++p;
    if (p >= end)
    {
        return t;
    }
    ++p;   // past '{'
    t.entries.reserve(content.size() / 40 + 16);
    while (p < end)
    {
        while (p < end && *p != '"' && *p != '}') ++p;   // to key opening quote
        if (p >= end || *p == '}') break;
        ++p;
        const char *keyStart = p;
        while (p < end && *p != '"') ++p;                // to key closing quote
        if (p >= end) break;
        long id = std::strtol(keyStart, nullptr, 10);
        ++p;
        while (p < end && *p != ':') ++p;                // to ':'
        if (p >= end) break;
        ++p;
        while (p < end && *p != '"' && *p != '}') ++p;   // to value opening quote
        if (p >= end || *p == '}') break;
        ++p;
        std::string val;
        while (p < end && *p != '"')
        {
            if (*p == '\\' && p + 1 < end) ++p;          // skip an escape, keep next byte
            val.push_back(*p);
            ++p;
        }
        if (p >= end) break;
        ++p;
        t.entries.emplace(static_cast<int32_t>(id), std::move(val));
    }
    return t;
}

const std::string *GameVal::Lookup(const char *gamevalType, int32_t id)
{
    if (!gamevalType || id < 0)
    {
        return nullptr;
    }
    Table &t  = LoadType(gamevalType);
    auto   it = t.entries.find(id);
    return it == t.entries.end() ? nullptr : &it->second;
}

const char *GameVal::NameForCacheType(const char *cacheType, int32_t id)
{
    const char *gv = GamevalFile(cacheType);
    if (!gv)
    {
        return nullptr;
    }
    const std::string *s = Lookup(gv, id);
    return s ? s->c_str() : nullptr;
}

void GameVal::ListIds(const char *cacheType, std::vector<int> &out)
{
    out.clear();
    const char *gv = GamevalFile(cacheType);
    if (!gv)
    {
        return;
    }
    Table &t = LoadType(gv);
    out.reserve(t.entries.size());
    for (const auto &kv : t.entries)
    {
        out.push_back(kv.first);
    }
    std::sort(out.begin(), out.end());
}

const char *GameVal::ComponentName(int32_t iface, int32_t comp)
{
    if (iface < 0 || comp < 0)
    {
        return nullptr;
    }
    int32_t            key = (iface << 16) | (comp & 0xFFFF);
    const std::string *s   = Lookup("component", key);
    return s ? s->c_str() : nullptr;
}

const char *GameVal::GamevalFile(const char *cacheType)
{
    if (!cacheType)
    {
        return nullptr;
    }
    struct Map
    {
        const char *cache;
        const char *gv;
    };
    // The few names that differ between the cache decoder and the gameval
    // archive, plus the pass-through types. Intentionally omits worldmap /
    // underlay / overlay / players (no gameval table exists).
    static constexpr Map kMap[] = {
        { "item",   "obj"        },
        { "sprite", "graphic"    },
        { "varp",   "var_player" },
        { "if",     "interface"  },
        { "npc",    "npc"        },
        { "loc",    "loc"        },
        { "varbit", "varbit"     },
        { "struct", "struct"     },
        { "enum",   "enum"       },
        { "param",  "param"      },
        { "inv",    "inv"        },
        { "seq",    "seq"        },
        { "model",  "model"      },
        { "dbrow",  "dbrow"      },
        { "quest",  "quest"      },
    };
    for (const auto &m : kMap)
    {
        if (std::strcmp(cacheType, m.cache) == 0)
        {
            return m.gv;
        }
    }
    return nullptr;
}

}   // namespace nxtdbg::gameval
