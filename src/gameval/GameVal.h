#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace nxtdbg::gameval
{

// Loads the bundled "gameval" id -> symbolic-name tables
// (resources/gameval/*.json, copied next to nxt_debugger.exe at build time) and
// resolves cache-entity ids to Jagex's internal symbolic names (e.g. loc 3 ->
// "MCANNONDOOR", interface 11 -> "BANK_DEPOSIT_BOX"). Used as a *fallback* label
// wherever the cache has no display name. Per-type files are parsed lazily on
// first lookup, so the multi-MB tables (loc/component/model) cost nothing until
// browsed.
//
// Source: RS3 beta cache (build 947/948). Live ids match for the overwhelming
// majority of content; very recent additions may be unlabeled (returns nullptr).
class GameVal
{
public:
    // Resolves the resources/gameval directory next to the running exe. Cheap;
    // no files are read until the first lookup.
    void Init();

    // Looks up the symbolic name for a *cache/browse* type string (the same
    // strings the panels already hold: "npc", "loc", "item", "if", "varbit",
    // "varp", "struct", "enum", "param", "inv", "seq", "sprite", "model",
    // "dbrow", "quest"). Maps the few names that differ from the gameval file
    // (item->obj, sprite->graphic, varp->var_player, if->interface). Returns
    // nullptr on miss, on an unmapped type, or when id < 0. The returned pointer
    // is stable for the process lifetime (node-based map, never erased).
    const char *NameForCacheType(const char *cacheType, int32_t id);

    // Interface components are keyed by the composite id (iface << 16) | comp.
    const char *ComponentName(int32_t iface, int32_t comp);

    // Sorted list of every id known for a cache/browse type (same names as
    // NameForCacheType). Lets the cache browser populate its id list from the
    // bundled tables when the cache's own enumeration is unavailable. `out` is
    // cleared first; left empty for an unmapped type.
    void ListIds(const char *cacheType, std::vector<int> &out);

private:
    struct Table
    {
        bool                                     loaded = false;
        std::unordered_map<int32_t, std::string> entries;
    };

    Table             &LoadType(const std::string &gamevalType);
    const std::string *Lookup(const char *gamevalType, int32_t id);
    static const char *GamevalFile(const char *cacheType);

    std::wstring                           resDir_;   // ...\resources\gameval\ (trailing sep)
    std::unordered_map<std::string, Table> tables_;
};

}
