#include "Layers.h"

#include "rpc/DrawParams.h"
#include "rpc/MsgPack.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>
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
constexpr size_t  kEntityBufBytes  = 4 * 1024;   // one highlight_entity map
constexpr int     kMinFastTtlMs    = 750;
// World coordinates on the wire are sub-tiles (agent kWireSubtilesPerTile):
// a tile's minimum corner is tile * 256 and its centre tile * 256 + 128.
constexpr int32_t kSubtilesPerTile = 256;
constexpr int32_t kHalfTile        = kSubtilesPerTile / 2;

struct LayerInfo
{
    const char *token;
    const char *title;
    uint32_t    color;
    int         maxDistance;
    int         maxCount;
};

// Defaults from the brief: 20 tiles for actors, 15 for locs and ground; caps
// npc 100, player 50, loc 200, ground 100, projectile 50, spot anim 50.
// Colours are distinct hues so two layers on at once read apart without
// labels.
constexpr LayerInfo kLayerInfo[kLayerCount] = {
    { "npc",    "NPCs",               0xFFFFC107u, 20, 100 },
    { "player", "Players",            0xFF5C9FE0u, 20,  50 },
    { "self",   "Self",               0xFF4AD191u, 20,   1 },
    { "loc",    "Scene objects",      0xFFB388FFu, 15, 200 },
    { "ground", "Ground items",       0xFFFF6E6Eu, 15, 100 },
    { "proj",   "Projectiles",        0xFF26C6DAu, 30,  50 },
    { "spot",   "Spot anims (tiles)", 0xFFFF80ABu, 20,  50 },
};

struct LabelFieldInfo
{
    const char       *token;
    const char       *title;
    bool LabelFields::*member;
};

constexpr LabelFieldInfo kLabelFieldInfo[kLabelFieldCount] = {
    { "name",   "name",      &LabelFields::showName },
    { "id",     "id",        &LabelFields::showId },
    { "dist",   "distance",  &LabelFields::showDistance },
    { "coords", "coords",    &LabelFields::showCoords },
    { "anim",   "animation", &LabelFields::showAnim },
    { "spot",   "spot anim", &LabelFields::showSpotAnim },
    { "hp",     "hp",        &LabelFields::showHp },
    { "combat", "combat",    &LabelFields::showCombat },
    { "target", "target",    &LabelFields::showTarget },
};

// Label priority across layers, and the order names join in on a shared tile:
// the one thing always worth a caption first, then what moves, then what does
// not. The fast layers merge among themselves on their own refresh.
constexpr LayerId kEntityLabelOrder[] = {
    LayerId::Self, LayerId::Player, LayerId::Npc, LayerId::Ground, LayerId::Loc,
};
constexpr LayerId kFastLabelOrder[] = {
    LayerId::Projectile, LayerId::SpotAnim,
};

struct LayerOrder
{
    const LayerId *ids;
    int            count;
};

// Single-line labels (multi-row off): the names sharing a tile join on one
// line, for an agent that draws '\n' as a glyph.
constexpr const char *kLabelJoin = " | ";
// Multi-row labels: room kept beside one full row for a "\n+NNNN more" row, so
// overflow always keeps at least the first row.
constexpr size_t      kMoreRowReserve = 11;
constexpr size_t      kMaxRowChars    = kMaxLabelTextChars - kMoreRowReserve;

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

const nxt::ipc::NpcEntry *FindNpc(const nxt::ipc::Snapshot &s, int32_t serverIndex)
{
    const uint32_t n = std::min<uint32_t>(s.npcCount, nxt::ipc::kNpcCap);
    for (uint32_t i = 0; i < n; ++i)
    {
        if (s.npcs[i].serverIndex == serverIndex)
        {
            return &s.npcs[i];
        }
    }
    return nullptr;
}

std::string PlayerName(const nxt::ipc::PlayerEntry &p)
{
    return std::string(p.name, strnlen(p.name, sizeof(p.name)));
}

// The morph-resolved type carries the name the player sees.
int32_t NpcShownType(const nxt::ipc::NpcEntry &e)
{
    return e.activeTypeId >= 0 ? e.activeTypeId : e.typeId;
}

constexpr int kTargetPlayer  = 0;   // LocalPlayer::targetType (PROTOCOL.md)
constexpr int kTargetNpc     = 1;
constexpr int kTargetUnknown = -1;  // followingIndex carries no type

// Who a server index names. With a type (self's target) the list is known;
// without one (npc / player followingIndex) a name is given only when exactly
// one list holds that index, never a guess between two.
std::string TargetName(const nxt::ipc::Snapshot &s, const EntityLookup &lookup,
                       int32_t index, int type)
{
    if (index < 0)
    {
        return {};
    }
    const nxt::ipc::PlayerEntry *p = type == kTargetNpc ? nullptr : FindPlayer(s, index);
    const nxt::ipc::NpcEntry    *n = type == kTargetPlayer ? nullptr : FindNpc(s, index);
    if (p != nullptr && n != nullptr)
    {
        return {};
    }
    if (p != nullptr)
    {
        return PlayerName(*p);
    }
    return n != nullptr ? lookup.Name(NameKind::Npc, NpcShownType(*n)) : std::string();
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
        const int32_t shown = NpcShownType(e);
        c.selectorId  = e.serverIndex;
        c.shownId     = shown;
        c.altId       = e.typeId;
        c.name        = lookup.Name(NameKind::Npc, shown);
        c.animationId = e.animationId;
        c.spotAnimId  = e.spotAnimId;
        c.hp          = e.hp;
        c.maxHp       = e.maxHp;
        c.combatLevel = e.combatLevel;
        c.targetIndex = e.followingIndex;
        c.targetName  = TargetName(s, lookup, e.followingIndex, kTargetUnknown);
        out.push_back(std::move(c));
    }
}

void CollectPlayers(const nxt::ipc::Snapshot &s, int maxDistance, const EntityLookup &lookup,
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
        c.selectorId  = p.serverIndex;
        c.shownId     = p.serverIndex;
        c.name        = PlayerName(p);
        c.animationId = p.animationId;
        c.spotAnimId  = p.spotAnimId;
        c.combatLevel = p.combatLevel;
        c.targetIndex = p.followingIndex;
        c.targetName  = TargetName(s, lookup, p.followingIndex, kTargetUnknown);
        out.push_back(std::move(c));
    }
}

void CollectSelf(const nxt::ipc::Snapshot &s, const EntityLookup &lookup,
                 std::vector<LayerCandidate> &out)
{
    LayerCandidate c = AtTile(s.self.tileX, s.self.tileY, s.self.plane, 0);
    c.selectorId = s.self.serverIndex;
    c.shownId    = s.self.serverIndex;
    const nxt::ipc::PlayerEntry *row = FindPlayer(s, s.self.serverIndex);
    c.name = (row != nullptr && row->name[0] != '\0') ? PlayerName(*row) : std::string("You");
    c.animationId = s.self.animationId;
    c.spotAnimId  = s.self.spotAnimId;
    c.combatLevel = s.self.combatLevel;
    c.targetIndex = s.self.targetIndex;
    c.targetName  = TargetName(s, lookup, s.self.targetIndex, s.self.targetType);
    out.push_back(std::move(c));
}

// Placed at the target point (where it lands); `plane` always equals the
// local player's (the server sends no other).
void CollectProjectiles(const nxt::ipc::Snapshot &s, int maxDistance,
                        std::vector<LayerCandidate> &out)
{
    const uint32_t n = std::min<uint32_t>(s.projectileCount, nxt::ipc::kProjectileCap);
    for (uint32_t i = 0; i < n; ++i)
    {
        const nxt::ipc::ProjectileEntry &p = s.projectiles[i];
        int dist = 0;
        if (p.projectileId < 0 || !InRange(s, p.endTileX, p.endTileY, p.plane, maxDistance, dist))
        {
            continue;
        }
        LayerCandidate c = AtTile(p.endTileX, p.endTileY, p.plane, dist);
        c.selectorId  = p.projectileId;
        c.shownId     = p.projectileId;
        c.name        = "gfx " + std::to_string(p.projectileId);
        c.serial      = p.startCycle;
        c.sourceIndex = p.sourceIndex;
        c.targetIndex = p.targetIndex;
        c.fromX       = p.startTileX;
        c.fromY       = p.startTileY;
        out.push_back(std::move(c));
    }
}

bool IsSameSpot(const SpotAnimSighting &a, const SpotAnimSighting &b)
{
    return a.spotAnimId == b.spotAnimId && a.tileX == b.tileX && a.tileY == b.tileY
        && a.plane == b.plane;
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
    case LayerId::Projectile:
    case LayerId::SpotAnim:
        break;   // world shapes, not entities: BuildWorldParams
    }
}

int32_t TileCorner(int32_t tile)
{
    return tile * kSubtilesPerTile;
}

int32_t TileCentre(int32_t tile)
{
    return tile * kSubtilesPerTile + kHalfTile;
}

// One world-space command. A rect and a line may carry no text or font (the
// agent refuses a stray one), so the caption is its own text command.
void BuildWorldParams(const DesiredDraw &d, uint32_t ttlMs, rpc::ParamBuilder &p)
{
    const LayerCandidate &c = d.candidate;
    p.AddStr("key", d.key.c_str());
    p.AddStr("space", "world");
    p.AddInt("plane", c.plane);
    p.AddInt("color", static_cast<int64_t>(d.colorArgb));
    p.AddInt("ttl_ms", ttlMs);
    switch (d.prim)
    {
    case DrawPrim::TileBox:
        p.AddStr("kind", "rect");
        p.AddInt("x", TileCorner(c.tileX));
        p.AddInt("y", TileCorner(c.tileY));
        p.AddInt("w", kSubtilesPerTile);
        p.AddInt("h", kSubtilesPerTile);
        p.AddInt("thickness", kDrawThickness);
        break;
    case DrawPrim::Line:
        p.AddStr("kind", "line");
        p.AddInt("x1", TileCentre(c.fromX));
        p.AddInt("y1", TileCentre(c.fromY));
        p.AddInt("x2", TileCentre(c.tileX));
        p.AddInt("y2", TileCentre(c.tileY));
        p.AddInt("thickness", kDrawThickness);
        break;
    case DrawPrim::Text:
        p.AddStr("kind", "text");
        p.AddInt("x", TileCentre(c.tileX));
        p.AddInt("y", TileCentre(c.tileY));
        p.AddStr("text", d.label.c_str());
        p.AddStr("font", "small");
        break;
    case DrawPrim::Entity:
        break;   // EncodeLayerBatch sends these through BuildEntityParams
    }
}

// highlight_entity params. A batch item in the settled shape is the same map
// behind kind "entity".
void BuildEntityParams(LayerId id, const DesiredDraw &d, uint32_t ttlMs, rpc::ParamBuilder &p)
{
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

// "draw store full (512 commands)", "draw text slots full (64)".
bool IsCapacityError(const std::string &error)
{
    return error.find(" full") != std::string::npos;
}

// --- tile labels -------------------------------------------------------------

struct LabelWant
{
    DesiredDraw *draw;
    LayerId      id;
    int          rank;   // position in the label order
};

// Every labelled draw on one world tile (a loc counts at its footprint origin).
struct TileGroup
{
    int32_t                distance;   // nearest member
    std::vector<LabelWant> wants;      // layer order, then nearest; wants[0] carries
};

std::string TileKey(const LayerCandidate &c)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d:%d:%d", c.tileX, c.tileY, c.plane);
    return buf;
}

// Clears every label and groups the draws whose layer wants one by tile. A
// layer's draws are already nearest first and layers are visited in
// `order`, so a group's members arrive in caption order.
std::vector<TileGroup> GroupWantsByTile(const LayerConfig &cfg, Selection (&sel)[kLayerCount],
                                        LayerOrder order)
{
    std::vector<TileGroup>                  groups;
    std::unordered_map<std::string, size_t> byTile;
    for (int rank = 0; rank < order.count; ++rank)
    {
        const LayerId id       = order.ids[rank];
        const bool    isWanted = WantsLabel(id, cfg.layers[Idx(id)]);
        for (DesiredDraw &d : sel[Idx(id)].draws)
        {
            d.label.clear();
            if (!isWanted)
            {
                continue;
            }
            const auto it = byTile.emplace(TileKey(d.candidate), groups.size());
            if (it.second)
            {
                groups.push_back(TileGroup{ d.candidate.distance, {} });
            }
            TileGroup &g = groups[it.first->second];
            g.distance = std::min(g.distance, d.candidate.distance);
            g.wants.push_back(LabelWant{ &d, id, rank });
        }
    }
    return groups;
}

std::string JoinParts(const std::vector<std::string> &parts, size_t count)
{
    std::string out;
    for (size_t i = 0; i < count; ++i)
    {
        if (i > 0)
        {
            out += kLabelJoin;
        }
        out += parts[i];
    }
    return out;
}

std::string MoreSuffix(size_t dropped)
{
    return dropped == 0 ? std::string("...") : "... +" + std::to_string(dropped) + " more";
}

// The parts joined within maxChars. Whole parts are dropped from the end and
// counted ("A | B... +3 more"); a first part too long on its own is cut.
std::string JoinWithin(const std::vector<std::string> &parts, size_t maxChars)
{
    const std::string all = JoinParts(parts, parts.size());
    if (all.size() <= maxChars)
    {
        return all;
    }
    for (size_t keep = parts.size() - 1; keep >= 1; --keep)
    {
        const std::string head   = JoinParts(parts, keep);
        const std::string suffix = MoreSuffix(parts.size() - keep);
        if (head.size() + suffix.size() <= maxChars)
        {
            return head + suffix;
        }
    }
    const std::string suffix = MoreSuffix(parts.size() - 1);
    const size_t      room   = maxChars > suffix.size() ? maxChars - suffix.size() : 0;
    return parts[0].substr(0, room) + suffix;
}

// The rows of a multi-row label and, per row, the index of the thing it shows.
struct LabelRows
{
    std::vector<std::string> text;
    std::vector<size_t>      owner;
};

void PushRow(LabelRows &io, std::string row, size_t owner)
{
    io.text.push_back(std::move(row));
    io.owner.push_back(owner);
}

// One thing's fields as rows of at most rowChars: fields join with ' ' while
// they fit; only a field longer than rowChars on its own is cut at the width.
void WrapFields(const std::vector<std::string> &fields, size_t rowChars, size_t owner,
                LabelRows &io)
{
    std::string row;
    for (const std::string &f : fields)
    {
        if (!row.empty() && row.size() + 1 + f.size() <= rowChars)
        {
            row += ' ';
            row += f;
            continue;
        }
        if (!row.empty())
        {
            PushRow(io, std::move(row), owner);
        }
        size_t at = 0;
        while (f.size() - at > rowChars)
        {
            PushRow(io, f.substr(at, rowChars), owner);
            at += rowChars;
        }
        row = f.substr(at);
    }
    if (!row.empty())
    {
        PushRow(io, std::move(row), owner);
    }
}

// The first `count` rows joined with '\n'.
std::string JoinRows(const std::vector<std::string> &rows, size_t count)
{
    std::string out;
    for (size_t i = 0; i < count; ++i)
    {
        if (i > 0)
        {
            out += '\n';
        }
        out += rows[i];
    }
    return out;
}

// Every row when they fit the agent's row and text-slot limits; else the
// longest run of leading rows that fits with a last "+N more" row, N the
// things whose rows are not all shown.
std::string FitRows(const LabelRows &rows, size_t thingCount)
{
    const std::string all = JoinRows(rows.text, rows.text.size());
    if (rows.text.size() <= kMaxLabelRows && all.size() <= kMaxLabelTextChars)
    {
        return all;
    }
    for (size_t keep = std::min(rows.text.size() - 1, kMaxLabelRows - 1); keep >= 1; --keep)
    {
        const std::string head = JoinRows(rows.text, keep);
        const std::string more = "+" + std::to_string(thingCount - rows.owner[keep]) + " more";
        if (head.size() + 1 + more.size() <= kMaxLabelTextChars)
        {
            return head + '\n' + more;
        }
    }
    return "+" + std::to_string(thingCount) + " more";   // unreachable: kMoreRowReserve
}

// Identical field lists collapse to one with a count on its last field
// ("Guard x3"), first seen first; things with no fields are skipped.
std::vector<std::vector<std::string>> MergeThings(
    const std::vector<std::vector<std::string>> &things)
{
    std::vector<std::vector<std::string>> distinct;
    std::vector<int>                      counts;
    for (const std::vector<std::string> &t : things)
    {
        if (t.empty())
        {
            continue;
        }
        const auto it = std::find(distinct.begin(), distinct.end(), t);
        if (it != distinct.end())
        {
            ++counts[static_cast<size_t>(it - distinct.begin())];
            continue;
        }
        distinct.push_back(t);
        counts.push_back(1);
    }
    for (size_t i = 0; i < distinct.size(); ++i)
    {
        if (counts[i] > 1)
        {
            distinct[i].back() += " x" + std::to_string(counts[i]);
        }
    }
    return distinct;
}

// One tile's caption: multi-row from each draw's fields, or the single line.
std::string TileCaption(const LayerConfig &cfg, const TileGroup &g)
{
    const size_t width = static_cast<size_t>(
        Clamp(cfg.tileLabelChars, kMinTileLabelChars, kMaxTileLabelChars));
    if (cfg.isMultiRowLabels)
    {
        std::vector<std::vector<std::string>> things;
        for (const LabelWant &w : g.wants)
        {
            things.push_back(
                FormatLabelParts(cfg.layers[Idx(w.id)].labels, w.id, w.draw->candidate));
        }
        return MergeTileRows(things, width);
    }
    std::vector<std::string> labels;
    for (const LabelWant &w : g.wants)
    {
        labels.push_back(FormatLabel(cfg.layers[Idx(w.id)].labels, w.id, w.draw->candidate));
    }
    return MergeTileLabels(labels, width);
}

// Nearest first ACROSS layers: the caption budget goes to what is closest to
// the player, whichever layer it belongs to. One tile is one caption.
LabelTally ApplyLabelsIn(const LayerConfig &cfg, Selection (&sel)[kLayerCount],
                         LayerOrder order, int budget)
{
    std::vector<TileGroup> groups = GroupWantsByTile(cfg, sel, order);
    std::stable_sort(groups.begin(), groups.end(), [](const TileGroup &a, const TileGroup &b)
    {
        if (a.distance != b.distance)
        {
            return a.distance < b.distance;
        }
        return a.wants.front().rank < b.wants.front().rank;
    });
    LabelTally   tally;
    const size_t cap = static_cast<size_t>(Clamp(budget, 0, kMaxLabelBudget));
    for (TileGroup &g : groups)
    {
        std::string caption = TileCaption(cfg, g);
        if (caption.empty())
        {
            continue;   // e.g. "anim" only and nothing on the tile animating
        }
        ++tally.wanted;
        if (tally.given < cap)
        {
            g.wants.front().draw->label = std::move(caption);
            ++tally.given;
        }
    }
    return tally;
}

// "anim 808", "-> Zezima", "#12 -> tile": the per-actor and projectile parts.
std::string Endpoint(int32_t index)
{
    return index >= 0 ? "#" + std::to_string(index) : std::string("tile");
}

std::string TargetPart(LayerId id, const LayerCandidate &c)
{
    if (id == LayerId::Projectile)
    {
        return Endpoint(c.sourceIndex) + " -> " + Endpoint(c.targetIndex);
    }
    if (c.targetIndex < 0)
    {
        return {};
    }
    return "-> " + (c.targetName.empty() ? Endpoint(c.targetIndex) : c.targetName);
}

void AddPart(std::vector<std::string> &io, std::string part)
{
    if (!part.empty())
    {
        io.push_back(std::move(part));
    }
}

void AddActorParts(const LabelFields &f, LayerId id, const LayerCandidate &c,
                   std::vector<std::string> &io)
{
    if (f.showAnim && c.animationId >= 0 && LabelFieldApplies(id, LabelField::Anim))
    {
        io.push_back("anim " + std::to_string(c.animationId));
    }
    if (f.showSpotAnim && c.spotAnimId >= 0 && LabelFieldApplies(id, LabelField::SpotAnim))
    {
        io.push_back("spot " + std::to_string(c.spotAnimId));
    }
    if (f.showHp && c.maxHp > 0 && LabelFieldApplies(id, LabelField::Hp))
    {
        io.push_back("hp " + std::to_string(c.hp) + "/" + std::to_string(c.maxHp));
    }
    if (f.showCombat && c.combatLevel > 0 && LabelFieldApplies(id, LabelField::Combat))
    {
        io.push_back("lvl " + std::to_string(c.combatLevel));
    }
    if (f.showTarget && LabelFieldApplies(id, LabelField::Target))
    {
        AddPart(io, TargetPart(id, c));
    }
}

std::string FormatCoords(const LayerCandidate &c)
{
    return "(" + std::to_string(c.tileX) + "," + std::to_string(c.tileY) + ","
         + std::to_string(c.plane) + ")";
}

std::string NamePart(LayerId id, const LayerCandidate &c)
{
    std::string text = c.name.empty() ? std::string("?") : c.name;
    if (id == LayerId::Ground && c.quantity > 1)
    {
        text += " x" + std::to_string(c.quantity);
    }
    return text;
}

// The farthest last draw among the group's enabled layers; -1 when all empty.
int FarthestTail(const LayerConfig &cfg, bool isFast, const Selection (&sel)[kLayerCount])
{
    int best = -1;
    for (int i = 0; i < kLayerCount; ++i)
    {
        const LayerId id = static_cast<LayerId>(i);
        if (IsFastLayer(id) != isFast || !cfg.layers[i].isEnabled || sel[i].draws.empty())
        {
            continue;
        }
        if (best < 0 || sel[i].draws.back().candidate.distance
                            > sel[best].draws.back().candidate.distance)
        {
            best = i;
        }
    }
    return best;
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
    if (s == "projectile" || s == "projectiles")
    {
        outId = LayerId::Projectile;
        return true;
    }
    if (s == "spotanim" || s == "spotanims")
    {
        outId = LayerId::SpotAnim;
        return true;
    }
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

bool IsFastLayer(LayerId id)
{
    return id == LayerId::Projectile || id == LayerId::SpotAnim;
}

bool IsActorLayer(LayerId id)
{
    return id == LayerId::Npc || id == LayerId::Player || id == LayerId::Self;
}

// --- label fields -------------------------------------------------------------

const char *LabelFieldToken(LabelField f)
{
    return kLabelFieldInfo[static_cast<int>(f)].token;
}

const char *LabelFieldTitle(LabelField f)
{
    return kLabelFieldInfo[static_cast<int>(f)].title;
}

bool ParseLabelField(std::string_view s, LabelField &outField)
{
    if (s == "distance")
    {
        outField = LabelField::Distance;
        return true;
    }
    for (int i = 0; i < kLabelFieldCount; ++i)
    {
        if (s == kLabelFieldInfo[i].token)
        {
            outField = static_cast<LabelField>(i);
            return true;
        }
    }
    return false;
}

bool GetLabelField(const LabelFields &f, LabelField field)
{
    return f.*kLabelFieldInfo[static_cast<int>(field)].member;
}

void SetLabelField(LabelFields &io, LabelField field, bool isOn)
{
    io.*kLabelFieldInfo[static_cast<int>(field)].member = isOn;
}

bool LabelFieldApplies(LayerId id, LabelField field)
{
    switch (field)
    {
    case LabelField::Name:
    case LabelField::Id:
    case LabelField::Distance:
    case LabelField::Coords:
        return true;
    case LabelField::Anim:
    case LabelField::SpotAnim:
    case LabelField::Combat:
        return IsActorLayer(id);
    case LabelField::Hp:
        return id == LayerId::Npc;
    case LabelField::Target:
        return IsActorLayer(id) || id == LayerId::Projectile;
    }
    return false;
}

namespace
{

// The single-mode tokens the label combo wrote before label fields existed.
bool ParseLegacyLabels(std::string_view s, LabelFields &io)
{
    const bool isNone = s == "none";
    const bool isName = s == "name";
    const bool isId   = s == "name_id";
    const bool isDist = s == "name_id_dist";
    if (!isNone && !isName && !isId && !isDist)
    {
        return false;
    }
    io.showName     = !isNone;
    io.showId       = isId || isDist;
    io.showDistance = isDist;
    return true;
}

}   // namespace

bool ParseLabelFields(std::string_view s, LabelFields &io)
{
    if (ParseLegacyLabels(s, io))
    {
        return true;
    }
    LabelFields out;
    for (int i = 0; i < kLabelFieldCount; ++i)
    {
        SetLabelField(out, static_cast<LabelField>(i), false);
    }
    while (!s.empty())
    {
        const size_t           comma = s.find(',');
        const std::string_view token = s.substr(0, comma);
        if (!token.empty())
        {
            LabelField field = LabelField::Name;
            if (!ParseLabelField(token, field))
            {
                return false;   // io untouched: a bad list changes nothing
            }
            SetLabelField(out, field, true);
        }
        s = comma == std::string_view::npos ? std::string_view() : s.substr(comma + 1);
    }
    io = out;
    return true;
}

std::string LabelFieldsToken(const LabelFields &f)
{
    std::string out;
    for (int i = 0; i < kLabelFieldCount; ++i)
    {
        if (!GetLabelField(f, static_cast<LabelField>(i)))
        {
            continue;
        }
        if (!out.empty())
        {
            out += ',';
        }
        out += kLabelFieldInfo[i].token;
    }
    return out;
}

LayerSettings DefaultLayerSettings(LayerId id)
{
    const LayerInfo &info = kLayerInfo[Idx(id)];
    LayerSettings    s;
    s.colorArgb   = info.color;
    s.maxDistance = info.maxDistance;
    s.maxCount    = info.maxCount;
    s.labels      = LabelFields{};
    // A projectile's graphic id alone does not say who threw it at whom.
    s.labels.showTarget = id == LayerId::Projectile;
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
    io.refreshMs      = Clamp(io.refreshMs, kMinRefreshMs, kMaxRefreshMs);
    io.fastRefreshMs  = Clamp(io.fastRefreshMs, kMinFastRefreshMs, kMaxFastRefreshMs);
    io.labelBudget    = Clamp(io.labelBudget, 0, kMaxLabelBudget);
    io.tileLabelChars = Clamp(io.tileLabelChars, kMinTileLabelChars, kMaxTileLabelChars);
    for (int i = 0; i < kLayerCount; ++i)
    {
        LayerSettings &s = io.layers[i];
        const int maxCount = IsFastLayer(static_cast<LayerId>(i)) ? kMaxFastLayerCount
                                                                  : kMaxLayerCount;
        s.maxDistance = Clamp(s.maxDistance, 0, kMaxLayerDistance);
        s.maxCount    = Clamp(s.maxCount, 1, maxCount);
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

bool AnyLayerOn(const LayerConfig &c, bool isFast)
{
    for (int i = 0; i < kLayerCount; ++i)
    {
        if (c.layers[i].isEnabled && IsFastLayer(static_cast<LayerId>(i)) == isFast)
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
    case LayerId::Npc:        CollectNpcs(s, maxDistance, lookup, out);    break;
    case LayerId::Player:     CollectPlayers(s, maxDistance, lookup, out); break;
    case LayerId::Self:       CollectSelf(s, lookup, out);                 break;
    case LayerId::Loc:        CollectLocs(s, maxDistance, lookup, out);    break;
    case LayerId::Ground:     CollectGround(s, maxDistance, lookup, out);  break;
    case LayerId::Projectile: CollectProjectiles(s, maxDistance, out);     break;
    case LayerId::SpotAnim:   break;   // event-fed: CollectSpotAnims
    }
}

void RecordSpotAnim(std::vector<SpotAnimSighting> &io, const SpotAnimSighting &s)
{
    for (SpotAnimSighting &have : io)
    {
        if (IsSameSpot(have, s))
        {
            have.seenAtMs = s.seenAtMs;
            return;
        }
    }
    io.push_back(s);
}

void ExpireSpotAnims(std::vector<SpotAnimSighting> &io, uint64_t nowMs, uint64_t holdMs)
{
    std::erase_if(io, [nowMs, holdMs](const SpotAnimSighting &s)
    {
        return nowMs - s.seenAtMs >= holdMs;
    });
}

void CollectSpotAnims(const std::vector<SpotAnimSighting> &sightings,
                      const nxt::ipc::Snapshot &s, int maxDistance,
                      std::vector<LayerCandidate> &out)
{
    out.clear();
    if (!IsInWorld(s))
    {
        return;
    }
    for (const SpotAnimSighting &g : sightings)
    {
        int dist = 0;
        if (g.spotAnimId < 0 || !InRange(s, g.tileX, g.tileY, g.plane, maxDistance, dist))
        {
            continue;
        }
        LayerCandidate c = AtTile(g.tileX, g.tileY, g.plane, dist);
        c.selectorId = g.spotAnimId;
        c.shownId    = g.spotAnimId;
        c.name       = "spot " + std::to_string(g.spotAnimId);
        out.push_back(std::move(c));
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
    case LayerId::Projectile:
        std::snprintf(buf, sizeof(buf), "dbg:%s:%d:%d:%d:%d", LayerToken(id),
                      c.selectorId, c.serial, c.sourceIndex, c.targetIndex);
        return buf;
    case LayerId::Loc:
    case LayerId::Ground:
    case LayerId::SpotAnim:
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
    const bool isAnimatingOnly = s.isAnimatingOnly
                              && (id == LayerId::Npc || id == LayerId::Player);
    for (LayerCandidate &c : candidates)
    {
        if (c.distance > s.maxDistance || !MatchesFilter(c, s.filter)
            || (isAnimatingOnly && c.animationId < 0))
        {
            continue;
        }
        std::string key = LayerKey(id, c);
        if (!seen.insert(key).second)
        {
            continue;   // two stacks of one item on one tile are one highlight
        }
        all.push_back(DesiredDraw{ std::move(key), std::move(c), std::string(), s.colorArgb,
                                   IsFastLayer(id) ? DrawPrim::TileBox : DrawPrim::Entity });
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

bool WantsLabel(LayerId id, const LayerSettings &s)
{
    for (int i = 0; i < kLabelFieldCount; ++i)
    {
        const auto field = static_cast<LabelField>(i);
        if (GetLabelField(s.labels, field) && LabelFieldApplies(id, field))
        {
            return true;
        }
    }
    return false;
}

std::vector<std::string> FormatLabelParts(const LabelFields &fields, LayerId id,
                                          const LayerCandidate &c)
{
    std::vector<std::string> parts;
    if (fields.showName)
    {
        AddPart(parts, NamePart(id, c));
    }
    if (fields.showId)
    {
        parts.push_back("#" + std::to_string(c.shownId));
    }
    if (fields.showDistance)
    {
        parts.push_back(std::to_string(c.distance) + "t");
    }
    if (fields.showCoords)
    {
        parts.push_back(FormatCoords(c));
    }
    AddActorParts(fields, id, c, parts);
    return parts;
}

std::string FormatLabel(const LabelFields &fields, LayerId id, const LayerCandidate &c)
{
    std::string text;
    for (const std::string &p : FormatLabelParts(fields, id, c))
    {
        text += text.empty() ? p : " " + p;
    }
    return text;
}

uint32_t WireCommandCount(LayerId id, const LayerSettings &s, const DesiredDraw &d)
{
    if (!IsFastLayer(id))
    {
        return 1;
    }
    const LayerCandidate &c = d.candidate;
    const bool hasLine = id == LayerId::Projectile && c.fromX >= 0 && c.fromY >= 0
                      && (c.fromX != c.tileX || c.fromY != c.tileY);
    return 1u + (hasLine ? 1u : 0u) + (WantsLabel(id, s) ? 1u : 0u);
}

uint32_t FitStoreBudget(const LayerConfig &cfg, bool isFast, Selection (&sel)[kLayerCount],
                        uint32_t budget)
{
    uint32_t total = 0;
    for (int i = 0; i < kLayerCount; ++i)
    {
        const LayerId id = static_cast<LayerId>(i);
        if (IsFastLayer(id) != isFast || !cfg.layers[i].isEnabled)
        {
            continue;
        }
        for (const DesiredDraw &d : sel[i].draws)
        {
            total += WireCommandCount(id, cfg.layers[i], d);
        }
    }
    uint32_t dropped = 0;
    while (total > budget)
    {
        const int i = FarthestTail(cfg, isFast, sel);
        if (i < 0)
        {
            break;
        }
        total -= WireCommandCount(static_cast<LayerId>(i), cfg.layers[i], sel[i].draws.back());
        sel[i].draws.pop_back();
        ++dropped;
    }
    return dropped;
}

std::string MergeTileLabels(const std::vector<std::string> &labels, size_t maxChars)
{
    // Identical captions collapse to one part with a count, first seen first.
    std::vector<std::string> parts;
    std::vector<int>         counts;
    for (const std::string &l : labels)
    {
        if (l.empty())
        {
            continue;   // a layer whose fields have no value on this draw this tick
        }
        const auto it = std::find(parts.begin(), parts.end(), l);
        if (it != parts.end())
        {
            ++counts[static_cast<size_t>(it - parts.begin())];
            continue;
        }
        parts.push_back(l);
        counts.push_back(1);
    }
    if (parts.empty())
    {
        return {};
    }
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (counts[i] > 1)
        {
            parts[i] += " x" + std::to_string(counts[i]);
        }
    }
    return JoinWithin(parts, maxChars);
}

std::string MergeTileRows(const std::vector<std::vector<std::string>> &things, size_t rowChars)
{
    const std::vector<std::vector<std::string>> distinct = MergeThings(things);
    const size_t width = std::clamp(rowChars, size_t{ 1 }, kMaxRowChars);
    LabelRows    rows;
    for (size_t i = 0; i < distinct.size(); ++i)
    {
        WrapFields(distinct[i], width, i, rows);
    }
    return rows.text.empty() ? std::string() : FitRows(rows, distinct.size());
}

LabelTally ApplyLabels(const LayerConfig &cfg, Selection (&sel)[kLayerCount])
{
    const LayerOrder order{ kEntityLabelOrder, static_cast<int>(std::size(kEntityLabelOrder)) };
    return ApplyLabelsIn(cfg, sel, order, cfg.labelBudget);
}

LabelTally ApplyFastLabels(const LayerConfig &cfg, Selection (&sel)[kLayerCount], int budget)
{
    const LayerOrder order{ kFastLabelOrder, static_cast<int>(std::size(kFastLabelOrder)) };
    return ApplyLabelsIn(cfg, sel, order, budget);
}

std::vector<DesiredDraw> ExpandDraws(LayerId id, const std::vector<DesiredDraw> &draws)
{
    if (!IsFastLayer(id))
    {
        return draws;
    }
    std::vector<DesiredDraw> out;
    out.reserve(draws.size() * 3);
    for (const DesiredDraw &d : draws)
    {
        DesiredDraw box = d;
        box.prim = DrawPrim::TileBox;
        box.label.clear();
        out.push_back(box);
        const LayerCandidate &c = d.candidate;
        const bool hasLine = id == LayerId::Projectile && c.fromX >= 0 && c.fromY >= 0
                          && (c.fromX != c.tileX || c.fromY != c.tileY);
        if (hasLine)
        {
            DesiredDraw line = box;
            line.key += ":l";
            line.prim = DrawPrim::Line;
            out.push_back(std::move(line));
        }
        if (!d.label.empty())
        {
            DesiredDraw text = d;
            text.key += ":t";
            text.prim = DrawPrim::Text;
            out.push_back(std::move(text));
        }
    }
    return out;
}

// --- diff -------------------------------------------------------------------

std::string DrawSignature(const DesiredDraw &d)
{
    char buf[96];
    const LayerCandidate &c = d.candidate;
    if (d.prim == DrawPrim::Entity)
    {
        std::snprintf(buf, sizeof(buf), "%08X|%d|%d|", d.colorArgb, c.w, c.h);
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "%08X|%d|%d,%d,%d|%d,%d|", d.colorArgb,
                      static_cast<int>(d.prim), c.tileX, c.tileY, c.plane, c.fromX, c.fromY);
    }
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

uint32_t FastDrawTtlMs(int fastRefreshMs)
{
    const int r = Clamp(fastRefreshMs, kMinFastRefreshMs, kMaxFastRefreshMs);
    return static_cast<uint32_t>(Clamp(r * kTtlIntervals, kMinFastTtlMs, kMaxDrawTtlMs));
}

uint64_t FastResendAgeMs(int fastRefreshMs)
{
    const int r = Clamp(fastRefreshMs, kMinFastRefreshMs, kMaxFastRefreshMs);
    const int64_t age = static_cast<int64_t>(FastDrawTtlMs(r)) - kResendMargin * r;
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
        if (want[i].prim == DrawPrim::Entity)
        {
            item.AddStr("kind", "entity");
            BuildEntityParams(id, want[i], ttlMs, item);
        }
        else
        {
            BuildWorldParams(want[i], ttlMs, item);
        }
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
    else if (res.count == 0 && res.dropped >= itemCount && !IsCapacityError(res.error))
    {
        res.outcome = BatchOutcome::Rejected;
    }
    else
    {
        res.outcome = BatchOutcome::Partial;
    }
    return res;
}

// --- per-entity fallback ----------------------------------------------------

const char *AgentModeToken(AgentEntityMode m)
{
    switch (m)
    {
    case AgentEntityMode::Unknown:   return "unknown";
    case AgentEntityMode::Batch:     return "batch";
    case AgentEntityMode::PerEntity: return "per_entity";
    }
    return "unknown";
}

const char *AgentModeTitle(AgentEntityMode m)
{
    switch (m)
    {
    case AgentEntityMode::Unknown:   return "agent: not probed yet";
    case AgentEntityMode::Batch:     return "agent: batch entities";
    case AgentEntityMode::PerEntity: return "agent: legacy, per-entity";
    }
    return "agent: not probed yet";
}

AgentEntityMode ModeAfterBatch(AgentEntityMode current, const BatchResult &res)
{
    if (res.outcome == BatchOutcome::Rejected
        && res.error.find("unknown kind") != std::string::npos)
    {
        return AgentEntityMode::PerEntity;
    }
    if (res.outcome == BatchOutcome::Applied || res.outcome == BatchOutcome::Partial)
    {
        return AgentEntityMode::Batch;
    }
    return current;
}

std::vector<size_t> TakeCallBudget(const std::vector<size_t> &toSend, uint32_t &ioBudget)
{
    const size_t n = std::min(toSend.size(), static_cast<size_t>(ioBudget));
    ioBudget -= static_cast<uint32_t>(n);
    return std::vector<size_t>(toSend.begin(), toSend.begin() + static_cast<ptrdiff_t>(n));
}

bool EncodeHighlightEntity(LayerId id, const DesiredDraw &d, uint32_t ttlMs,
                           std::vector<uint8_t> &out)
{
    out.assign(kEntityBufBytes, 0);
    Writer w(out.data(), out.size());
    rpc::ParamBuilder p;
    BuildEntityParams(id, d, ttlMs, p);
    rpc::WriteParamMap(w, p);
    if (w.Overflowed())
    {
        out.clear();
        return false;
    }
    out.resize(w.BytesWritten());
    return true;
}

EntityCallResult ClassifyEntityReply(rpc::CallStatus status, const std::vector<uint8_t> &reply)
{
    EntityCallResult res;
    if (status == rpc::CallStatus::Ok)
    {
        res.outcome = EntityCallOutcome::Applied;
        return res;
    }
    if (status != rpc::CallStatus::HandlerError)
    {
        res.outcome = EntityCallOutcome::Transport;
        res.error   = rpc::StatusText(status);
        return res;
    }
    Reader r(reply.data(), reply.size());
    res.error   = rpc::ValueToText(r);
    res.outcome = IsCapacityError(res.error) ? EntityCallOutcome::Capacity
                                             : EntityCallOutcome::Refused;
    return res;
}

std::string RefusalMessage(LayerId id, AgentEntityMode mode, const std::string &agentError)
{
    const bool isLocOrGround = id == LayerId::Loc || id == LayerId::Ground;
    if (mode == AgentEntityMode::PerEntity && isLocOrGround)
    {
        return kNeedsNewerAgentMessage;
    }
    return agentError.empty() ? std::string("agent refused every entry") : agentError;
}

}
