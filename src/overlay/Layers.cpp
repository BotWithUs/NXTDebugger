#include "Layers.h"

#include "rpc/DrawParams.h"
#include "rpc/MsgPack.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace nxtdbg::overlay
{

using nxt::rpc::msgpack::Reader;
using nxt::rpc::msgpack::Type;
using nxt::rpc::msgpack::Writer;

namespace
{

constexpr int32_t kGameStateInGame = 30;
constexpr int     kMinDrawTtlMs    = 2000;
constexpr int     kMaxDrawTtlMs    = 60000;
constexpr int     kTtlIntervals    = 5;   // TTL = five refresh intervals
constexpr int     kResendMargin    = 2;   // re-send two intervals before expiry
constexpr int     kDrawThickness   = 2;
constexpr size_t  kBatchBufBytes   = 256 * 1024;

struct LayerInfo
{
    const char *token;
    const char *title;
    uint32_t    color;
    int         maxDistance;
    int         maxCount;
};

// Defaults from the brief: 20 tiles for actors, 15 for locs and ground; caps
// npc 100, player 50, loc 200, ground 100. Colours are distinct hues so two
// layers on at once read apart without labels.
constexpr LayerInfo kLayerInfo[kLayerCount] = {
    { "npc",    "NPCs",          0xFFFFC107u, 20, 100 },
    { "player", "Players",       0xFF5C9FE0u, 20,  50 },
    { "self",   "Self",          0xFF4AD191u, 20,   1 },
    { "loc",    "Scene objects", 0xFFB388FFu, 15, 200 },
    { "ground", "Ground items",  0xFFFF6E6Eu, 15, 100 },
};

constexpr const char *kLabelTokens[kLabelModeCount] = {
    "none", "name", "name_id", "name_id_dist",
};
constexpr const char *kLabelTitles[kLabelModeCount] = {
    "None", "Name", "Name + id", "Name + id + distance",
};

// Label priority across layers: the one thing always worth a caption first,
// then what moves, then what does not.
constexpr LayerId kLabelOrder[kLayerCount] = {
    LayerId::Self, LayerId::Npc, LayerId::Player, LayerId::Ground, LayerId::Loc,
};

int Idx(LayerId id)
{
    return static_cast<int>(id);
}

int Chebyshev(int ax, int ay, int bx, int by)
{
    const int dx = ax > bx ? ax - bx : bx - ax;
    const int dy = ay > by ? ay - by : by - ay;
    return dx > dy ? dx : dy;
}

int Clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

bool IsInWorld(const nxt::ipc::Snapshot &s)
{
    return s.gameState == kGameStateInGame && s.ownIndex >= 0 && s.self.serverIndex >= 0;
}

// Plane + distance gate shared by every layer: a candidate on another floor or
// past the cut-off is not a candidate at all, before any name is looked up.
bool InRange(const nxt::ipc::Snapshot &s, int x, int y, int plane, int maxDistance,
             int &outDistance)
{
    if (x < 0 || y < 0 || plane != s.self.plane)
    {
        return false;
    }
    outDistance = Chebyshev(x, y, s.self.tileX, s.self.tileY);
    return outDistance <= maxDistance;
}

LayerCandidate AtTile(int x, int y, int plane, int distance)
{
    LayerCandidate c;
    c.tileX    = x;
    c.tileY    = y;
    c.plane    = plane;
    c.distance = distance;
    return c;
}

void CollectNpcs(const nxt::ipc::Snapshot &s, int maxDistance, const EntityLookup &lookup,
                 std::vector<LayerCandidate> &out)
{
    const uint32_t n = std::min<uint32_t>(s.npcCount, nxt::ipc::kNpcCap);
    for (uint32_t i = 0; i < n; ++i)
    {
        const nxt::ipc::NpcEntry &e = s.npcs[i];
        int dist = 0;
        if (e.serverIndex < 0 || !InRange(s, e.tileX, e.tileY, e.plane, maxDistance, dist))
        {
            continue;
        }
        LayerCandidate c = AtTile(e.tileX, e.tileY, e.plane, dist);
        // The morph-resolved type carries the name the player sees.
        const int32_t shown = e.activeTypeId >= 0 ? e.activeTypeId : e.typeId;
        c.selectorId = e.serverIndex;
        c.shownId    = shown;
        c.altId      = e.typeId;
        c.name       = lookup.Name(NameKind::Npc, shown);
        out.push_back(std::move(c));
    }
}

const nxt::ipc::PlayerEntry *FindPlayer(const nxt::ipc::Snapshot &s, int32_t serverIndex)
{
    const uint32_t n = std::min<uint32_t>(s.playerCount, nxt::ipc::kPlayerCap);
    for (uint32_t i = 0; i < n; ++i)
    {
        if (s.players[i].serverIndex == serverIndex)
        {
            return &s.players[i];
        }
    }
    return nullptr;
}

std::string PlayerName(const nxt::ipc::PlayerEntry &p)
{
    return std::string(p.name, strnlen(p.name, sizeof(p.name)));
}

void CollectPlayers(const nxt::ipc::Snapshot &s, int maxDistance,
                    std::vector<LayerCandidate> &out)
{
    const uint32_t n = std::min<uint32_t>(s.playerCount, nxt::ipc::kPlayerCap);
    for (uint32_t i = 0; i < n; ++i)
    {
        const nxt::ipc::PlayerEntry &p = s.players[i];
        int dist = 0;
        // The local player is the Self layer's, not this one's.
        if (p.serverIndex < 0 || p.serverIndex == s.ownIndex
            || !InRange(s, p.tileX, p.tileY, p.plane, maxDistance, dist))
        {
            continue;
        }
        LayerCandidate c = AtTile(p.tileX, p.tileY, p.plane, dist);
        c.selectorId = p.serverIndex;
        c.shownId    = p.serverIndex;
        c.name       = PlayerName(p);
        out.push_back(std::move(c));
    }
}

void CollectSelf(const nxt::ipc::Snapshot &s, std::vector<LayerCandidate> &out)
{
    LayerCandidate c = AtTile(s.self.tileX, s.self.tileY, s.self.plane, 0);
    c.selectorId = s.self.serverIndex;
    c.shownId    = s.self.serverIndex;
    const nxt::ipc::PlayerEntry *row = FindPlayer(s, s.self.serverIndex);
    c.name = (row != nullptr && row->name[0] != '\0') ? PlayerName(*row) : std::string("You");
    out.push_back(std::move(c));
}

// The id batch-shape.md says a loc selector takes: interactId for a direct
// location, typeId for a combined section (whose interactId is -1).
int32_t LocSelectorId(const nxt::ipc::LocationEntry &l)
{
    return l.interactId >= 0 ? l.interactId : l.typeId;
}

bool IsDrawableLoc(const nxt::ipc::LocationEntry &l)
{
    constexpr uint8_t kSkip = nxt::ipc::kLocFlagHidden | nxt::ipc::kLocFlagDeleted;
    return (l.flags & kSkip) == 0 && LocSelectorId(l) >= 0;
}

void CollectLocs(const nxt::ipc::Snapshot &s, int maxDistance, const EntityLookup &lookup,
                 std::vector<LayerCandidate> &out)
{
    const uint32_t n = std::min<uint32_t>(s.locationCount, nxt::ipc::kLocationCap);
    for (uint32_t i = 0; i < n; ++i)
    {
        const nxt::ipc::LocationEntry &l = s.locations[i];
        int dist = 0;
        if (!IsDrawableLoc(l) || !InRange(s, l.tileX, l.tileY, l.plane, maxDistance, dist))
        {
            continue;
        }
        LayerCandidate c = AtTile(l.tileX, l.tileY, l.plane, dist);
        c.selectorId = LocSelectorId(l);
        c.shownId    = c.selectorId;
        c.altId      = l.resolvedId;   // the appearance id carries the name
        c.name       = lookup.Name(NameKind::Loc, l.resolvedId);
        int32_t sx = 1;
        int32_t sy = 1;
        if (!lookup.LocSize(c.selectorId, sx, sy))
        {
            sx = 1;
            sy = 1;
        }
        RotatedFootprint(sx, sy, l.rotation, c.w, c.h);
        out.push_back(std::move(c));
    }
}

void CollectGround(const nxt::ipc::Snapshot &s, int maxDistance, const EntityLookup &lookup,
                   std::vector<LayerCandidate> &out)
{
    const uint32_t n = std::min<uint32_t>(s.groundItemCount, nxt::ipc::kGroundItemCap);
    for (uint32_t i = 0; i < n; ++i)
    {
        const nxt::ipc::GroundItemEntry &g = s.groundItems[i];
        int dist = 0;
        if (g.itemId < 0 || !InRange(s, g.tileX, g.tileY, g.plane, maxDistance, dist))
        {
            continue;
        }
        LayerCandidate c = AtTile(g.tileX, g.tileY, g.plane, dist);
        c.selectorId = g.itemId;
        c.shownId    = g.itemId;
        c.quantity   = g.quantity;
        c.name       = lookup.Name(NameKind::Item, g.itemId);
        out.push_back(std::move(c));
    }
}

bool IsAllDigits(const char *s)
{
    if (*s == '\0')
    {
        return false;
    }
    for (; *s != '\0'; ++s)
    {
        if (!std::isdigit(static_cast<unsigned char>(*s)))
        {
            return false;
        }
    }
    return true;
}

bool ContainsNoCase(const std::string &hay, const char *needle)
{
    const size_t n = std::strlen(needle);
    if (n > hay.size())
    {
        return false;
    }
    for (size_t i = 0; i + n <= hay.size(); ++i)
    {
        size_t j = 0;
        while (j < n && std::tolower(static_cast<unsigned char>(hay[i + j]))
                        == std::tolower(static_cast<unsigned char>(needle[j])))
        {
            ++j;
        }
        if (j == n)
        {
            return true;
        }
    }
    return false;
}

void AddSelector(LayerId id, const LayerCandidate &c, rpc::ParamBuilder &p)
{
    switch (id)
    {
    case LayerId::Npc:
        p.AddInt("npc", c.selectorId);
        break;
    case LayerId::Player:
        p.AddInt("player", c.selectorId);
        break;
    case LayerId::Self:
        p.AddBool("self", true);
        break;
    case LayerId::Loc:
    case LayerId::Ground:
        p.AddInt(id == LayerId::Loc ? "loc" : "ground_item", c.selectorId);
        p.AddInt("x", c.tileX);
        p.AddInt("y", c.tileY);
        // Always sent: the agent defaults a missing plane to 0, not to the
        // local player's plane (batch-shape.md).
        p.AddInt("plane", c.plane);
        break;
    }
}

// One batch item in the settled shape: kind "entity" + highlight_entity params.
void BuildEntityItem(LayerId id, const DesiredDraw &d, uint32_t ttlMs, rpc::ParamBuilder &p)
{
    p.AddStr("kind", "entity");
    p.AddStr("key", d.key.c_str());
    AddSelector(id, d.candidate, p);
    if (d.candidate.w > 0 && d.candidate.h > 0)
    {
        p.AddInt("w", d.candidate.w);
        p.AddInt("h", d.candidate.h);
    }
    p.AddInt("color", static_cast<int64_t>(d.colorArgb));
    p.AddInt("thickness", kDrawThickness);
    p.AddInt("ttl_ms", ttlMs);
    if (!d.label.empty())
    {
        p.AddStr("label", d.label.c_str());
        p.AddStr("font", "small");
    }
}

bool ReadBatchCounts(const std::vector<uint8_t> &reply, BatchResult &out)
{
    Reader   r(reply.data(), reply.size());
    uint32_t n = 0;
    if (!r.ReadMapHeader(n))
    {
        return false;
    }
    for (uint32_t i = 0; i < n; ++i)
    {
        const char *k  = nullptr;
        uint32_t    kn = 0;
        if (!r.ReadString(k, kn))
        {
            return false;
        }
        const std::string_view key(k, kn);
        int64_t v = 0;
        if ((key == "count" || key == "dropped") && r.Peek() == Type::Int && r.ReadInt(v))
        {
            (key == "count" ? out.count : out.dropped) = static_cast<uint32_t>(v);
        }
        else if (key == "error" && r.Peek() == Type::Str)
        {
            rpc::ReadStrInto(r, out.error);
        }
        else
        {
            r.SkipValue();
        }
    }
    return true;
}

}   // namespace

// --- settings ---------------------------------------------------------------

const char *LayerToken(LayerId id)
{
    return kLayerInfo[Idx(id)].token;
}

const char *LayerTitle(LayerId id)
{
    return kLayerInfo[Idx(id)].title;
}

bool ParseLayerToken(std::string_view s, LayerId &outId)
{
    for (int i = 0; i < kLayerCount; ++i)
    {
        if (s == kLayerInfo[i].token)
        {
            outId = static_cast<LayerId>(i);
            return true;
        }
    }
    return false;
}

const char *LabelModeToken(LabelMode m)
{
    return kLabelTokens[static_cast<int>(m)];
}

const char *LabelModeTitle(LabelMode m)
{
    return kLabelTitles[static_cast<int>(m)];
}

bool ParseLabelMode(std::string_view s, LabelMode &outMode)
{
    for (int i = 0; i < kLabelModeCount; ++i)
    {
        if (s == kLabelTokens[i])
        {
            outMode = static_cast<LabelMode>(i);
            return true;
        }
    }
    return false;
}

LayerSettings DefaultLayerSettings(LayerId id)
{
    const LayerInfo &info = kLayerInfo[Idx(id)];
    LayerSettings    s;
    s.colorArgb   = info.color;
    s.maxDistance = info.maxDistance;
    s.maxCount    = info.maxCount;
    s.labels      = LabelMode::Name;
    return s;
}

LayerConfig DefaultLayerConfig()
{
    LayerConfig c;
    for (int i = 0; i < kLayerCount; ++i)
    {
        c.layers[i] = DefaultLayerSettings(static_cast<LayerId>(i));
    }
    return c;
}

void SanitizeConfig(LayerConfig &io)
{
    io.refreshMs   = Clamp(io.refreshMs, kMinRefreshMs, kMaxRefreshMs);
    io.labelBudget = Clamp(io.labelBudget, 0, kMaxLabelBudget);
    for (LayerSettings &s : io.layers)
    {
        s.maxDistance = Clamp(s.maxDistance, 0, kMaxLayerDistance);
        s.maxCount    = Clamp(s.maxCount, 1, kMaxLayerCount);
        if (static_cast<int>(s.labels) >= kLabelModeCount)
        {
            s.labels = LabelMode::Name;
        }
        s.filter[kFilterChars - 1] = '\0';
    }
    io.layers[Idx(LayerId::Self)].maxCount = 1;
}

bool AnyWorldLayerOn(const LayerConfig &c)
{
    for (const LayerSettings &s : c.layers)
    {
        if (s.isEnabled)
        {
            return true;
        }
    }
    return false;
}

// --- candidates ---------------------------------------------------------------

std::string EntityLookup::Name(NameKind kind, int32_t id) const
{
    return (name != nullptr && id >= 0) ? name(ctx, kind, id) : std::string();
}

bool EntityLookup::LocSize(int32_t locId, int32_t &outX, int32_t &outY) const
{
    return locSize != nullptr && locId >= 0 && locSize(ctx, locId, outX, outY);
}

void RotatedFootprint(int32_t sizeX, int32_t sizeY, int rotation, int32_t &outW, int32_t &outH)
{
    const int32_t x = sizeX > 0 ? sizeX : 1;
    const int32_t y = sizeY > 0 ? sizeY : 1;
    const bool    isQuarterTurn = (rotation & 1) != 0;
    outW = isQuarterTurn ? y : x;
    outH = isQuarterTurn ? x : y;
}

void CollectCandidates(LayerId id, const nxt::ipc::Snapshot &s, int maxDistance,
                       const EntityLookup &lookup, std::vector<LayerCandidate> &out)
{
    out.clear();
    if (!IsInWorld(s))
    {
        return;
    }
    switch (id)
    {
    case LayerId::Npc:    CollectNpcs(s, maxDistance, lookup, out);   break;
    case LayerId::Player: CollectPlayers(s, maxDistance, out);        break;
    case LayerId::Self:   CollectSelf(s, out);                        break;
    case LayerId::Loc:    CollectLocs(s, maxDistance, lookup, out);   break;
    case LayerId::Ground: CollectGround(s, maxDistance, lookup, out); break;
    }
}

bool MatchesFilter(const LayerCandidate &c, const char *filter)
{
    if (filter == nullptr || filter[0] == '\0')
    {
        return true;
    }
    if (IsAllDigits(filter))
    {
        const long long want = std::strtoll(filter, nullptr, 10);
        return want == c.selectorId || want == c.shownId || want == c.altId;
    }
    return ContainsNoCase(c.name, filter);
}

std::string LayerKeyPrefix(LayerId id)
{
    return id == LayerId::Self ? std::string("dbg:self")
                               : std::string("dbg:") + LayerToken(id) + ":";
}

std::string LayerKey(LayerId id, const LayerCandidate &c)
{
    char buf[64];
    switch (id)
    {
    case LayerId::Self:
        return "dbg:self";
    case LayerId::Npc:
    case LayerId::Player:
        std::snprintf(buf, sizeof(buf), "dbg:%s:%d", LayerToken(id), c.selectorId);
        return buf;
    case LayerId::Loc:
    case LayerId::Ground:
        break;
    }
    std::snprintf(buf, sizeof(buf), "dbg:%s:%d:%d:%d:%d", LayerToken(id),
                  c.selectorId, c.tileX, c.tileY, c.plane);
    return buf;
}

Selection SelectLayer(LayerId id, const LayerSettings &s, std::vector<LayerCandidate> &candidates)
{
    Selection                       sel;
    std::unordered_set<std::string> seen;
    std::vector<DesiredDraw>        all;
    for (LayerCandidate &c : candidates)
    {
        if (c.distance > s.maxDistance || !MatchesFilter(c, s.filter))
        {
            continue;
        }
        std::string key = LayerKey(id, c);
        if (!seen.insert(key).second)
        {
            continue;   // two stacks of one item on one tile are one highlight
        }
        all.push_back(DesiredDraw{ std::move(key), std::move(c), std::string(), s.colorArgb });
    }
    std::sort(all.begin(), all.end(), [](const DesiredDraw &a, const DesiredDraw &b)
    {
        if (a.candidate.distance != b.candidate.distance)
        {
            return a.candidate.distance < b.candidate.distance;
        }
        return a.key < b.key;
    });
    sel.matched = static_cast<uint32_t>(all.size());
    if (all.size() > static_cast<size_t>(s.maxCount))
    {
        all.resize(static_cast<size_t>(s.maxCount));
    }
    sel.draws = std::move(all);
    return sel;
}

std::string FormatLabel(LabelMode mode, LayerId id, const LayerCandidate &c)
{
    if (mode == LabelMode::None)
    {
        return {};
    }
    std::string text = c.name.empty() ? std::string("?") : c.name;
    if (id == LayerId::Ground && c.quantity > 1)
    {
        text += " x" + std::to_string(c.quantity);
    }
    if (mode == LabelMode::NameId || mode == LabelMode::NameIdDistance)
    {
        text += " #" + std::to_string(c.shownId);
    }
    if (mode == LabelMode::NameIdDistance)
    {
        text += " " + std::to_string(c.distance) + "t";
    }
    return text;
}

LabelTally ApplyLabels(const LayerConfig &cfg, Selection (&sel)[kLayerCount])
{
    struct Want
    {
        DesiredDraw *draw;
        LayerId      id;
        int          rank;   // position in kLabelOrder: breaks distance ties
    };
    std::vector<Want> wants;
    for (int rank = 0; rank < kLayerCount; ++rank)
    {
        const LayerId id = kLabelOrder[rank];
        for (DesiredDraw &d : sel[Idx(id)].draws)
        {
            d.label.clear();
            if (cfg.layers[Idx(id)].labels != LabelMode::None)
            {
                wants.push_back(Want{ &d, id, rank });
            }
        }
    }
    // Nearest first ACROSS layers: the caption budget goes to what is closest
    // to the player, whichever layer it belongs to.
    std::stable_sort(wants.begin(), wants.end(), [](const Want &a, const Want &b)
    {
        if (a.draw->candidate.distance != b.draw->candidate.distance)
        {
            return a.draw->candidate.distance < b.draw->candidate.distance;
        }
        return a.rank < b.rank;
    });
    LabelTally   tally;
    const size_t budget = static_cast<size_t>(Clamp(cfg.labelBudget, 0, kMaxLabelBudget));
    tally.wanted = static_cast<uint32_t>(wants.size());
    for (size_t i = 0; i < wants.size() && i < budget; ++i)
    {
        DesiredDraw &d = *wants[i].draw;
        d.label = FormatLabel(cfg.layers[Idx(wants[i].id)].labels, wants[i].id, d.candidate);
        ++tally.given;
    }
    return tally;
}

// --- diff -------------------------------------------------------------------

std::string DrawSignature(const DesiredDraw &d)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%08X|%d|%d|", d.colorArgb, d.candidate.w, d.candidate.h);
    return buf + d.label;
}

uint32_t DrawTtlMs(int refreshMs)
{
    const int r = Clamp(refreshMs, kMinRefreshMs, kMaxRefreshMs);
    return static_cast<uint32_t>(Clamp(r * kTtlIntervals, kMinDrawTtlMs, kMaxDrawTtlMs));
}

uint64_t ResendAgeMs(int refreshMs)
{
    const int r = Clamp(refreshMs, kMinRefreshMs, kMaxRefreshMs);
    const int64_t age = static_cast<int64_t>(DrawTtlMs(r)) - kResendMargin * r;
    return static_cast<uint64_t>(age > r ? age : r);
}

LayerDiff DiffLayer(const SentMap &sent, const std::vector<DesiredDraw> &want,
                    uint64_t nowMs, uint64_t resendAgeMs)
{
    LayerDiff                       diff;
    std::unordered_set<std::string> wanted;
    for (size_t i = 0; i < want.size(); ++i)
    {
        const DesiredDraw &d = want[i];
        wanted.insert(d.key);
        const auto it = sent.find(d.key);
        const bool isFresh = it != sent.end() && it->second.sentAtMs != 0
                          && it->second.signature == DrawSignature(d)
                          && nowMs - it->second.sentAtMs < resendAgeMs;
        if (!isFresh)
        {
            diff.toSend.push_back(i);
        }
    }
    for (const auto &kv : sent)
    {
        if (wanted.find(kv.first) == wanted.end())
        {
            diff.toClear.push_back(kv.first);
        }
    }
    std::sort(diff.toClear.begin(), diff.toClear.end());
    return diff;
}

void CommitSends(SentMap &io, const std::vector<DesiredDraw> &want,
                 const std::vector<size_t> &sentIdx, uint64_t nowMs)
{
    for (size_t i : sentIdx)
    {
        io[want[i].key] = SentDraw{ DrawSignature(want[i]), nowMs };
    }
}

void CommitClears(SentMap &io, const std::vector<std::string> &cleared)
{
    for (const std::string &k : cleared)
    {
        io.erase(k);
    }
}

void MarkAllStale(SentMap &io)
{
    for (auto &kv : io)
    {
        kv.second = SentDraw{};
    }
}

// --- wire -------------------------------------------------------------------

bool EncodeLayerBatch(LayerId id, const std::vector<DesiredDraw> &want,
                      const std::vector<size_t> &idx, uint32_t ttlMs,
                      std::vector<uint8_t> &out)
{
    out.assign(kBatchBufBytes, 0);
    Writer w(out.data(), out.size());
    w.WriteMapHeader(1);
    w.WriteCStr("items");
    w.WriteArrayHeader(static_cast<uint32_t>(idx.size()));
    for (size_t i : idx)
    {
        rpc::ParamBuilder item;
        BuildEntityItem(id, want[i], ttlMs, item);
        rpc::WriteParamMap(w, item);
    }
    if (w.Overflowed() || idx.size() > static_cast<size_t>(kMaxLayerCount))
    {
        out.clear();
        return false;
    }
    out.resize(w.BytesWritten());
    return true;
}

bool EncodeClearKeys(const std::vector<std::string> &keys, std::vector<uint8_t> &out)
{
    out.assign(kBatchBufBytes, 0);
    Writer w(out.data(), out.size());
    w.WriteMapHeader(1);
    w.WriteCStr("keys");
    w.WriteArrayHeader(static_cast<uint32_t>(keys.size()));
    for (const std::string &k : keys)
    {
        w.WriteString(k.data(), static_cast<uint32_t>(k.size()));
    }
    if (w.Overflowed())
    {
        out.clear();
        return false;
    }
    out.resize(w.BytesWritten());
    return true;
}

BatchResult ClassifyBatchReply(rpc::CallStatus status, const std::vector<uint8_t> &reply,
                               uint32_t itemCount)
{
    BatchResult res;
    if (status == rpc::CallStatus::HandlerError)
    {
        Reader r(reply.data(), reply.size());
        res.outcome = BatchOutcome::Rejected;
        res.error   = rpc::ValueToText(r);
        return res;
    }
    if (status != rpc::CallStatus::Ok)
    {
        res.outcome = BatchOutcome::Transport;
        res.error   = rpc::StatusText(status);
        return res;
    }
    if (!ReadBatchCounts(reply, res))
    {
        res.outcome = BatchOutcome::Transport;
        res.error   = "unreadable debug_draw_set_batch reply";
        return res;
    }
    if (res.dropped == 0)
    {
        res.outcome = BatchOutcome::Applied;
    }
    // Capacity ("draw store full (512 commands)", "draw text slots full (64)")
    // is shared with every other connection and frees up on its own, so a
    // batch refused for it is Partial and retried -- only a refusal of the
    // entries themselves latches the layer off.
    else if (res.count == 0 && res.dropped >= itemCount
             && res.error.find(" full") == std::string::npos)
    {
        res.outcome = BatchOutcome::Rejected;
    }
    else
    {
        res.outcome = BatchOutcome::Partial;
    }
    return res;
}

}
