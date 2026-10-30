// Layer tests: the pure half of the overlay layers (src/overlay/Layers.cpp) --
// snapshot collection, filter / distance / cap selection, the dbg: key scheme,
// the diff against what was sent, label budgeting, the batch encoding the
// agent reads and the reply classification.
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
using nxtdbg::overlay::LabelMode;
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
    CheckStr("label name", ov::FormatLabel(LabelMode::Name, LayerId::Ground, c), "Coins x250");
    CheckStr("label name+id+dist",
             ov::FormatLabel(LabelMode::NameIdDistance, LayerId::Ground, c), "Coins x250 #995 3t");
    CheckStr("label none", ov::FormatLabel(LabelMode::None, LayerId::Ground, c), "");

    ov::LayerConfig cfg = ov::DefaultLayerConfig();
    Selection sel[ov::kLayerCount];
    for (int i = 0; i < 3; ++i)
    {
        sel[static_cast<int>(LayerId::Loc)].draws.push_back(DesiredDraw{ "l" + std::to_string(i),
                                                            Cand(i, i, "Tree"), "", 0 });
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
    std::string kind, key, label;
    int64_t     loc = -1, x = -1, y = -1, plane = -1, w = -1, h = -1, ttl = -1;
    bool        isSelf = false;
};

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
            std::string *slot = key == "kind" ? &v.kind : key == "key" ? &v.key
                              : key == "label" ? &v.label : nullptr;
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
        int64_t *slot = key == "loc" ? &v.loc : key == "x" ? &v.x : key == "y" ? &v.y
                      : key == "plane" ? &v.plane : key == "w" ? &v.w : key == "h" ? &v.h
                      : key == "ttl_ms" ? &v.ttl : nullptr;
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
    TestDiff();
    TestTtl();
    TestSanitize();
    TestEncodeLocBatch();
    TestEncodeClear();
    TestClassify();
    std::printf("%d failure(s)\n", failures);
    return failures;
}
