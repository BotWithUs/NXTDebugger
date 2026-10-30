// Layer tests: the pure half of the overlay layers (src/overlay/Layers.cpp) --
// snapshot collection, filter / distance / cap selection, the dbg: key scheme,
// the diff against what was sent, label budgeting, the batch encoding the
// agent reads, the reply classification and the per-entity fallback for old
// agents (mode switch, per-refresh call cap).
//
// Same no-framework shape as WireTests.cpp: each CHECK prints what it compared
// and the exit code is the failure count. Every test drives the production
// entry point; the expected values are written out, never recomputed.

#include "overlay/Layers.h"

#include "ipc/SharedLayout.h"
#include "rpc/MsgPack.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{

using nxtdbg::overlay::BatchOutcome;
using nxtdbg::overlay::DesiredDraw;
using nxtdbg::overlay::EntityLookup;
using nxtdbg::overlay::LabelField;
using nxtdbg::overlay::LabelFields;
using nxtdbg::overlay::LayerCandidate;
using nxtdbg::overlay::LayerId;
using nxtdbg::overlay::NameKind;
using nxtdbg::overlay::Selection;
using nxtdbg::overlay::SentMap;
using nxt::rpc::msgpack::Reader;
using nxt::rpc::msgpack::Type;
using nxt::rpc::msgpack::Writer;

namespace ov = nxtdbg::overlay;

int failures = 0;

void CheckEq(const char *what, long long got, long long want)
{
    const bool isOk = got == want;
    std::printf("%s  %-52s got %lld, want %lld\n", isOk ? "PASS" : "FAIL", what, got, want);
    if (!isOk)
    {
        ++failures;
    }
}

void CheckStr(const char *what, const std::string &got, const char *want)
{
    const bool isOk = got == want;
    std::printf("%s  %-52s got \"%s\", want \"%s\"\n", isOk ? "PASS" : "FAIL", what,
                got.c_str(), want);
    if (!isOk)
    {
        ++failures;
    }
}

// --- fixtures ------------------------------------------------------------------

std::string FakeName(void *, NameKind kind, int32_t id)
{
    if (kind == NameKind::Npc && id == 3000) { return "Banker"; }
    if (kind == NameKind::Npc && id == 3001) { return "Guard"; }
    if (kind == NameKind::Loc && id == 501)  { return "Bank booth"; }
    if (kind == NameKind::Item)              { return "Coins"; }
    return {};
}

// Loc 500 is 2x3; everything else unknown (draws 1x1).
bool FakeLocSize(void *, int32_t id, int32_t &outX, int32_t &outY)
{
    if (id != 500)
    {
        return false;
    }
    outX = 2;
    outY = 3;
    return true;
}

const EntityLookup kLookup{ &FakeName, &FakeLocSize, nullptr };

std::unique_ptr<nxt::ipc::Snapshot> InWorldSnapshot()
{
    auto s = std::make_unique<nxt::ipc::Snapshot>();
    std::memset(s.get(), 0, sizeof(nxt::ipc::Snapshot));
    s->gameState        = 30;
    s->ownIndex         = 7;
    s->self.serverIndex = 7;
    s->self.tileX       = 3200;
    s->self.tileY       = 3200;
    s->self.plane       = 0;
    return s;
}

void AddNpc(nxt::ipc::Snapshot &s, int idx, int type, int x, int y, int plane)
{
    nxt::ipc::NpcEntry &n = s.npcs[s.npcCount++];
    n.serverIndex  = idx;
    n.typeId       = type;
    n.activeTypeId = -1;
    n.tileX        = static_cast<int16_t>(x);
    n.tileY        = static_cast<int16_t>(y);
    n.plane        = static_cast<int8_t>(plane);
}

LayerCandidate Cand(int id, int dist, const char *name)
{
    LayerCandidate c;
    c.selectorId = id;
    c.shownId    = id;
    c.distance   = dist;
    c.tileX      = 3200 + dist;
    c.tileY      = 3200;
    c.name       = name;
    return c;
}

// The fields a `labels=` list names, through the production parser.
LabelFields Fields(const char *list)
{
    LabelFields f;
    if (!ov::ParseLabelFields(list, f))
    {
        std::printf("FAIL  fixture: bad label list \"%s\"\n", list);
        ++failures;
    }
    return f;
}

// --- collection ------------------------------------------------------------------

void TestCollectNpcs()
{
    auto s = InWorldSnapshot();
    AddNpc(*s, 11, 3000, 3203, 3201, 0);   // 3 tiles
    AddNpc(*s, 12, 3001, 3230, 3200, 0);   // 30 tiles: past the cut-off
    AddNpc(*s, 13, 3000, 3201, 3200, 1);   // other plane
    std::vector<LayerCandidate> out;
    ov::CollectCandidates(LayerId::Npc, *s, 20, kLookup, out);
    CheckEq("npc: plane + distance gate", static_cast<long long>(out.size()), 1);
    CheckEq("npc: selector is the server index", out[0].selectorId, 11);
    CheckEq("npc: chebyshev distance", out[0].distance, 3);
    CheckStr("npc: name from the lookup", out[0].name, "Banker");

    s->gameState = 10;
    ov::CollectCandidates(LayerId::Npc, *s, 20, kLookup, out);
    CheckEq("not in-world: nothing", static_cast<long long>(out.size()), 0);
}

void TestCollectLocs()
{
    auto s = InWorldSnapshot();
    nxt::ipc::LocationEntry &direct = s->locations[s->locationCount++];
    direct = { 400, 500, -1, 3202, 3200, 0, 10, 1, 0, 501 };     // rotation 1
    nxt::ipc::LocationEntry &section = s->locations[s->locationCount++];
    section = { 600, -1, -1, 3201, 3200, 0, 10, 0, nxt::ipc::kLocFlagCombinedSection, 600 };
    nxt::ipc::LocationEntry &deleted = s->locations[s->locationCount++];
    deleted = { 700, 701, -1, 3201, 3201, 0, 10, 0, nxt::ipc::kLocFlagDeleted, 701 };
    std::vector<LayerCandidate> out;
    ov::CollectCandidates(LayerId::Loc, *s, 15, kLookup, out);
    CheckEq("loc: deleted row skipped", static_cast<long long>(out.size()), 2);
    CheckEq("loc: direct selector is interactId", out[0].selectorId, 500);
    CheckEq("loc: rotation 1 swaps footprint (w)", out[0].w, 3);
    CheckEq("loc: rotation 1 swaps footprint (h)", out[0].h, 2);
    CheckStr("loc: name from the appearance id", out[0].name, "Bank booth");
    CheckEq("loc: section selector is typeId", out[1].selectorId, 600);
    CheckEq("loc: unknown size draws 1 wide", out[1].w, 1);
}

void TestCollectPlayersAndSelf()
{
    auto s = InWorldSnapshot();
    nxt::ipc::PlayerEntry &me = s->players[s->playerCount++];
    me.serverIndex = 7;
    me.tileX = 3200;
    me.tileY = 3200;
    std::snprintf(me.name, sizeof(me.name), "Zezima");
    nxt::ipc::PlayerEntry &other = s->players[s->playerCount++];
    other.serverIndex = 9;
    other.tileX = 3205;
    other.tileY = 3200;
    std::vector<LayerCandidate> out;
    ov::CollectCandidates(LayerId::Player, *s, 20, kLookup, out);
    CheckEq("players: local player excluded", static_cast<long long>(out.size()), 1);
    CheckEq("players: other player kept", out[0].selectorId, 9);
    ov::CollectCandidates(LayerId::Self, *s, 20, kLookup, out);
    CheckStr("self: name from the player row", out[0].name, "Zezima");
}

// --- selection + keys ----------------------------------------------------------

void TestKeys()
{
    LayerCandidate c = Cand(500, 0, "x");
    c.tileX = 3202;
    c.tileY = 3201;
    c.plane = 1;
    CheckStr("key npc", ov::LayerKey(LayerId::Npc, c), "dbg:npc:500");
    CheckStr("key player", ov::LayerKey(LayerId::Player, c), "dbg:player:500");
    CheckStr("key self", ov::LayerKey(LayerId::Self, c), "dbg:self");
    CheckStr("key loc", ov::LayerKey(LayerId::Loc, c), "dbg:loc:500:3202:3201:1");
    CheckStr("key ground", ov::LayerKey(LayerId::Ground, c), "dbg:ground:500:3202:3201:1");
    CheckStr("prefix loc", ov::LayerKeyPrefix(LayerId::Loc), "dbg:loc:");
    LayerCandidate worst = Cand(2147483647, 0, "x");
    worst.tileX = 32767;
    worst.tileY = 32767;
    worst.plane = 3;
    CheckEq("longest ground key fits 47 bytes",
            ov::LayerKey(LayerId::Ground, worst).size() <= ov::kMaxDrawKeyBytes, 1);
}

void TestSelectCapAndOrder()
{
    ov::LayerSettings s = ov::DefaultLayerSettings(LayerId::Npc);
    s.maxCount = 2;
    std::vector<LayerCandidate> c = {
        Cand(1, 9, "Guard"), Cand(2, 1, "Banker"), Cand(3, 4, "Guard"), Cand(2, 1, "Banker"),
    };
    const Selection sel = ov::SelectLayer(LayerId::Npc, s, c);
    CheckEq("select: duplicate key counted once (M)", sel.matched, 3);
    CheckEq("select: capped to max_count (N)", static_cast<long long>(sel.draws.size()), 2);
    CheckEq("select: nearest first", sel.draws[0].candidate.selectorId, 2);
    CheckEq("select: then next nearest", sel.draws[1].candidate.selectorId, 3);
}

void TestSelectFilter()
{
    ov::LayerSettings s = ov::DefaultLayerSettings(LayerId::Npc);
    std::snprintf(s.filter, sizeof(s.filter), "bank");
    std::vector<LayerCandidate> c = { Cand(1, 1, "Guard"), Cand(2, 2, "Banker") };
    Selection sel = ov::SelectLayer(LayerId::Npc, s, c);
    CheckEq("filter: case-insensitive name substring",
            static_cast<long long>(sel.draws.size()), 1);
    std::snprintf(s.filter, sizeof(s.filter), "1");
    c = { Cand(1, 1, "Guard"), Cand(11, 2, "Banker1") };
    sel = ov::SelectLayer(LayerId::Npc, s, c);
    CheckEq("filter: digits are an exact id", static_cast<long long>(sel.draws.size()), 1);
    s.filter[0] = '\0';
    s.maxDistance = 1;
    c = { Cand(1, 1, "Guard"), Cand(2, 2, "Banker") };
    sel = ov::SelectLayer(LayerId::Npc, s, c);
    CheckEq("select: max_distance applies", static_cast<long long>(sel.draws.size()), 1);
}

void TestLabels()
{
    LayerCandidate c = Cand(995, 3, "Coins");
    c.quantity = 250;
    CheckStr("label name", ov::FormatLabel(Fields("name"), LayerId::Ground, c), "Coins x250");
    CheckStr("label name+id+dist",
             ov::FormatLabel(Fields("name,id,dist"), LayerId::Ground, c), "Coins x250 #995 3t");
    CheckStr("label none", ov::FormatLabel(Fields(""), LayerId::Ground, c), "");
    CheckStr("label coords only", ov::FormatLabel(Fields("coords"), LayerId::Ground, c),
             "(3203,3200,0)");
    CheckStr("label name + coords", ov::FormatLabel(Fields("name,coords"), LayerId::Ground, c),
             "Coins x250 (3203,3200,0)");
    CheckStr("label id without name", ov::FormatLabel(Fields("id"), LayerId::Ground, c), "#995");

    ov::LayerConfig cfg = ov::DefaultLayerConfig();
    Selection sel[ov::kLayerCount];
    for (int i = 0; i < 3; ++i)
    {
        LayerCandidate tree = Cand(i, i, "Tree");
        tree.tileY = 3210;   // its own tile: this is the budget, not the merge
        sel[static_cast<int>(LayerId::Loc)].draws.push_back(DesiredDraw{ "l" + std::to_string(i),
                                                            tree, "", 0 });
        sel[static_cast<int>(LayerId::Npc)].draws.push_back(DesiredDraw{ "n" + std::to_string(i),
                                                            Cand(i, i, "Man"), "", 0 });
    }
    cfg.labelBudget = 4;
    const ov::LabelTally tally = ov::ApplyLabels(cfg, sel);
    CheckEq("label budget: given", tally.given, 4);
    CheckEq("label budget: wanted", tally.wanted, 6);
    // Nearest first across layers: distances 0,0,1,1 get the four labels.
    CheckEq("label budget: loc at 1 tile labelled",
            sel[static_cast<int>(LayerId::Loc)].draws[1].label.empty(), 0);
    CheckEq("label budget: npc at 1 tile labelled",
            sel[static_cast<int>(LayerId::Npc)].draws[1].label.empty(), 0);
    CheckEq("label budget: npc at 2 tiles refused",
            sel[static_cast<int>(LayerId::Npc)].draws[2].label.empty(), 1);
    CheckEq("label budget: loc at 2 tiles refused",
            sel[static_cast<int>(LayerId::Loc)].draws[2].label.empty(), 1);
    CheckStr("label budget: label text", sel[static_cast<int>(LayerId::Npc)].draws[0].label,
             "Man");
}

DesiredDraw OnTile(const char *key, int id, int dist, int x, int y, const char *name)
{
    LayerCandidate c = Cand(id, dist, name);
    c.tileX = x;
    c.tileY = y;
    return DesiredDraw{ key, c, "", 0 };
}

std::vector<DesiredDraw> &Draws(Selection (&sel)[ov::kLayerCount], LayerId id)
{
    return sel[static_cast<int>(id)].draws;
}

void TestTileLabelGrouping()
{
    ov::LayerConfig cfg = ov::DefaultLayerConfig();
    Selection sel[ov::kLayerCount];
    // One tile: three goblins, a player, a bank booth. Another tile: one cow.
    Draws(sel, LayerId::Npc).push_back(OnTile("n1", 1, 2, 3202, 3200, "Goblin"));
    Draws(sel, LayerId::Npc).push_back(OnTile("n2", 2, 2, 3202, 3200, "Goblin"));
    Draws(sel, LayerId::Npc).push_back(OnTile("n3", 3, 2, 3202, 3200, "Goblin"));
    Draws(sel, LayerId::Npc).push_back(OnTile("n4", 4, 3, 3203, 3200, "Cow"));
    Draws(sel, LayerId::Player).push_back(OnTile("p1", 9, 2, 3202, 3200, "Zezima"));
    Draws(sel, LayerId::Loc).push_back(OnTile("l1", 501, 2, 3202, 3200, "Bank booth"));
    const ov::LabelTally tally = ov::ApplyLabels(cfg, sel);
    CheckEq("tile labels: one caption per tile (given)", tally.given, 2);
    CheckEq("tile labels: wanted counts tiles", tally.wanted, 2);
    CheckStr("tile labels: player carries, layer order, merged",
             Draws(sel, LayerId::Player)[0].label, "Zezima | Goblin x3 | Bank booth");
    CheckStr("tile labels: goblin keeps box, no label", Draws(sel, LayerId::Npc)[0].label, "");
    CheckStr("tile labels: loc keeps box, no label", Draws(sel, LayerId::Loc)[0].label, "");
    CheckStr("tile labels: other tile untouched", Draws(sel, LayerId::Npc)[3].label, "Cow");

    // Another plane is another tile.
    Selection planes[ov::kLayerCount];
    Draws(planes, LayerId::Npc).push_back(OnTile("n1", 1, 1, 3201, 3200, "Guard"));
    Draws(planes, LayerId::Npc).push_back(OnTile("n2", 2, 1, 3201, 3200, "Guard"));
    Draws(planes, LayerId::Npc)[1].candidate.plane = 1;
    ov::ApplyLabels(cfg, planes);
    CheckStr("tile labels: plane 0 guard", Draws(planes, LayerId::Npc)[0].label, "Guard");
    CheckStr("tile labels: plane 1 guard", Draws(planes, LayerId::Npc)[1].label, "Guard");
}

void TestTileLabelBudget()
{
    ov::LayerConfig cfg = ov::DefaultLayerConfig();
    cfg.labelBudget = 1;
    Selection sel[ov::kLayerCount];
    Draws(sel, LayerId::Npc).push_back(OnTile("n1", 1, 1, 3201, 3200, "Man"));
    Draws(sel, LayerId::Npc).push_back(OnTile("n2", 2, 1, 3201, 3200, "Woman"));
    Draws(sel, LayerId::Ground).push_back(OnTile("g1", 995, 1, 3201, 3200, "Coins"));
    Draws(sel, LayerId::Npc).push_back(OnTile("n3", 3, 4, 3204, 3200, "Rat"));
    const ov::LabelTally tally = ov::ApplyLabels(cfg, sel);
    CheckEq("tile budget: merged tile spends one", tally.given, 1);
    CheckEq("tile budget: wanted", tally.wanted, 2);
    CheckStr("tile budget: merged caption", Draws(sel, LayerId::Npc)[0].label,
             "Man | Woman | Coins");
    CheckStr("tile budget: far tile refused", Draws(sel, LayerId::Npc)[2].label, "");

    // A layer with labels off contributes nothing to the tile's caption.
    cfg.labelBudget = 40;
    cfg.layers[static_cast<int>(LayerId::Ground)].labels = Fields("");
    ov::ApplyLabels(cfg, sel);
    CheckStr("tile budget: labels-off layer left out", Draws(sel, LayerId::Npc)[0].label,
             "Man | Woman");
}

void TestTileLabelTruncation()
{
    CheckStr("merge: identical", ov::MergeTileLabels({ "Goblin", "Goblin", "Goblin" }),
             "Goblin x3");
    CheckStr("merge: empty", ov::MergeTileLabels({}), "");
    // 48-char cap: whole parts drop from the end and are counted.
    const std::string got = ov::MergeTileLabels(
        { "Guard", "Banker", "Man", "Woman", "Goblin", "Rat", "Cow", "Chicken", "Imp" });
    CheckStr("merge: truncated", got, "Guard | Banker | Man | Woman | Goblin... +4 more");
    CheckEq("merge: within cap", static_cast<long long>(got.size()) <= 48, 1);
    CheckStr("merge: exactly fits",
             ov::MergeTileLabels({ "Aaaaaaaaaaaaaaaaaaaaa", "Bbbbbbbbbbbbbbbbbbbbbbbb" }),
             "Aaaaaaaaaaaaaaaaaaaaa | Bbbbbbbbbbbbbbbbbbbbbbbb");
    CheckStr("merge: one long name cut",
             ov::MergeTileLabels({ std::string(60, 'n') }),
             (std::string(45, 'n') + "...").c_str());
    CheckStr("merge: long first of two cut",
             ov::MergeTileLabels({ std::string(60, 'n'), "Rat" }),
             (std::string(37, 'n') + "... +1 more").c_str());
}

// A carrier change on a tile is a label change on the same keys: the diff
// re-sends both as restyles and clears nothing.
void TestTileLabelCarrierDiff()
{
    ov::LayerConfig cfg = ov::DefaultLayerConfig();
    Selection sel[ov::kLayerCount];
    Draws(sel, LayerId::Npc).push_back(OnTile("dbg:npc:1", 1, 1, 3201, 3200, "Man"));
    Draws(sel, LayerId::Loc).push_back(OnTile("dbg:loc:5", 5, 1, 3201, 3200, "Tree"));
    ov::ApplyLabels(cfg, sel);
    SentMap npcSent;
    SentMap locSent;
    const std::vector<size_t> first = { 0 };
    ov::CommitSends(npcSent, Draws(sel, LayerId::Npc), first, 1000);
    ov::CommitSends(locSent, Draws(sel, LayerId::Loc), first, 1000);

    // The player walks onto the tile: the player now carries the caption.
    Draws(sel, LayerId::Player).push_back(OnTile("dbg:player:9", 9, 1, 3201, 3200, "Zezima"));
    ov::ApplyLabels(cfg, sel);
    CheckStr("carrier: player takes the caption", Draws(sel, LayerId::Player)[0].label,
             "Zezima | Man | Tree");
    const ov::LayerDiff npcDiff = ov::DiffLayer(npcSent, Draws(sel, LayerId::Npc), 1100, 10000);
    const ov::LayerDiff locDiff = ov::DiffLayer(locSent, Draws(sel, LayerId::Loc), 1100, 10000);
    CheckEq("carrier: old carrier restyled", static_cast<long long>(npcDiff.toSend.size()), 1);
    CheckEq("carrier: old carrier not cleared", static_cast<long long>(npcDiff.toClear.size()), 0);
    CheckEq("carrier: non-carrier unchanged", static_cast<long long>(locDiff.toSend.size()), 0);
    CheckEq("carrier: non-carrier not cleared", static_cast<long long>(locDiff.toClear.size()), 0);
}

// --- diff ---------------------------------------------------------------------

std::vector<DesiredDraw> Wanted(std::initializer_list<const char *> keys)
{
    std::vector<DesiredDraw> v;
    for (const char *k : keys)
    {
        v.push_back(DesiredDraw{ k, Cand(1, 1, "x"), "lbl", 0xFF00FF00u });
    }
    return v;
}

void TestDiff()
{
    SentMap sent;
    std::vector<DesiredDraw> want = Wanted({ "dbg:npc:1", "dbg:npc:2" });
    ov::LayerDiff d = ov::DiffLayer(sent, want, 1000, 1800);
    CheckEq("diff: first refresh sends all", static_cast<long long>(d.toSend.size()), 2);
    ov::CommitSends(sent, want, d.toSend, 1000);

    d = ov::DiffLayer(sent, want, 1600, 1800);
    CheckEq("diff: unchanged and fresh sends nothing",
            static_cast<long long>(d.toSend.size()), 0);

    want[1].label = "renamed";
    d = ov::DiffLayer(sent, want, 1600, 1800);
    CheckEq("diff: a changed label is re-sent", static_cast<long long>(d.toSend.size()), 1);
    CheckEq("diff: only the changed one", static_cast<long long>(d.toSend[0]), 1);

    want = Wanted({ "dbg:npc:1" });
    d = ov::DiffLayer(sent, want, 1600, 1800);
    CheckEq("diff: vanished key is cleared", static_cast<long long>(d.toClear.size()), 1);
    CheckStr("diff: the vanished key", d.toClear[0], "dbg:npc:2");
    ov::CommitClears(sent, d.toClear);
    CheckEq("commit clears forgets it", static_cast<long long>(sent.size()), 1);

    d = ov::DiffLayer(sent, want, 2800, 1800);
    CheckEq("diff: re-sent before its TTL runs out",
            static_cast<long long>(d.toSend.size()), 1);

    ov::MarkAllStale(sent);
    d = ov::DiffLayer(sent, want, 1001, 1800);
    CheckEq("stale: re-sent", static_cast<long long>(d.toSend.size()), 1);
    CheckEq("stale: key kept for clearing", static_cast<long long>(sent.size()), 1);
}

void TestTtl()
{
    CheckEq("ttl at 600 ms refresh", ov::DrawTtlMs(600), 3000);
    CheckEq("resend age at 600 ms", static_cast<long long>(ov::ResendAgeMs(600)), 1800);
    CheckEq("ttl floor", ov::DrawTtlMs(100), 2000);
    CheckEq("ttl outlives the refresh", ov::DrawTtlMs(10000) > 10000, 1);
}

void TestSanitize()
{
    ov::LayerConfig c = ov::DefaultLayerConfig();
    c.refreshMs = 5;
    c.layers[0].maxCount = 9999;
    c.layers[0].maxDistance = -4;
    ov::SanitizeConfig(c);
    CheckEq("sanitize refresh floor", c.refreshMs, ov::kMinRefreshMs);
    CheckEq("sanitize count <= batch cap", c.layers[0].maxCount, ov::kMaxLayerCount);
    CheckEq("sanitize distance >= 0", c.layers[0].maxDistance, 0);
    CheckEq("defaults: loc distance", ov::DefaultLayerSettings(LayerId::Loc).maxDistance, 15);
    CheckEq("defaults: loc count", ov::DefaultLayerSettings(LayerId::Loc).maxCount, 200);
    CheckEq("defaults: player count", ov::DefaultLayerSettings(LayerId::Player).maxCount, 50);
}

// --- wire -----------------------------------------------------------------------

struct ItemView
{
    std::string kind, key, label, space, text, font;
    int64_t     loc = -1, npc = -1, x = -1, y = -1, plane = -1, w = -1, h = -1, ttl = -1;
    int64_t     x1 = -1, y1 = -1, x2 = -1, y2 = -1;
    bool        isSelf = false;
};

std::string *StrSlot(ItemView &v, const std::string &key)
{
    return key == "kind" ? &v.kind : key == "key" ? &v.key : key == "label" ? &v.label
         : key == "space" ? &v.space : key == "text" ? &v.text : key == "font" ? &v.font
         : nullptr;
}

int64_t *IntSlot(ItemView &v, const std::string &key)
{
    return key == "loc" ? &v.loc : key == "npc" ? &v.npc
         : key == "x" ? &v.x : key == "y" ? &v.y
         : key == "plane" ? &v.plane : key == "w" ? &v.w : key == "h" ? &v.h
         : key == "ttl_ms" ? &v.ttl : key == "x1" ? &v.x1 : key == "y1" ? &v.y1
         : key == "x2" ? &v.x2 : key == "y2" ? &v.y2 : nullptr;
}

void ReadItem(Reader &r, ItemView &v)
{
    uint32_t n = 0;
    r.ReadMapHeader(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        const char *k = nullptr;
        uint32_t    kn = 0;
        r.ReadString(k, kn);
        const std::string key(k, kn);
        if (r.Peek() == Type::Str)
        {
            const char *s = nullptr;
            uint32_t    sn = 0;
            r.ReadString(s, sn);
            std::string *slot = StrSlot(v, key);
            if (slot != nullptr)
            {
                slot->assign(s, sn);
            }
            continue;
        }
        if (r.Peek() == Type::Bool)
        {
            r.ReadBool(v.isSelf);
            continue;
        }
        int64_t x = 0;
        r.ReadInt(x);
        int64_t *slot = IntSlot(v, key);
        if (slot != nullptr)
        {
            *slot = x;
        }
    }
}

void TestEncodeLocBatch()
{
    LayerCandidate c = Cand(500, 2, "Bank booth");
    c.tileX = 3202;
    c.tileY = 3201;
    c.plane = 0;
    c.w     = 3;
    c.h     = 2;
    std::vector<DesiredDraw> want = { DesiredDraw{ ov::LayerKey(LayerId::Loc, c), c,
                                                   "Bank booth", 0xFFB388FFu } };
    std::vector<uint8_t> body;
    CheckEq("encode loc batch", ov::EncodeLayerBatch(LayerId::Loc, want, { 0 }, 3000, body), 1);
    Reader   r(body.data(), body.size());
    uint32_t n = 0;
    r.ReadMapHeader(n);
    const char *k = nullptr;
    uint32_t    kn = 0;
    r.ReadString(k, kn);
    CheckStr("batch top key", std::string(k, kn), "items");
    r.ReadArrayHeader(n);
    CheckEq("batch item count", n, 1);
    ItemView v;
    ReadItem(r, v);
    CheckStr("item kind", v.kind, "entity");
    CheckStr("item key", v.key, "dbg:loc:500:3202:3201:0");
    CheckEq("item loc selector", v.loc, 500);
    CheckEq("item x", v.x, 3202);
    CheckEq("item y", v.y, 3201);
    CheckEq("item plane always sent", v.plane, 0);
    CheckEq("item w from cache footprint", v.w, 3);
    CheckEq("item h from cache footprint", v.h, 2);
    CheckEq("item ttl", v.ttl, 3000);
}

void TestEncodeClear()
{
    std::vector<uint8_t> body;
    ov::EncodeClearKeys({ "dbg:npc:1", "dbg:npc:2" }, body);
    Reader   r(body.data(), body.size());
    uint32_t n = 0;
    r.ReadMapHeader(n);
    const char *k = nullptr;
    uint32_t    kn = 0;
    r.ReadString(k, kn);
    CheckStr("clear uses keys[]", std::string(k, kn), "keys");
    r.ReadArrayHeader(n);
    CheckEq("clear key count", n, 2);
}

std::vector<uint8_t> Reply(uint32_t count, uint32_t dropped, const char *error)
{
    std::vector<uint8_t> out(256);
    Writer w(out.data(), out.size());
    w.WriteMapHeader(3);
    w.WriteCStr("count");
    w.WriteUInt(count);
    w.WriteCStr("dropped");
    w.WriteUInt(dropped);
    w.WriteCStr("error");
    if (error != nullptr) { w.WriteCStr(error); } else { w.WriteNil(); }
    out.resize(w.BytesWritten());
    return out;
}

void TestClassify()
{
    using nxtdbg::rpc::CallStatus;
    auto outcome = [](CallStatus st, const std::vector<uint8_t> &r, uint32_t n)
    {
        return static_cast<long long>(ov::ClassifyBatchReply(st, r, n).outcome);
    };
    CheckEq("classify applied", outcome(CallStatus::Ok, Reply(3, 0, nullptr), 3),
            static_cast<long long>(BatchOutcome::Applied));
    CheckEq("classify partial", outcome(CallStatus::Ok, Reply(2, 1, "draw store full (512 commands)"), 3),
            static_cast<long long>(BatchOutcome::Partial));
    CheckEq("classify all refused = rejected",
            outcome(CallStatus::Ok, Reply(0, 3, "unknown kind \"entity\""), 3),
            static_cast<long long>(BatchOutcome::Rejected));
    CheckEq("classify all refused for capacity = partial",
            outcome(CallStatus::Ok, Reply(0, 3, "draw text slots full (64)"), 3),
            static_cast<long long>(BatchOutcome::Partial));
    std::vector<uint8_t> err(64);
    Writer w(err.data(), err.size());
    w.WriteCStr("debug_draw_set_batch: malformed items");
    err.resize(w.BytesWritten());
    const ov::BatchResult res = ov::ClassifyBatchReply(CallStatus::HandlerError, err, 3);
    CheckEq("classify envelope error = rejected", static_cast<long long>(res.outcome),
            static_cast<long long>(BatchOutcome::Rejected));
    CheckStr("classify keeps the agent's message", res.error,
             "debug_draw_set_batch: malformed items");
    CheckEq("classify timeout = transport", outcome(CallStatus::Timeout, {}, 3),
            static_cast<long long>(BatchOutcome::Transport));
}

// --- per-entity fallback -----------------------------------------------------------

std::vector<uint8_t> ErrorReply(const char *text)
{
    std::vector<uint8_t> out(256);
    Writer w(out.data(), out.size());
    w.WriteCStr(text);
    out.resize(w.BytesWritten());
    return out;
}

long long Mode(ov::AgentEntityMode m)
{
    return static_cast<long long>(m);
}

// The exact refusal an agent before feat/overlay-loc-ground gives the batch,
// both as a per-item drop and as an envelope error.
constexpr const char *kOldAgentError =
    "unknown kind (want line, rect, ellipse, poly, text or component)";

void TestModeSwitch()
{
    using nxtdbg::rpc::CallStatus;
    using ov::AgentEntityMode;
    const ov::BatchResult dropped = ov::ClassifyBatchReply(CallStatus::Ok,
                                                           Reply(0, 3, kOldAgentError), 3);
    CheckEq("old agent drop -> per-entity",
            Mode(ov::ModeAfterBatch(AgentEntityMode::Unknown, dropped)),
            Mode(AgentEntityMode::PerEntity));
    const ov::BatchResult envelope = ov::ClassifyBatchReply(CallStatus::HandlerError,
                                                            ErrorReply(kOldAgentError), 3);
    CheckEq("old agent envelope error -> per-entity",
            Mode(ov::ModeAfterBatch(AgentEntityMode::Unknown, envelope)),
            Mode(AgentEntityMode::PerEntity));
    const ov::BatchResult applied = ov::ClassifyBatchReply(CallStatus::Ok, Reply(3, 0, nullptr), 3);
    CheckEq("applied batch -> batch", Mode(ov::ModeAfterBatch(AgentEntityMode::Unknown, applied)),
            Mode(AgentEntityMode::Batch));
    const ov::BatchResult full = ov::ClassifyBatchReply(
        CallStatus::Ok, Reply(1, 2, "draw text slots full (64)"), 3);
    CheckEq("partial batch -> batch", Mode(ov::ModeAfterBatch(AgentEntityMode::Unknown, full)),
            Mode(AgentEntityMode::Batch));
    const ov::BatchResult timeout = ov::ClassifyBatchReply(CallStatus::Timeout, {}, 3);
    CheckEq("timeout keeps unknown", Mode(ov::ModeAfterBatch(AgentEntityMode::Unknown, timeout)),
            Mode(AgentEntityMode::Unknown));
    const ov::BatchResult other = ov::ClassifyBatchReply(
        CallStatus::Ok, Reply(0, 3, "entity tile must be 0..32767"), 3);
    CheckEq("other refusal keeps batch", Mode(ov::ModeAfterBatch(AgentEntityMode::Batch, other)),
            Mode(AgentEntityMode::Batch));
    CheckStr("mode token per-entity", ov::AgentModeToken(AgentEntityMode::PerEntity),
             "per_entity");
    CheckStr("mode title batch", ov::AgentModeTitle(AgentEntityMode::Batch),
             "agent: batch entities");
    CheckStr("mode title per-entity", ov::AgentModeTitle(AgentEntityMode::PerEntity),
             "agent: legacy, per-entity");
}

void TestCallBudget()
{
    std::vector<size_t> toSend;
    for (size_t i = 0; i < 100; ++i)
    {
        toSend.push_back(i);
    }
    uint32_t budget = ov::kMaxEntityCallsPerRefresh;
    const std::vector<size_t> first = ov::TakeCallBudget(toSend, budget);
    CheckEq("budget: first layer capped at 64", static_cast<long long>(first.size()), 64);
    CheckEq("budget: nearest first", static_cast<long long>(first.front()), 0);
    CheckEq("budget: last taken", static_cast<long long>(first.back()), 63);
    CheckEq("budget: spent", budget, 0);
    const std::vector<size_t> second = ov::TakeCallBudget({ 0, 1 }, budget);
    CheckEq("budget: a later layer waits", static_cast<long long>(second.size()), 0);
    budget = 10;
    const std::vector<size_t> small = ov::TakeCallBudget({ 4, 7 }, budget);
    CheckEq("budget: under the cap takes all", static_cast<long long>(small.size()), 2);
    CheckEq("budget: remainder", budget, 8);
}

// The rest of a capped layer is still unsent, so the next refresh's diff
// carries it again: the cap spreads a crowd over refreshes, it drops nothing.
void TestCallBudgetSpreads()
{
    std::vector<DesiredDraw> want;
    for (int i = 0; i < 100; ++i)
    {
        LayerCandidate c = Cand(i, i % 20, "Guard");
        want.push_back(DesiredDraw{ ov::LayerKey(LayerId::Npc, c), c, "", 0xFFFFAB47u });
    }
    SentMap  sent;
    uint32_t budget = ov::kMaxEntityCallsPerRefresh;
    ov::LayerDiff d1 = ov::DiffLayer(sent, want, 1000, 1800);
    ov::CommitSends(sent, want, ov::TakeCallBudget(d1.toSend, budget), 1000);
    budget = ov::kMaxEntityCallsPerRefresh;
    ov::LayerDiff d2 = ov::DiffLayer(sent, want, 1600, 1800);
    CheckEq("spread: second refresh sends the rest", static_cast<long long>(d2.toSend.size()), 36);
    ov::CommitSends(sent, want, ov::TakeCallBudget(d2.toSend, budget), 1600);
    CheckEq("spread: all sent after two refreshes", static_cast<long long>(sent.size()), 100);
}

void TestEntityReplies()
{
    using nxtdbg::rpc::CallStatus;
    using ov::EntityCallOutcome;
    auto outcome = [](CallStatus st, const std::vector<uint8_t> &r)
    {
        return static_cast<long long>(ov::ClassifyEntityReply(st, r).outcome);
    };
    CheckEq("entity ok", outcome(CallStatus::Ok, {}),
            static_cast<long long>(EntityCallOutcome::Applied));
    CheckEq("entity store full = capacity",
            outcome(CallStatus::HandlerError, ErrorReply("draw store full (512 commands)")),
            static_cast<long long>(EntityCallOutcome::Capacity));
    const ov::EntityCallResult loc = ov::ClassifyEntityReply(
        CallStatus::HandlerError, ErrorReply("highlight_entity needs npc, player or self"));
    CheckEq("entity old-agent loc = refused", static_cast<long long>(loc.outcome),
            static_cast<long long>(EntityCallOutcome::Refused));
    CheckEq("entity timeout = transport", outcome(CallStatus::Timeout, {}),
            static_cast<long long>(EntityCallOutcome::Transport));
    using ov::AgentEntityMode;
    CheckStr("refusal: legacy loc names the DLL",
             ov::RefusalMessage(LayerId::Loc, AgentEntityMode::PerEntity, loc.error),
             "needs the newer agent DLL (feat/overlay-loc-ground)");
    CheckStr("refusal: legacy ground names the DLL",
             ov::RefusalMessage(LayerId::Ground, AgentEntityMode::PerEntity, loc.error),
             "needs the newer agent DLL (feat/overlay-loc-ground)");
    CheckStr("refusal: legacy npc keeps the agent's error",
             ov::RefusalMessage(LayerId::Npc, AgentEntityMode::PerEntity, "bad npc"), "bad npc");
    CheckStr("refusal: batch loc keeps the agent's error",
             ov::RefusalMessage(LayerId::Loc, AgentEntityMode::Batch, "x"), "x");
}

void TestEncodeHighlightEntity()
{
    LayerCandidate c = Cand(42, 3, "Guard");
    const DesiredDraw d{ ov::LayerKey(LayerId::Npc, c), c, "Guard", 0xFFFFAB47u };
    std::vector<uint8_t> body;
    CheckEq("encode highlight_entity", ov::EncodeHighlightEntity(LayerId::Npc, d, 3000, body), 1);
    Reader   r(body.data(), body.size());
    ItemView v;
    ReadItem(r, v);
    CheckStr("highlight_entity has no kind", v.kind, "");
    CheckStr("highlight_entity key", v.key, "dbg:npc:42");
    CheckEq("highlight_entity npc selector", v.npc, 42);
    CheckStr("highlight_entity label", v.label, "Guard");
    CheckEq("highlight_entity ttl", v.ttl, 3000);
}

}

// --- label fields -----------------------------------------------------------------

void TestLegacyLabelTokens()
{
    LabelFields f;
    f.showCoords = true;   // an old ini's separate `coords=1` line
    CheckEq("legacy name_id parses", ov::ParseLabelFields("name_id", f), 1);
    CheckEq("legacy name_id: name", f.showName, 1);
    CheckEq("legacy name_id: id", f.showId, 1);
    CheckEq("legacy name_id: no distance", f.showDistance, 0);
    CheckEq("legacy token keeps coords", f.showCoords, 1);
    ov::ParseLabelFields("name_id_dist", f);
    CheckEq("legacy name_id_dist: distance", f.showDistance, 1);
    ov::ParseLabelFields("none", f);
    CheckEq("legacy none: name off", f.showName, 0);
    CheckEq("legacy none: id off", f.showId, 0);
    CheckEq("legacy none: coords kept", f.showCoords, 1);
    ov::ParseLabelFields("name", f);
    CheckStr("legacy name -> list", ov::LabelFieldsToken(f), "name,coords");

    CheckStr("list round trip", ov::LabelFieldsToken(Fields("target,name,anim,coords")),
             "name,coords,anim,target");
    CheckEq("list: distance alias", Fields("distance").showDistance, 1);
    CheckStr("empty list: all off", ov::LabelFieldsToken(Fields("")), "");
    LabelFields keep = Fields("name,hp");
    CheckEq("bad list refused", ov::ParseLabelFields("name,bogus", keep), 0);
    CheckStr("bad list changes nothing", ov::LabelFieldsToken(keep), "name,hp");
    LabelField field = LabelField::Name;
    CheckEq("field token spot", ov::ParseLabelField("spot", field), 1);
    CheckEq("field token spot -> SpotAnim", static_cast<long long>(field),
            static_cast<long long>(LabelField::SpotAnim));
}

void AddPlayer(nxt::ipc::Snapshot &s, int idx, int x, const char *name)
{
    nxt::ipc::PlayerEntry &p = s.players[s.playerCount++];
    p.serverIndex    = idx;
    p.tileX          = static_cast<int16_t>(x);
    p.tileY          = 3200;
    p.animationId    = -1;
    p.spotAnimId     = -1;
    p.followingIndex = -1;
    std::snprintf(p.name, sizeof(p.name), "%s", name);
}

void TestActorLabelFields()
{
    auto s = InWorldSnapshot();
    AddPlayer(*s, 9, 3205, "Zezima");
    AddNpc(*s, 11, 3000, 3203, 3200, 0);
    nxt::ipc::NpcEntry &n = s->npcs[0];
    n.animationId    = 808;
    n.spotAnimId     = 90;
    n.hp             = 50;
    n.maxHp          = 100;
    n.combatLevel    = 12;
    n.followingIndex = 9;
    std::vector<LayerCandidate> out;
    ov::CollectCandidates(LayerId::Npc, *s, 20, kLookup, out);
    const LabelFields all = Fields("name,anim,spot,hp,combat,target");
    CheckStr("npc: every actor field", ov::FormatLabel(all, LayerId::Npc, out[0]),
             "Banker anim 808 spot 90 hp 50/100 lvl 12 -> Zezima");
    out[0].animationId = -1;
    out[0].spotAnimId  = -1;
    CheckStr("npc: idle fields left out", ov::FormatLabel(all, LayerId::Npc, out[0]),
             "Banker hp 50/100 lvl 12 -> Zezima");

    // Player 11 as well as npc 11: an index with no type names no one.
    AddPlayer(*s, 12, 3206, "Lynx");
    s->players[1].followingIndex = 11;
    AddPlayer(*s, 11, 3207, "Twin");
    ov::CollectCandidates(LayerId::Player, *s, 20, kLookup, out);
    CheckStr("player: ambiguous target is an index",
             ov::FormatLabel(Fields("target"), LayerId::Player, out[1]), "-> #11");
    CheckStr("player: no hp in the snapshot",
             ov::FormatLabel(Fields("name,hp"), LayerId::Player, out[1]), "Lynx");

    // Self's target carries a type, so the same index resolves.
    s->self.targetIndex = 11;
    s->self.targetType  = 1;
    s->self.animationId = -1;
    ov::CollectCandidates(LayerId::Self, *s, 20, kLookup, out);
    CheckStr("self: typed target names the npc",
             ov::FormatLabel(Fields("target"), LayerId::Self, out[0]), "-> Banker");

    LayerCandidate loc = Cand(501, 2, "Bank booth");
    loc.animationId = 5;
    CheckStr("loc: actor fields ignored", ov::FormatLabel(Fields("name,anim"), LayerId::Loc, loc),
             "Bank booth");
    ov::LayerSettings ls = ov::DefaultLayerSettings(LayerId::Loc);
    ls.labels = Fields("anim");
    CheckEq("wants label: anim only on a loc", ov::WantsLabel(LayerId::Loc, ls), 0);
    CheckEq("wants label: anim only on an npc", ov::WantsLabel(LayerId::Npc, ls), 1);
}

void TestAnimatingOnly()
{
    ov::LayerSettings s = ov::DefaultLayerSettings(LayerId::Npc);
    s.isAnimatingOnly = true;
    std::vector<LayerCandidate> c = { Cand(1, 1, "Idle"), Cand(2, 2, "Busy") };
    c[1].animationId = 808;
    const Selection sel = ov::SelectLayer(LayerId::Npc, s, c);
    CheckEq("animating only: idle dropped", static_cast<long long>(sel.draws.size()), 1);
    CheckEq("animating only: animating kept", sel.draws[0].candidate.selectorId, 2);
    CheckEq("animating only: matched counts after the filter", sel.matched, 1);
    std::vector<LayerCandidate> locs = { Cand(1, 1, "Tree") };
    CheckEq("animating only: not a loc filter",
            static_cast<long long>(ov::SelectLayer(LayerId::Loc, s, locs).draws.size()), 1);
}

// Merged tile captions are cut to cfg.tileLabelChars.
void TestTileLabelChars()
{
    CheckStr("merge: 24-char cap",
             ov::MergeTileLabels({ "Guard", "Banker", "Man", "Woman", "Goblin" }, 24),
             "Guard... +4 more");
    ov::LayerConfig cfg = ov::DefaultLayerConfig();
    cfg.tileLabelChars = 120;
    Selection sel[ov::kLayerCount];
    for (int i = 0; i < 8; ++i)
    {
        Draws(sel, LayerId::Npc).push_back(OnTile(("n" + std::to_string(i)).c_str(), i, 1,
                                                  3201, 3200, ("Npc" + std::to_string(i)).c_str()));
    }
    ov::ApplyLabels(cfg, sel);
    CheckStr("label chars 120: all eight fit", Draws(sel, LayerId::Npc)[0].label,
             "Npc0 | Npc1 | Npc2 | Npc3 | Npc4 | Npc5 | Npc6 | Npc7");
    cfg.tileLabelChars = 24;
    ov::ApplyLabels(cfg, sel);
    CheckStr("label chars 24: cut", Draws(sel, LayerId::Npc)[0].label,
             "Npc0 | Npc1... +6 more");
}

// --- projectiles / spot anims -----------------------------------------------------

void AddProjectile(nxt::ipc::Snapshot &s, int gfx, int cycle, int src, int tgt, int toX, int toY)
{
    nxt::ipc::ProjectileEntry &p = s.projectiles[s.projectileCount++];
    p.projectileId = gfx;
    p.startCycle   = cycle;
    p.sourceIndex  = static_cast<int16_t>(src);
    p.targetIndex  = static_cast<int16_t>(tgt);
    p.startTileX   = 3200;
    p.startTileY   = 3200;
    p.endTileX     = static_cast<int16_t>(toX);
    p.endTileY     = static_cast<int16_t>(toY);
}

void TestCollectProjectiles()
{
    auto s = InWorldSnapshot();
    AddProjectile(*s, 1234, 5000, 7, 11, 3205, 3203);
    AddProjectile(*s, 1235, 5001, 7, -1, 3260, 3200);   // 60 tiles: past the cut-off
    std::vector<LayerCandidate> out;
    ov::CollectCandidates(LayerId::Projectile, *s, 30, kLookup, out);
    CheckEq("proj: distance gate", static_cast<long long>(out.size()), 1);
    CheckEq("proj: placed at the target tile x", out[0].tileX, 3205);
    CheckEq("proj: placed at the target tile y", out[0].tileY, 3203);
    CheckEq("proj: distance to the target tile", out[0].distance, 5);
    CheckEq("proj: launch tile kept", out[0].fromX, 3200);
    CheckStr("proj: key", ov::LayerKey(LayerId::Projectile, out[0]), "dbg:proj:1234:5000:7:11");
    CheckStr("proj: prefix", ov::LayerKeyPrefix(LayerId::Projectile), "dbg:proj:");
    CheckStr("proj: default label (name + target)",
             ov::FormatLabel(ov::DefaultLayerSettings(LayerId::Projectile).labels,
                             LayerId::Projectile, out[0]),
             "gfx 1234 #7 -> #11");
    out[0].targetIndex = -1;
    CheckStr("proj: tile target", ov::FormatLabel(Fields("target"), LayerId::Projectile, out[0]),
             "#7 -> tile");

    LayerCandidate worst = Cand(2147483647, 0, "x");   // a graphic id is never negative
    worst.serial      = -2147483647 - 1;
    worst.sourceIndex = -32768;
    worst.targetIndex = -32768;
    CheckEq("longest projectile caption key fits 47 bytes",
            (ov::LayerKey(LayerId::Projectile, worst) + ":t").size() <= ov::kMaxDrawKeyBytes, 1);
}

Selection OneProjectile(int toX)
{
    LayerCandidate c = Cand(1234, 5, "gfx 1234");
    c.serial      = 5000;
    c.sourceIndex = 7;
    c.targetIndex = 11;
    c.fromX       = 3200;
    c.fromY       = 3200;
    c.tileX       = toX;
    std::vector<LayerCandidate> cands = { c };
    return ov::SelectLayer(LayerId::Projectile,
                           ov::DefaultLayerSettings(LayerId::Projectile), cands);
}

void TestProjectileExpandAndDiff()
{
    ov::LayerConfig cfg = ov::DefaultLayerConfig();
    Selection sel[ov::kLayerCount];
    sel[static_cast<int>(LayerId::Projectile)] = OneProjectile(3205);
    ov::ApplyFastLabels(cfg, sel, 10);
    std::vector<DesiredDraw> want = ov::ExpandDraws(LayerId::Projectile,
                                                    Draws(sel, LayerId::Projectile));
    CheckEq("expand: box + line + caption", static_cast<long long>(want.size()), 3);
    CheckStr("expand: box key", want[0].key, "dbg:proj:1234:5000:7:11");
    CheckStr("expand: line key", want[1].key, "dbg:proj:1234:5000:7:11:l");
    CheckStr("expand: caption key", want[2].key, "dbg:proj:1234:5000:7:11:t");
    CheckStr("expand: box carries no label", want[0].label, "");
    CheckStr("expand: caption text", want[2].label, "gfx 1234 #7 -> #11");

    SentMap sent;
    ov::LayerDiff d = ov::DiffLayer(sent, want, 1000, ov::FastResendAgeMs(150));
    CheckEq("proj diff: first refresh sends all three", static_cast<long long>(d.toSend.size()), 3);
    ov::CommitSends(sent, want, d.toSend, 1000);
    d = ov::DiffLayer(sent, want, 1150, ov::FastResendAgeMs(150));
    CheckEq("proj diff: unchanged sends nothing", static_cast<long long>(d.toSend.size()), 0);

    // The target walks a tile: same keys, new geometry -> all three re-sent.
    sel[static_cast<int>(LayerId::Projectile)] = OneProjectile(3206);
    ov::ApplyFastLabels(cfg, sel, 10);
    want = ov::ExpandDraws(LayerId::Projectile, Draws(sel, LayerId::Projectile));
    d = ov::DiffLayer(sent, want, 1300, ov::FastResendAgeMs(150));
    CheckEq("proj diff: moved target re-sent", static_cast<long long>(d.toSend.size()), 3);
    CheckEq("proj diff: moved target not cleared", static_cast<long long>(d.toClear.size()), 0);

    // It lands: every one of its commands is cleared.
    d = ov::DiffLayer(sent, {}, 1450, ov::FastResendAgeMs(150));
    CheckEq("proj diff: landed clears all three", static_cast<long long>(d.toClear.size()), 3);
    CheckStr("proj diff: clears the line", d.toClear[1], "dbg:proj:1234:5000:7:11:l");

    // No label budget left: box and line only. Launched on its own tile: no line.
    ov::ApplyFastLabels(cfg, sel, 0);
    CheckEq("expand: no caption without budget", static_cast<long long>(
                ov::ExpandDraws(LayerId::Projectile, Draws(sel, LayerId::Projectile)).size()), 2);
    sel[static_cast<int>(LayerId::Projectile)] = OneProjectile(3200);
    Draws(sel, LayerId::Projectile)[0].candidate.tileY = 3200;
    CheckEq("expand: no line when it does not travel", static_cast<long long>(
                ov::ExpandDraws(LayerId::Projectile, Draws(sel, LayerId::Projectile)).size()), 1);

    // An entity's position is not in its signature: the agent follows it.
    std::vector<DesiredDraw> npc = Wanted({ "dbg:npc:1" });
    SentMap npcSent;
    ov::CommitSends(npcSent, npc, { 0 }, 1000);
    npc[0].candidate.tileX += 3;
    CheckEq("entity move is no resend",
            static_cast<long long>(ov::DiffLayer(npcSent, npc, 1100, 1800).toSend.size()), 0);
}

void TestFastLabelsApart()
{
    ov::LayerConfig cfg = ov::DefaultLayerConfig();
    Selection sel[ov::kLayerCount];
    Draws(sel, LayerId::Npc).push_back(OnTile("dbg:npc:1", 1, 5, 3205, 3200, "Man"));
    sel[static_cast<int>(LayerId::Projectile)] = OneProjectile(3205);
    ov::ApplyLabels(cfg, sel);
    CheckStr("fast apart: npc caption alone", Draws(sel, LayerId::Npc)[0].label, "Man");
    CheckStr("fast apart: entity pass leaves projectiles",
             Draws(sel, LayerId::Projectile)[0].label, "");
    const ov::LabelTally t = ov::ApplyFastLabels(cfg, sel, 0);
    CheckEq("fast labels: wanted", t.wanted, 1);
    CheckEq("fast labels: none given at budget 0", t.given, 0);
    ov::ApplyFastLabels(cfg, sel, 1);
    CheckStr("fast labels: projectile captioned", Draws(sel, LayerId::Projectile)[0].label,
             "gfx 1234 #7 -> #11");
    CheckStr("fast labels: npc untouched", Draws(sel, LayerId::Npc)[0].label, "Man");
}

void TestStoreBudget()
{
    ov::LayerConfig cfg = ov::DefaultLayerConfig();
    cfg.layers[static_cast<int>(LayerId::Npc)].isEnabled = true;
    cfg.layers[static_cast<int>(LayerId::Loc)].isEnabled = true;
    Selection sel[ov::kLayerCount];
    for (int dist : { 1, 2, 9 })
    {
        Draws(sel, LayerId::Npc).push_back(OnTile("n", dist, dist, 3200 + dist, 3200, "Man"));
    }
    for (int dist : { 3, 8 })
    {
        Draws(sel, LayerId::Loc).push_back(OnTile("l", dist, dist, 3200 + dist, 3200, "Tree"));
    }
    Draws(sel, LayerId::Ground).push_back(OnTile("g", 1, 20, 3220, 3200, "Coins"));   // off
    CheckEq("store: farthest two dropped", ov::FitStoreBudget(cfg, false, sel, 3), 2);
    CheckEq("store: npc keeps two", static_cast<long long>(Draws(sel, LayerId::Npc).size()), 2);
    CheckEq("store: loc keeps one", static_cast<long long>(Draws(sel, LayerId::Loc).size()), 1);
    CheckEq("store: disabled layer untouched",
            static_cast<long long>(Draws(sel, LayerId::Ground).size()), 1);

    // A labelled, travelling projectile costs three commands.
    cfg.layers[static_cast<int>(LayerId::Projectile)].isEnabled = true;
    for (int i = 0; i < 3; ++i)
    {
        Draws(sel, LayerId::Projectile).push_back(OneProjectile(3201 + i).draws[0]);
        Draws(sel, LayerId::Projectile).back().candidate.distance = i + 1;
    }
    CheckEq("store: projectile command cost", ov::WireCommandCount(LayerId::Projectile,
            cfg.layers[static_cast<int>(LayerId::Projectile)], Draws(sel, LayerId::Projectile)[0]), 3);
    CheckEq("store: fast group trimmed apart", ov::FitStoreBudget(cfg, true, sel, 7), 1);
    CheckEq("store: two projectiles fit 7",
            static_cast<long long>(Draws(sel, LayerId::Projectile).size()), 2);
    CheckEq("store: entity layers untouched by the fast fit",
            static_cast<long long>(Draws(sel, LayerId::Npc).size()), 2);
}

void TestSpotAnims()
{
    std::vector<ov::SpotAnimSighting> seen;
    ov::RecordSpotAnim(seen, { 90, 3201, 3200, 0, 1000 });
    ov::RecordSpotAnim(seen, { 90, 3201, 3200, 0, 1500 });
    ov::RecordSpotAnim(seen, { 90, 3260, 3200, 0, 1500 });   // 60 tiles away
    CheckEq("spot: one per id and tile", static_cast<long long>(seen.size()), 2);
    CheckEq("spot: repeat refreshes its time", static_cast<long long>(seen[0].seenAtMs), 1500);
    auto s = InWorldSnapshot();
    std::vector<LayerCandidate> out;
    ov::CollectSpotAnims(seen, *s, 20, out);
    CheckEq("spot: distance gate", static_cast<long long>(out.size()), 1);
    CheckStr("spot: name", out[0].name, "spot 90");
    CheckStr("spot: key", ov::LayerKey(LayerId::SpotAnim, out[0]), "dbg:spot:90:3201:3200:0");
    ov::ExpireSpotAnims(seen, 3499, ov::kSpotAnimHoldMs);
    CheckEq("spot: held for the hold time", static_cast<long long>(seen.size()), 2);
    ov::ExpireSpotAnims(seen, 3500, ov::kSpotAnimHoldMs);
    CheckEq("spot: expired after it", static_cast<long long>(seen.size()), 0);
}

void TestFastConfig()
{
    CheckEq("fast ttl at 150 ms", ov::FastDrawTtlMs(150), 750);
    CheckEq("fast resend age at 150 ms", static_cast<long long>(ov::FastResendAgeMs(150)), 450);
    CheckEq("fast ttl at 1000 ms", ov::FastDrawTtlMs(1000), 5000);
    ov::LayerConfig c = ov::DefaultLayerConfig();
    CheckEq("defaults: projectiles off", c.layers[static_cast<int>(LayerId::Projectile)].isEnabled, 0);
    CheckEq("defaults: spot anims off", c.layers[static_cast<int>(LayerId::SpotAnim)].isEnabled, 0);
    CheckEq("defaults: projectile cap", c.layers[static_cast<int>(LayerId::Projectile)].maxCount, 50);
    CheckEq("defaults: spot anim cap", c.layers[static_cast<int>(LayerId::SpotAnim)].maxCount, 50);
    CheckEq("defaults: fast refresh", c.fastRefreshMs, 150);
    c.fastRefreshMs  = 1;
    c.tileLabelChars = 500;
    c.layers[static_cast<int>(LayerId::Projectile)].maxCount = 200;
    ov::SanitizeConfig(c);
    CheckEq("sanitize fast refresh floor", c.fastRefreshMs, ov::kMinFastRefreshMs);
    CheckEq("sanitize label chars ceiling", c.tileLabelChars, ov::kMaxTileLabelChars);
    CheckEq("sanitize fast layer count", c.layers[static_cast<int>(LayerId::Projectile)].maxCount,
            ov::kMaxFastLayerCount);
    c.tileLabelChars = 3;
    ov::SanitizeConfig(c);
    CheckEq("sanitize label chars floor", c.tileLabelChars, ov::kMinTileLabelChars);
    LayerId id = LayerId::Npc;
    CheckEq("layer alias projectile", ov::ParseLayerToken("projectile", id), 1);
    CheckEq("layer alias -> Projectile", static_cast<long long>(id),
            static_cast<long long>(LayerId::Projectile));
}

void TestEncodeWorldBatch()
{
    ov::LayerConfig cfg = ov::DefaultLayerConfig();
    Selection sel[ov::kLayerCount];
    sel[static_cast<int>(LayerId::Projectile)] = OneProjectile(3205);
    Draws(sel, LayerId::Projectile)[0].candidate.tileY = 3203;
    ov::ApplyFastLabels(cfg, sel, 10);
    const std::vector<DesiredDraw> want =
        ov::ExpandDraws(LayerId::Projectile, Draws(sel, LayerId::Projectile));
    std::vector<uint8_t> body;
    CheckEq("encode world batch",
            ov::EncodeLayerBatch(LayerId::Projectile, want, { 0, 1, 2 }, 750, body), 1);
    Reader   r(body.data(), body.size());
    uint32_t n = 0;
    r.ReadMapHeader(n);
    const char *k = nullptr;
    uint32_t    kn = 0;
    r.ReadString(k, kn);
    r.ReadArrayHeader(n);
    CheckEq("world batch: three items", n, 3);
    ItemView box, line, text;
    ReadItem(r, box);
    ReadItem(r, line);
    ReadItem(r, text);
    CheckStr("box kind", box.kind, "rect");
    CheckStr("box space", box.space, "world");
    CheckEq("box x: tile corner in sub-tiles", box.x, 3205LL * 256);
    CheckEq("box y: tile corner in sub-tiles", box.y, 3203LL * 256);
    CheckEq("box w: one tile", box.w, 256);
    CheckEq("box plane", box.plane, 0);
    CheckEq("box ttl", box.ttl, 750);
    CheckStr("box: no font (the agent refuses one on a rect)", box.font, "");
    CheckStr("line kind", line.kind, "line");
    CheckEq("line x1: launch tile centre", line.x1, 3200LL * 256 + 128);
    CheckEq("line y2: target tile centre", line.y2, 3203LL * 256 + 128);
    CheckStr("text kind", text.kind, "text");
    CheckStr("text key", text.key, "dbg:proj:1234:5000:7:11:t");
    CheckStr("text payload", text.text, "gfx 1234 #7 -> #11");
    CheckEq("text anchor: tile centre", text.x, 3205LL * 256 + 128);
    CheckStr("text: no label key", text.label, "");
}

int main()
{
    TestCollectNpcs();
    TestCollectLocs();
    TestCollectPlayersAndSelf();
    TestKeys();
    TestSelectCapAndOrder();
    TestSelectFilter();
    TestLabels();
    TestTileLabelGrouping();
    TestTileLabelBudget();
    TestTileLabelTruncation();
    TestTileLabelCarrierDiff();
    TestDiff();
    TestTtl();
    TestSanitize();
    TestEncodeLocBatch();
    TestEncodeClear();
    TestClassify();
    TestModeSwitch();
    TestCallBudget();
    TestCallBudgetSpreads();
    TestEntityReplies();
    TestEncodeHighlightEntity();
    TestLegacyLabelTokens();
    TestActorLabelFields();
    TestAnimatingOnly();
    TestTileLabelChars();
    TestCollectProjectiles();
    TestProjectileExpandAndDiff();
    TestFastLabelsApart();
    TestStoreBudget();
    TestSpotAnims();
    TestFastConfig();
    TestEncodeWorldBatch();
    std::printf("%d failure(s)\n", failures);
    return failures;
}
