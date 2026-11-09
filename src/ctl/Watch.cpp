// Watches, the change log and the decoded event log.
//
// Two feeds land in the change log. The event ring carries every varp / varbit
// / varc change the agent observes, so short-lived values are caught even when
// they revert between polls. Polling covers what the ring does not (varc
// strings, interface component text / visibility) and re-baselines the rest.
// A change is only logged when the new value differs from the last one this
// log recorded, so the two feeds never double-report one transition.
#include "ctl/Commands.h"
#include "ctl/Control.h"

#include "app/App.h"
#include "ipc/Events.h"
#include "wire/EventFormat.h"
#include "wire/EventReader.h"
#include "wire/SnapshotReader.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace nxtdbg::ctl
{

namespace
{

constexpr int64_t  kDefaultReadMax = 500;
constexpr int64_t  kMaxReadMax     = 5000;
constexpr uint32_t kMaxOpenIfaces  = 64;   // Snapshot::openIfaces capacity

constexpr const char *kDiffFields[] = {
    "text", "hidden", "type", "category", "sprite_id", "item_id", "item_amount", "triggers",
};
constexpr const char *kGeometryFields[] = { "x", "y", "w", "h" };

using CompKey = std::pair<int32_t, int32_t>;

// Common header for every change / event body: wall time since the debugger
// started and the server tick it was observed on (null when not attached).
JsonValue Stamp(app::App &a, const char *kind)
{
    JsonValue o = JObj();
    Put(o, "t_ms", JInt(static_cast<int64_t>(GetTickCount64() - a.ctl.startTick)));
    const auto *snap = wire::CurrentSnapshot(a.session);
    Put(o, "tick", snap ? JInt(snap->serverTick) : JNull());
    Put(o, "kind", JStr(kind));
    return o;
}

void LogIntChange(app::App &a, const char *kind, int32_t id, int64_t oldValue,
                  int64_t newValue, const char *source)
{
    JsonValue c = Stamp(a, kind);
    Put(c, "id", JInt(id));
    Put(c, "old", JInt(oldValue));
    Put(c, "new", JInt(newValue));
    Put(c, "source", JStr(source));
    a.ctl.changes.Add(std::move(c));
}

// Poll result for one int watch. The first sighting is a silent baseline.
void ApplyPolled(app::App &a, IntWatch &w, int64_t v, const char *kind)
{
    if (w.isKnown && w.value != v)
    {
        LogIntChange(a, kind, w.id, w.value, v, "poll");
    }
    w.value   = v;
    w.isKnown = true;
}

// Ring event for one int watch: logged with the event's own old/new, unless a
// poll already recorded this exact value.
void ApplyEvent(app::App &a, IntWatch &w, int64_t oldValue, int64_t newValue, const char *kind)
{
    if (w.isKnown && w.value == newValue)
    {
        return;
    }
    LogIntChange(a, kind, w.id, oldValue, newValue, "event");
    w.value   = newValue;
    w.isKnown = true;
}

void PollInts(app::App &a, std::vector<IntWatch> &ws, const char *method, const char *kind)
{
    if (ws.empty())
    {
        return;
    }
    std::vector<int32_t> ids;
    for (const IntWatch &w : ws)
    {
        ids.push_back(w.id);
    }
    std::vector<int64_t> values;
    std::vector<bool>    found;
    std::string          err;
    if (!ReadIntVars(a, method, ids, values, found, err))
    {
        return;
    }
    for (size_t i = 0; i < ws.size(); ++i)
    {
        ApplyPolled(a, ws[i], values[i], kind);
    }
}

void PollVarbits(app::App &a)
{
    std::vector<IntWatch> &ws = a.ctl.watches.varbits;
    if (ws.empty())
    {
        return;
    }
    std::vector<int32_t> ids;
    for (const IntWatch &w : ws)
    {
        ids.push_back(w.id);
    }
    std::vector<int64_t>     values;
    std::vector<std::string> source;
    std::string              err;
    if (!ReadVarbits(a, ids, values, source, err))
    {
        return;
    }
    for (size_t i = 0; i < ws.size(); ++i)
    {
        if (source[i] == "varp")
        {
            ApplyPolled(a, ws[i], values[i], "varbit");
        }
    }
}

void PollStrings(app::App &a)
{
    std::vector<StrWatch> &ws = a.ctl.watches.varcstrs;
    if (ws.empty())
    {
        return;
    }
    std::vector<int32_t> ids;
    for (const StrWatch &w : ws)
    {
        ids.push_back(w.id);
    }
    std::vector<std::string> values;
    std::vector<bool>        found;
    std::string              err;
    if (!ReadStrVars(a, ids, values, found, err))
    {
        return;
    }
    for (size_t i = 0; i < ws.size(); ++i)
    {
        StrWatch &w = ws[i];
        if (w.isKnown && w.value != values[i])
        {
            JsonValue c = Stamp(a, "varcstr");
            Put(c, "id", JInt(w.id));
            Put(c, "old", JStr(w.value));
            Put(c, "new", JStr(values[i]));
            Put(c, "source", JStr("poll"));
            a.ctl.changes.Add(std::move(c));
        }
        w.value   = values[i];
        w.isKnown = true;
    }
}

JsonValue DiffedFields(const JsonValue &node, bool isGeometryDiffed)
{
    JsonValue f = JObj();
    for (const char *k : kDiffFields)
    {
        if (const JsonValue *v = node.Find(k))
        {
            Put(f, k, *v);
        }
    }
    if (isGeometryDiffed)
    {
        for (const char *k : kGeometryFields)
        {
            if (const JsonValue *v = node.Find(k))
            {
                Put(f, k, *v);
            }
        }
    }
    return f;
}

JsonValue CompChange(app::App &a, const char *kind, int32_t iface, const CompKey &key)
{
    JsonValue c = Stamp(a, kind);
    Put(c, "iface", JInt(iface));
    Put(c, "comp", JInt(key.first));
    Put(c, "sub", JInt(key.second));
    Put(c, "gameval", JStr(a.gameval.ComponentName(iface, key.first)));
    return c;
}

void DiffOneComp(app::App &a, int32_t iface, const CompKey &key, const JsonValue &before,
                 const JsonValue &after)
{
    for (const auto &kv : after.objVal)
    {
        const JsonValue *old = before.Find(kv.first);
        if (old && ToJson(*old) == ToJson(kv.second))
        {
            continue;
        }
        JsonValue c = CompChange(a, "comp_changed", iface, key);
        Put(c, "field", JStr(kv.first));
        Put(c, "old", old ? *old : JNull());
        Put(c, "new", kv.second);
        a.ctl.changes.Add(std::move(c));
    }
}

void DiffTree(app::App &a, IfaceWatch &w, std::map<CompKey, JsonValue> &now)
{
    for (auto &kv : now)
    {
        auto it = w.comps.find(kv.first);
        if (it == w.comps.end())
        {
            JsonValue c = CompChange(a, "comp_added", w.iface, kv.first);
            Put(c, "fields", kv.second);
            a.ctl.changes.Add(std::move(c));
            continue;
        }
        DiffOneComp(a, w.iface, kv.first, it->second, kv.second);
    }
    for (const auto &kv : w.comps)
    {
        if (now.find(kv.first) == now.end())
        {
            a.ctl.changes.Add(CompChange(a, "comp_removed", w.iface, kv.first));
        }
    }
}

// An empty tree (interface closed) is reported once as iface_tree/empty and
// drops the baseline, rather than as a comp_removed per component; the next
// non-empty tree is reported as iface_tree/populated and re-baselines silently.
void PollIface(app::App &a, IfaceWatch &w)
{
    JsonValue   nodes;
    std::string err;
    if (!FetchTree(a, w.iface, 0, nodes, err))
    {
        return;
    }
    std::map<CompKey, JsonValue> now;
    for (const JsonValue &n : nodes.arrVal)
    {
        const JsonValue *comp = n.Find("comp");
        const JsonValue *sub  = n.Find("sub");
        CompKey          key{ comp ? int32_t(comp->AsInt()) : -1, sub ? int32_t(sub->AsInt()) : -1 };
        now.emplace(key, DiffedFields(n, a.ctl.watches.isGeometryDiffed));
    }
    const bool wasPopulated = w.isKnown && !w.comps.empty();
    if (w.isKnown && wasPopulated != !now.empty())
    {
        JsonValue c = Stamp(a, "iface_tree");
        Put(c, "iface", JInt(w.iface));
        Put(c, "state", JStr(now.empty() ? "empty" : "populated"));
        Put(c, "count", JInt(static_cast<int64_t>(now.size())));
        a.ctl.changes.Add(std::move(c));
    }
    else if (w.isKnown && wasPopulated)
    {
        DiffTree(a, w, now);
    }
    w.comps   = std::move(now);
    w.isKnown = true;
}

void ResetBaselines(WatchSet &ws)
{
    for (auto *list : { &ws.varps, &ws.varbits, &ws.varcs })
    {
        for (IntWatch &w : *list)
        {
            w.isKnown = false;
        }
    }
    for (StrWatch &w : ws.varcstrs)
    {
        w.isKnown = false;
    }
    for (IfaceWatch &w : ws.ifaces)
    {
        w.isKnown = false;
        w.comps.clear();
    }
}

void TrackOpenIfaces(app::App &a, const nxt::ipc::Snapshot &snap)
{
    Tracker       &tr    = a.ctl.tracker;
    const uint32_t count = std::min<uint32_t>(snap.openIfaceCount, kMaxOpenIfaces);
    std::vector<int32_t> cur(snap.openIfaces, snap.openIfaces + count);
    if (tr.hasOpen)
    {
        auto logIface = [&](const char *kind, int32_t id) {
            JsonValue c = Stamp(a, kind);
            Put(c, "iface", JInt(id));
            Put(c, "name", JStr(a.gameval.NameForCacheType("if", id)));
            a.ctl.changes.Add(std::move(c));
        };
        for (int32_t id : cur)
        {
            if (std::find(tr.lastOpen.begin(), tr.lastOpen.end(), id) == tr.lastOpen.end())
            {
                logIface("iface_open", id);
            }
        }
        for (int32_t id : tr.lastOpen)
        {
            if (std::find(cur.begin(), cur.end(), id) == cur.end())
            {
                logIface("iface_close", id);
            }
        }
    }
    tr.lastOpen = std::move(cur);
    tr.hasOpen  = true;
}

// --- event decoding ----------------------------------------------------------

template <typename T>
const T *Body(const wire::EventRecord &r)
{
    return r.bodyLen >= sizeof(T) ? reinterpret_cast<const T *>(r.body) : nullptr;
}

void DecodeVarLike(const wire::EventRecord &r, JsonValue &o)
{
    using namespace nxt::ipc;
    switch (r.type)
    {
        case kEventVarChange:
        case kEventVarbitChange:
        case kEventVarcChange:
            if (const auto *b = Body<VarChangeBody>(r))
            {
                Put(o, "id", JInt(b->varpId));
                Put(o, "old", JInt(b->oldValue));
                Put(o, "new", JInt(b->newValue));
            }
            return;
        case kEventObjVarChange:
            if (const auto *b = Body<ObjVarChangeBody>(r))
            {
                Put(o, "inv", JInt(b->invId));
                Put(o, "slot", JInt(b->slot));
                Put(o, "var", JInt(b->varId));
                Put(o, "old", JInt(b->oldValue));
                Put(o, "new", JInt(b->newValue));
            }
            return;
        default:
            return;
    }
}

void DecodeGameplay(const wire::EventRecord &r, JsonValue &o)
{
    using namespace nxt::ipc;
    if (r.type == kEventActionExecuted)
    {
        if (const auto *b = Body<ActionExecutedBody>(r))
        {
            Put(o, "action_id", JInt(b->actionId));
            Put(o, "param1", JInt(b->param1));
            Put(o, "param2", JInt(b->param2));
            Put(o, "param3", JInt(b->param3));
        }
    }
    else if (r.type == kEventChatMessage)
    {
        const auto *b = Body<ChatMessageBody>(r);
        if (b && size_t(b->senderLen) + b->textLen <= sizeof(b->buf))
        {
            const char *buf = reinterpret_cast<const char *>(b->buf);
            Put(o, "msg_type", JInt(b->msgType));
            Put(o, "sender", JStr(std::string(buf, b->senderLen)));
            Put(o, "message", JStr(std::string(buf + b->senderLen, b->textLen)));
        }
    }
    else if (r.type == kEventRadioGroupSelect)
    {
        if (const auto *b = Body<RadioGroupSelectBody>(r))
        {
            Put(o, "iface", JInt(b->ifaceId));
            Put(o, "comp", JInt(b->componentId));
            Put(o, "sub", JInt(b->subId));
            Put(o, "value", JInt(b->value));
            Put(o, "opcode", JInt(b->opcode));
        }
    }
}

void DecodeState(const wire::EventRecord &r, JsonValue &o)
{
    using namespace nxt::ipc;
    if (r.type == kEventLoginStateChange)
    {
        if (const auto *b = Body<LoginStateChangeBody>(r))
        {
            Put(o, "old", JInt(b->oldState));
            Put(o, "new", JInt(b->newState));
        }
    }
    else if (r.type == kEventTick)
    {
        if (const auto *b = Body<TickBody>(r))
        {
            Put(o, "server_tick", JInt(b->tick));
        }
    }
    else if (r.type == kEventKeyInput)
    {
        if (const auto *b = Body<KeyInputBody>(r))
        {
            Put(o, "vk", JInt(b->key));
            Put(o, "alt", JBool(b->isAlt != 0));
            Put(o, "ctrl", JBool(b->isCtrl != 0));
            Put(o, "shift", JBool(b->isShift != 0));
        }
    }
}

// Structured fields for the common types; every event also carries `text`,
// the exact line the Events panel shows, so no type is ever opaque.
JsonValue DecodeEvent(app::App &a, const wire::EventRecord &r)
{
    JsonValue o = Stamp(a, "event");
    Put(o, "ring_seq", JInt(static_cast<int64_t>(r.seq)));
    Put(o, "type", JStr(wire::EventTypeName(r.type)));
    Put(o, "type_id", JInt(static_cast<int64_t>(r.type)));
    char text[200];
    wire::FormatEventBody(r, text, sizeof(text));
    Put(o, "text", JStr(text));
    DecodeVarLike(r, o);
    DecodeGameplay(r, o);
    DecodeState(r, o);
    return o;
}

void RouteVarEvent(app::App &a, const wire::EventRecord &r)
{
    using namespace nxt::ipc;
    const auto *b = Body<VarChangeBody>(r);
    if (!b)
    {
        return;
    }
    std::vector<IntWatch> *list = nullptr;
    const char            *kind = nullptr;
    switch (r.type)
    {
        case kEventVarChange:    list = &a.ctl.watches.varps;   kind = "varp";   break;
        case kEventVarbitChange: list = &a.ctl.watches.varbits; kind = "varbit"; break;
        case kEventVarcChange:   list = &a.ctl.watches.varcs;   kind = "varc";   break;
        default:                 return;
    }
    if (r.type == kEventVarbitChange)
    {
        a.ctl.varbitSeen[b->varpId] = b->newValue;
    }
    for (IntWatch &w : *list)
    {
        if (w.id == b->varpId)
        {
            ApplyEvent(a, w, b->oldValue, b->newValue, kind);
        }
    }
}

// --- ring reads ----------------------------------------------------------------

bool TypeWanted(const JsonValue &entry, const std::vector<std::string> &types)
{
    if (types.empty())
    {
        return true;
    }
    const JsonValue *t = entry.Find("type");
    if (!t)
    {
        t = entry.Find("kind");
    }
    const std::string name = t ? t->AsString() : std::string();
    return std::find(types.begin(), types.end(), name) != types.end();
}

// {head, oldest, truncated, next, <itemsKey>: [...]}. Entries with seq > since,
// oldest first, at most `max`. `next` is the cursor to pass as the next since.
void ReadRing(const LogRing &ring, const JsonValue &args, const char *itemsKey,
              JsonValue &outResult)
{
    int64_t since = 0;
    int64_t max   = kDefaultReadMax;
    ArgInt(args, "since", since);
    ArgInt(args, "max", max);
    max = std::clamp<int64_t>(max, 0, kMaxReadMax);
    std::vector<std::string> types;
    if (const JsonValue *t = args.Find("types"); t && t->IsArray())
    {
        for (const JsonValue &e : t->arrVal)
        {
            types.push_back(e.AsString());
        }
    }
    const uint64_t oldest = ring.entries.empty() ? ring.nextSeq : ring.entries.front().seq;
    JsonValue      items  = JArr();
    uint64_t       next   = static_cast<uint64_t>(std::max<int64_t>(since, 0));
    for (const LogRing::Entry &e : ring.entries)
    {
        if (e.seq <= next)
        {
            continue;
        }
        if (static_cast<int64_t>(items.arrVal.size()) >= max)
        {
            break;
        }
        next = e.seq;
        if (!TypeWanted(e.body, types))
        {
            continue;
        }
        JsonValue o = JObj();
        Put(o, "seq", JInt(static_cast<int64_t>(e.seq)));
        o.objVal.insert(o.objVal.end(), e.body.objVal.begin(), e.body.objVal.end());
        Push(items, std::move(o));
    }
    Put(outResult, "head", JInt(static_cast<int64_t>(ring.Head())));
    Put(outResult, "oldest", JInt(static_cast<int64_t>(oldest)));
    Put(outResult, "truncated", JBool(since > 0 && static_cast<uint64_t>(since) + 1 < oldest));
    Put(outResult, "next", JInt(static_cast<int64_t>(next)));
    Put(outResult, itemsKey, std::move(items));
}

// --- watch set editing ----------------------------------------------------------

template <typename W>
void AddIds(std::vector<W> &list, const std::vector<int32_t> &ids)
{
    for (int32_t id : ids)
    {
        auto same = [id](const W &w) { return w.id == id; };
        if (std::find_if(list.begin(), list.end(), same) == list.end())
        {
            W w;
            w.id = id;
            list.push_back(std::move(w));
        }
    }
}

template <typename W>
void RemoveIds(std::vector<W> &list, const std::vector<int32_t> &ids)
{
    auto doomed = [&ids](const W &w) {
        return std::find(ids.begin(), ids.end(), w.id) != ids.end();
    };
    list.erase(std::remove_if(list.begin(), list.end(), doomed), list.end());
}

void AddIfaces(std::vector<IfaceWatch> &list, const std::vector<int32_t> &ids)
{
    for (int32_t id : ids)
    {
        auto same = [id](const IfaceWatch &w) { return w.iface == id; };
        if (std::find_if(list.begin(), list.end(), same) == list.end())
        {
            IfaceWatch w;
            w.iface = id;
            list.push_back(std::move(w));
        }
    }
}

struct WatchArgs
{
    std::vector<int32_t> varp;
    std::vector<int32_t> varbit;
    std::vector<int32_t> varc;
    std::vector<int32_t> varcstr;
    std::vector<int32_t> iface;
};

bool ParseWatchArgs(const JsonValue &args, WatchArgs &out, std::string &outError)
{
    return ArgIdList(args, "varp", out.varp, outError)
        && ArgIdList(args, "varbit", out.varbit, outError)
        && ArgIdList(args, "varc", out.varc, outError)
        && ArgIdList(args, "varcstr", out.varcstr, outError)
        && ArgIdList(args, "iface", out.iface, outError);
}

template <typename W>
JsonValue IntWatchList(const std::vector<W> &list)
{
    JsonValue arr = JArr();
    for (const W &w : list)
    {
        JsonValue o = JObj();
        Put(o, "id", JInt(w.id));
        Put(o, "value", w.isKnown ? JInt(w.value) : JNull());
        Push(arr, std::move(o));
    }
    return arr;
}

JsonValue DescribeWatches(app::App &a)
{
    const WatchSet &ws = a.ctl.watches;
    JsonValue       o  = JObj();
    Put(o, "varp", IntWatchList(ws.varps));
    Put(o, "varbit", IntWatchList(ws.varbits));
    Put(o, "varc", IntWatchList(ws.varcs));
    JsonValue strs = JArr();
    for (const StrWatch &w : ws.varcstrs)
    {
        JsonValue e = JObj();
        Put(e, "id", JInt(w.id));
        Put(e, "value", w.isKnown ? JStr(w.value) : JNull());
        Push(strs, std::move(e));
    }
    Put(o, "varcstr", std::move(strs));
    JsonValue ifaces = JArr();
    for (const IfaceWatch &w : ws.ifaces)
    {
        JsonValue e = JObj();
        Put(e, "iface", JInt(w.iface));
        Put(e, "components", w.isKnown ? JInt(static_cast<int64_t>(w.comps.size())) : JNull());
        Push(ifaces, std::move(e));
    }
    Put(o, "iface", std::move(ifaces));
    Put(o, "var_interval_ms", JInt(ws.varIntervalMs));
    Put(o, "iface_interval_ms", JInt(ws.ifaceIntervalMs));
    Put(o, "geometry", JBool(ws.isGeometryDiffed));
    return o;
}

void ApplyIntervals(WatchSet &ws, const JsonValue &args)
{
    int64_t v = 0;
    if (ArgInt(args, "var_interval_ms", v))
    {
        ws.varIntervalMs = static_cast<uint32_t>(std::clamp<int64_t>(v, 0, 60000));
    }
    if (ArgInt(args, "iface_interval_ms", v))
    {
        ws.ifaceIntervalMs = static_cast<uint32_t>(std::clamp<int64_t>(v, 0, 60000));
    }
    ws.isGeometryDiffed = ArgBool(args, "geometry", ws.isGeometryDiffed);
}

}   // namespace

void LogRing::Add(JsonValue body)
{
    entries.push_back({ nextSeq++, std::move(body) });
    while (entries.size() > capacity)
    {
        entries.pop_front();
    }
}

void TrackAlways(app::App &a)
{
    Tracker    &tr  = a.ctl.tracker;
    const DWORD pid = a.session.IsOpen() ? a.session.Pid() : 0;
    if (pid != tr.lastPid)
    {
        JsonValue c = Stamp(a, pid ? "attach" : "detach");
        Put(c, "pid", JInt(pid));
        Put(c, "old_pid", JInt(tr.lastPid));
        a.ctl.changes.Add(std::move(c));
        tr.lastPid       = pid;
        tr.hasOpen       = false;
        tr.lastGameState = INT32_MIN;
        ResetBaselines(a.ctl.watches);
        a.ctl.varbitSeen.clear();
    }
    const nxt::ipc::Snapshot *snap = wire::CurrentSnapshot(a.session);
    if (!snap)
    {
        return;
    }
    const int32_t gameState = snap->gameState;
    if (tr.lastGameState != INT32_MIN && gameState != tr.lastGameState)
    {
        JsonValue c = Stamp(a, "game_state");
        Put(c, "old", JInt(tr.lastGameState));
        Put(c, "new", JInt(gameState));
        a.ctl.changes.Add(std::move(c));
    }
    tr.lastGameState = gameState;
    TrackOpenIfaces(a, *snap);
}

void PollWatches(app::App &a)
{
    if (!a.session.IsOpen() || !a.rpc.IsConnected())
    {
        return;
    }
    WatchSet       &ws  = a.ctl.watches;
    const ULONGLONG now = GetTickCount64();
    const bool hasVars = !ws.varps.empty() || !ws.varbits.empty() || !ws.varcs.empty()
                      || !ws.varcstrs.empty();
    if (hasVars && now - ws.lastVarPoll >= ws.varIntervalMs)
    {
        ws.lastVarPoll = now;
        PollInts(a, ws.varps, "get_varps", "varp");
        PollInts(a, ws.varcs, "get_varcs_int", "varc");
        PollVarbits(a);
        PollStrings(a);
    }
    if (!ws.ifaces.empty() && now - ws.lastIfacePoll >= ws.ifaceIntervalMs)
    {
        ws.lastIfacePoll = now;
        for (IfaceWatch &w : ws.ifaces)
        {
            PollIface(a, w);
        }
    }
}

void RecordEvents(app::App &a, const std::vector<wire::EventRecord> &fresh)
{
    for (const wire::EventRecord &r : fresh)
    {
        a.ctl.events.Add(DecodeEvent(a, r));
        RouteVarEvent(a, r);
    }
}

// watch {varp, varbit, varc, varcstr, iface, var_interval_ms, iface_interval_ms,
//        geometry, replace} -> {watching, cursor}
bool CmdWatch(app::App &a, const JsonValue &args, JsonValue &outResult, std::string &outError)
{
    WatchArgs w;
    if (!ParseWatchArgs(args, w, outError))
    {
        return false;
    }
    WatchSet &ws = a.ctl.watches;
    if (ArgBool(args, "replace", false))
    {
        ws = WatchSet{};
    }
    AddIds(ws.varps, w.varp);
    AddIds(ws.varbits, w.varbit);
    AddIds(ws.varcs, w.varc);
    AddIds(ws.varcstrs, w.varcstr);
    AddIfaces(ws.ifaces, w.iface);
    ApplyIntervals(ws, args);
    // Baseline the new items now (silently), so the reply carries values and
    // the very next change is diffed against them rather than swallowed.
    ws.lastVarPoll   = 0;
    ws.lastIfacePoll = 0;
    PollWatches(a);
    Put(outResult, "watching", DescribeWatches(a));
    Put(outResult, "cursor", JInt(static_cast<int64_t>(a.ctl.changes.Head())));
    return true;
}

// unwatch {varp, varbit, varc, varcstr, iface} or {all: true}
bool CmdUnwatch(app::App &a, const JsonValue &args, JsonValue &outResult, std::string &outError)
{
    WatchSet &ws = a.ctl.watches;
    if (ArgBool(args, "all", false))
    {
        ws = WatchSet{};
    }
    else
    {
        WatchArgs w;
        if (!ParseWatchArgs(args, w, outError))
        {
            return false;
        }
        RemoveIds(ws.varps, w.varp);
        RemoveIds(ws.varbits, w.varbit);
        RemoveIds(ws.varcs, w.varc);
        RemoveIds(ws.varcstrs, w.varcstr);
        auto doomed = [&w](const IfaceWatch &iw) {
            return std::find(w.iface.begin(), w.iface.end(), iw.iface) != w.iface.end();
        };
        ws.ifaces.erase(std::remove_if(ws.ifaces.begin(), ws.ifaces.end(), doomed),
                        ws.ifaces.end());
    }
    Put(outResult, "watching", DescribeWatches(a));
    return true;
}

// changes {since, max, types?} -- `types` filters on kind (varp, comp_changed, ...).
bool CmdChanges(app::App &a, const JsonValue &args, JsonValue &outResult, std::string &)
{
    ReadRing(a.ctl.changes, args, "changes", outResult);
    return true;
}

// events {since, max, types?} -- `types` filters on type name (var, chat, action, ...).
bool CmdEvents(app::App &a, const JsonValue &args, JsonValue &outResult, std::string &)
{
    ReadRing(a.ctl.events, args, "events", outResult);
    Put(outResult, "drops", JInt(static_cast<int64_t>(a.events.DropCount())));
    Put(outResult, "paused", JBool(a.paused));
    return true;
}

}
