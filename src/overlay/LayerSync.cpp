#include "LayerSync.h"

#include "app/App.h"
#include "app/EntityName.h"
#include "cache/Json.h"
#include "ctl/Commands.h"
#include "ipc/Events.h"
#include "wire/SnapshotReader.h"

#include <Windows.h>

#include <cstring>
#include <vector>

namespace nxtdbg::overlay
{

namespace
{

constexpr DWORD kLayerCallTimeoutMs = 1500;

int Idx(LayerId id)
{
    return static_cast<int>(id);
}

LayerId IdAt(int i)
{
    return static_cast<LayerId>(i);
}

// --- cache-backed lookups ----------------------------------------------------

const char *CacheType(NameKind kind)
{
    switch (kind)
    {
    case NameKind::Npc:  return "npc";
    case NameKind::Loc:  return "loc";
    case NameKind::Item: return "item";
    }
    return "item";
}

std::string LookupName(void *ctx, NameKind kind, int32_t id)
{
    app::App      &a   = *static_cast<app::App *>(ctx);
    const int64_t  key = (static_cast<int64_t>(kind) << 32) | static_cast<uint32_t>(id);
    auto          &memo = a.layers.names;
    const auto     it  = memo.find(key);
    if (it != memo.end())
    {
        return it->second;
    }
    std::string name = app::EntityName(a, CacheType(kind), id);
    memo.emplace(key, name);
    return name;
}

// sizeX / sizeY off the cache loc definition (NXTCacheLibrary's loc JSON).
// Not memoised while the cache is closed, so the real size lands once it opens.
bool LookupLocSize(void *ctx, int32_t locId, int32_t &outX, int32_t &outY)
{
    app::App  &a    = *static_cast<app::App *>(ctx);
    auto      &memo = a.layers.locSizes;
    const auto it   = memo.find(locId);
    if (it != memo.end())
    {
        outX = it->second.first;
        outY = it->second.second;
        return true;
    }
    if (!a.cache.IsOpen())
    {
        return false;
    }
    cache::JsonValue def;
    const std::string json = a.cache.GetJson("loc", locId);
    const bool isParsed = !json.empty() && cache::ParseJson(json, def);
    const cache::JsonValue *sx = isParsed ? def.Find("sizeX") : nullptr;
    const cache::JsonValue *sy = isParsed ? def.Find("sizeY") : nullptr;
    outX = sx != nullptr ? static_cast<int32_t>(sx->AsInt(1)) : 1;
    outY = sy != nullptr ? static_cast<int32_t>(sy->AsInt(1)) : 1;
    memo.emplace(locId, std::make_pair(outX, outY));
    return true;
}

// A name memoised while the cache was closed is the gameval fallback; drop the
// memo when the cache opens or closes so labels pick up the real names.
void TrackCache(app::App &a)
{
    LayerSyncState &st = a.layers;
    if (st.config.layers[Idx(LayerId::Loc)].isEnabled)
    {
        ctl::EnsureCache(a);   // throttled to one try per 30 s
    }
    const bool isOpen = a.cache.IsOpen();
    if (isOpen != st.wasCacheOpen)
    {
        st.names.clear();
        st.locSizes.clear();
        st.wasCacheOpen    = isOpen;
        st.isRefreshForced = true;
    }
}

// --- rpc ---------------------------------------------------------------------

uint32_t TtlFor(const LayerConfig &c, LayerId id)
{
    return IsFastLayer(id) ? FastDrawTtlMs(c.fastRefreshMs) : DrawTtlMs(c.refreshMs);
}

uint64_t ResendAgeFor(const LayerConfig &c, LayerId id)
{
    return IsFastLayer(id) ? FastResendAgeMs(c.fastRefreshMs) : ResendAgeMs(c.refreshMs);
}

rpc::CallStatus Call(app::App &a, const char *method, const std::vector<uint8_t> &body,
                     std::vector<uint8_t> &outReply)
{
    ++a.layers.rpcCount;
    return a.rpc.Call(method, body.data(), static_cast<uint32_t>(body.size()), outReply,
                      kLayerCallTimeoutMs);
}

// One debug_draw_clear for `keys`. True when the agent answered (the keys are
// gone or were never there); false leaves them for the next refresh.
bool ClearKeys(app::App &a, const std::vector<std::string> &keys)
{
    if (keys.empty())
    {
        return true;
    }
    std::vector<uint8_t> body;
    std::vector<uint8_t> reply;
    return EncodeClearKeys(keys, body)
        && Call(a, "debug_draw_clear", body, reply) == rpc::CallStatus::Ok;
}

void ClearLayer(app::App &a, LayerRuntime &rt)
{
    std::vector<std::string> keys;
    keys.reserve(rt.sent.size());
    for (const auto &kv : rt.sent)
    {
        keys.push_back(kv.first);
    }
    if (ClearKeys(a, keys))
    {
        rt.sent.clear();
    }
}

void ApplyBatchResult(LayerRuntime &rt, const Selection &sel, const LayerDiff &diff,
                      const BatchResult &res, uint64_t nowMs)
{
    rt.lastDropped = res.dropped;
    rt.isWarning   = false;
    switch (res.outcome)
    {
    case BatchOutcome::Applied:
        CommitSends(rt.sent, sel.draws, diff.toSend, nowMs);
        rt.message.clear();
        break;
    case BatchOutcome::Partial:
        // The reply does not say WHICH items were refused, so nothing in the
        // map can be trusted to be on screen: record the keys (so they are
        // cleared later) and have the next refresh send them all again.
        CommitSends(rt.sent, sel.draws, diff.toSend, nowMs);
        MarkAllStale(rt.sent);
        rt.message   = "agent dropped " + std::to_string(res.dropped) + ": " + res.error;
        rt.isWarning = true;
        break;
    case BatchOutcome::Rejected:
        rt.isRejected = true;
        rt.message    = res.error.empty() ? std::string("agent refused every entry")
                                          : res.error;
        break;
    case BatchOutcome::Transport:
        rt.message = res.error;   // nothing committed; retried next refresh
        break;
    }
}

// One debug_draw_set_batch for the layer's adds / changes. False when the
// agent refused entity items as an unknown kind: the session is per-entity
// from now on and this layer's draws go out that way instead.
bool SendBatch(app::App &a, LayerId id, const Selection &sel, const LayerDiff &diff,
               uint64_t nowMs)
{
    LayerSyncState      &st = a.layers;
    LayerRuntime        &rt = st.runtime[Idx(id)];
    std::vector<uint8_t> body;
    std::vector<uint8_t> reply;
    if (!EncodeLayerBatch(id, sel.draws, diff.toSend, TtlFor(st.config, id), body))
    {
        rt.message = "batch too large to encode";
        return true;
    }
    const rpc::CallStatus status = Call(a, "debug_draw_set_batch", body, reply);
    const BatchResult     res    = ClassifyBatchReply(status, reply,
                                                      static_cast<uint32_t>(diff.toSend.size()));
    // Fast layers send geometry kinds, which say nothing about entity items.
    if (!IsFastLayer(id))
    {
        st.agentMode = ModeAfterBatch(st.agentMode, res);
        if (st.agentMode == AgentEntityMode::PerEntity)
        {
            return false;
        }
    }
    ApplyBatchResult(rt, sel, diff, res, nowMs);
    return true;
}

// --- per-entity (agents without entity batch items) ----------------------------

struct EntityRun
{
    uint32_t          calls   = 0;
    uint32_t          applied = 0;
    uint32_t          refused = 0;   // npc / player / self entries the agent would not draw
    uint32_t          waiting = 0;   // over the refresh's call budget: next refresh
    EntityCallOutcome stop    = EntityCallOutcome::Applied;   // Applied: ran to the end
    std::string       error;         // the first failure's agent / transport text
};

EntityCallResult SendEntity(app::App &a, LayerId id, const DesiredDraw &d, uint32_t ttlMs)
{
    std::vector<uint8_t> body;
    std::vector<uint8_t> reply;
    if (!EncodeHighlightEntity(id, d, ttlMs, body))
    {
        return EntityCallResult{ EntityCallOutcome::Refused, "highlight_entity params too large" };
    }
    return ClassifyEntityReply(Call(a, "highlight_entity", body, reply), reply);
}

// Refused loc / ground entries latch the layer (this agent has no such
// selectors); capacity and transport stop it until the next refresh.
void ApplyEntityRun(LayerRuntime &rt, LayerId id, const EntityRun &run)
{
    rt.lastDropped = run.refused;
    rt.isWarning   = false;
    rt.message.clear();
    const bool isAllRefused = run.refused > 0 && run.refused == run.calls;
    if (run.stop == EntityCallOutcome::Refused || isAllRefused)
    {
        rt.isRejected = true;
        rt.message    = RefusalMessage(id, AgentEntityMode::PerEntity, run.error);
        return;
    }
    if (run.stop == EntityCallOutcome::Transport)
    {
        rt.message = run.error;
        return;
    }
    rt.isWarning = true;
    if (run.stop == EntityCallOutcome::Capacity)
    {
        rt.message = "agent store full, retrying next refresh: " + run.error;
    }
    else if (run.refused > 0)
    {
        rt.message = "agent dropped " + std::to_string(run.refused) + ": " + run.error;
    }
    else if (run.waiting > 0)
    {
        rt.message = std::to_string(run.waiting) + " more go out next refresh (legacy agent: "
                   + std::to_string(kMaxEntityCallsPerRefresh) + " calls per refresh)";
    }
    else
    {
        rt.isWarning = false;
    }
}

// One highlight_entity per draw, nearest first, within the refresh's shared
// call budget. Each applied call is committed on its own, so a stop part-way
// leaves the sent map true and the rest goes out next refresh.
void SendPerEntity(app::App &a, LayerId id, const Selection &sel, const LayerDiff &diff,
                   uint64_t nowMs, uint32_t &ioBudget)
{
    LayerRuntime             &rt   = a.layers.runtime[Idx(id)];
    const uint32_t            ttl  = DrawTtlMs(a.layers.config.refreshMs);
    const std::vector<size_t> take = TakeCallBudget(diff.toSend, ioBudget);
    const bool isLocOrGround = id == LayerId::Loc || id == LayerId::Ground;
    EntityRun run;
    run.waiting = static_cast<uint32_t>(diff.toSend.size() - take.size());
    for (size_t i : take)
    {
        const EntityCallResult res = SendEntity(a, id, sel.draws[i], ttl);
        ++run.calls;
        if (res.outcome == EntityCallOutcome::Applied)
        {
            CommitSends(rt.sent, sel.draws, { i }, nowMs);
            ++run.applied;
            continue;
        }
        if (run.error.empty())
        {
            run.error = res.error;
        }
        if (res.outcome == EntityCallOutcome::Refused && !isLocOrGround)
        {
            ++run.refused;
            continue;
        }
        run.stop = res.outcome;
        break;
    }
    ApplyEntityRun(rt, id, run);
}

void SendLayer(app::App &a, LayerId id, const Selection &sel, uint64_t nowMs,
               uint32_t &ioCallBudget)
{
    LayerSyncState &st   = a.layers;
    LayerRuntime   &rt   = st.runtime[Idx(id)];
    const LayerDiff diff = DiffLayer(rt.sent, sel.draws, nowMs, ResendAgeFor(st.config, id));
    if (!diff.toSend.empty())
    {
        const bool canBatch  = IsFastLayer(id) || st.agentMode != AgentEntityMode::PerEntity;
        const bool isBatched = canBatch && SendBatch(a, id, sel, diff, nowMs);
        if (!isBatched)
        {
            SendPerEntity(a, id, sel, diff, nowMs, ioCallBudget);
        }
        if (rt.isRejected)
        {
            ClearLayer(a, rt);
            return;
        }
    }
    if (ClearKeys(a, diff.toClear))
    {
        CommitClears(rt.sent, diff.toClear);
    }
}

// --- refresh -----------------------------------------------------------------

bool IsLive(const LayerSyncState &st, int i, bool isFast)
{
    return IsFastLayer(IdAt(i)) == isFast && st.config.layers[i].isEnabled
        && !st.runtime[i].isRejected;
}

void SelectAll(app::App &a, const nxt::ipc::Snapshot &snap, bool isFast,
               Selection (&sel)[kLayerCount])
{
    LayerSyncState    &st = a.layers;
    const EntityLookup lookup{ &LookupName, &LookupLocSize, &a };
    std::vector<LayerCandidate> candidates;
    for (int i = 0; i < kLayerCount; ++i)
    {
        if (!IsLive(st, i, isFast))
        {
            continue;
        }
        const LayerSettings &s = st.config.layers[i];
        if (IdAt(i) == LayerId::SpotAnim)
        {
            CollectSpotAnims(st.spotAnims, snap, s.maxDistance, candidates);
        }
        else
        {
            CollectCandidates(IdAt(i), snap, s.maxDistance, lookup, candidates);
        }
        sel[i] = SelectLayer(IdAt(i), s, candidates);
        st.runtime[i].matched = sel[i].matched;
    }
}

// Commands the debugger holds for the OTHER refresh group: this group gets
// what is left of kDebuggerStoreBudget.
uint32_t StoreLeft(const LayerSyncState &st, bool isFast)
{
    uint32_t held = 0;
    for (int i = 0; i < kLayerCount; ++i)
    {
        if (IsFastLayer(IdAt(i)) != isFast)
        {
            held += static_cast<uint32_t>(st.runtime[i].sent.size());
        }
    }
    return held >= kDebuggerStoreBudget ? 0 : kDebuggerStoreBudget - held;
}

// Labels: the entity layers spend cfg.labelBudget first; the fast layers get
// what the last entity refresh left. Fast draws then become their commands.
void LabelGroup(LayerSyncState &st, bool isFast, Selection (&sel)[kLayerCount])
{
    if (!isFast)
    {
        st.labels = ApplyLabels(st.config, sel);
        return;
    }
    const int left = st.config.labelBudget - static_cast<int>(st.labels.given);
    st.fastLabels  = ApplyFastLabels(st.config, sel, left > 0 ? left : 0);
    for (int i = 0; i < kLayerCount; ++i)
    {
        if (IsFastLayer(IdAt(i)))
        {
            sel[i].draws = ExpandDraws(IdAt(i), sel[i].draws);
        }
    }
}

void Refresh(app::App &a, bool isFast, uint64_t nowMs)
{
    LayerSyncState &st = a.layers;
    const nxt::ipc::Snapshot *snap = wire::CurrentSnapshot(a.session);
    if (snap == nullptr)
    {
        return;
    }
    // The snapshot pointer is valid for this frame only: select (which copies
    // everything it keeps) before the first RPC blocks the frame.
    Selection sel[kLayerCount];
    SelectAll(a, *snap, isFast, sel);
    FitStoreBudget(st.config, isFast, sel, StoreLeft(st, isFast));
    for (int i = 0; i < kLayerCount; ++i)
    {
        if (IsLive(st, i, isFast))
        {
            st.runtime[i].shown = static_cast<uint32_t>(sel[i].draws.size());
        }
    }
    LabelGroup(st, isFast, sel);
    uint32_t callBudget = kMaxEntityCallsPerRefresh;   // per-entity mode only
    for (int i = 0; i < kLayerCount; ++i)
    {
        if (IsLive(st, i, isFast))
        {
            SendLayer(a, IdAt(i), sel[i], nowMs, callBudget);
        }
    }
}

// New world spot anim starts off the event ring since the last read. The
// first read after the layer turns on (or a reconnect) only anchors on the
// newest event, so history is not replayed as fresh.
void TrackSpotAnims(app::App &a, uint64_t nowMs)
{
    LayerSyncState &st     = a.layers;
    const auto     &log    = a.eventBacklog;
    const uint64_t  newest = log.empty() ? 0 : log.back().seq;
    if (!st.config.layers[Idx(LayerId::SpotAnim)].isEnabled)
    {
        st.spotAnims.clear();
        st.spotSeq = kNoSpotSeq;
        return;
    }
    if (st.spotSeq == kNoSpotSeq || newest < st.spotSeq)
    {
        st.spotSeq = newest;
    }
    for (auto it = log.rbegin(); it != log.rend() && it->seq > st.spotSeq; ++it)
    {
        nxt::ipc::SpotAnimBody body{};
        if (it->type != nxt::ipc::kEventSpotAnim || it->bodyLen < sizeof(body))
        {
            continue;
        }
        std::memcpy(&body, it->body, sizeof(body));
        if (body.targetServerIndex < 0)   // entity-attached ones are a label field
        {
            RecordSpotAnim(st.spotAnims, SpotAnimSighting{ body.spotAnimId, body.tileX,
                                                           body.tileY, body.plane, nowMs });
        }
    }
    st.spotSeq = newest;
    ExpireSpotAnims(st.spotAnims, nowMs, kSpotAnimHoldMs);
}

// The agent drops everything a connection drew when it closes, and a new
// connection owns nothing: either way the sent maps describe nothing real.
// A re-attach to another pid can reconnect within one frame, so the pid
// counts as a new connection too.
void TrackConnection(app::App &a)
{
    LayerSyncState &st          = a.layers;
    const bool      isConnected = a.rpc.IsConnected();
    const uint32_t  pid         = isConnected ? static_cast<uint32_t>(a.rpc.Pid()) : 0;
    if (isConnected != st.wasConnected || pid != st.connectedPid)
    {
        for (LayerRuntime &rt : st.runtime)
        {
            rt.sent.clear();
            rt.isRejected = false;   // a new agent may support what the old did not
            rt.message.clear();
        }
        st.isRefreshForced     = true;
        st.isFastRefreshForced = true;
        st.wasConnected        = isConnected;
        st.connectedPid        = pid;
        st.agentMode           = AgentEntityMode::Unknown;
        st.spotAnims.clear();
        st.spotSeq             = kNoSpotSeq;
    }
}

// Unticked layers give their keys back immediately, not on the next tick, and
// re-ticking a latched layer is the user asking to try again.
void ReleaseDisabled(app::App &a)
{
    LayerSyncState &st = a.layers;
    for (int i = 0; i < kLayerCount; ++i)
    {
        LayerRuntime &rt = st.runtime[i];
        if (st.config.layers[i].isEnabled)
        {
            continue;
        }
        rt.isRejected = false;
        rt.matched    = 0;
        rt.shown      = 0;
        if (!rt.sent.empty())
        {
            ClearLayer(a, rt);
        }
    }
}

bool IsDue(bool &ioIsForced, uint64_t &ioLastMs, int intervalMs, uint64_t nowMs)
{
    const bool isDue = ioIsForced || ioLastMs == 0
                    || nowMs - ioLastMs >= static_cast<uint64_t>(intervalMs);
    if (isDue)
    {
        ioIsForced = false;
        ioLastMs   = nowMs;
    }
    return isDue;
}

// One refresh of one group. Disabled layers of either group are released
// first; the entity group also keeps the cache-backed names current.
void RunRefresh(app::App &a, bool isFast, uint64_t nowMs)
{
    ReleaseDisabled(a);
    if (!AnyLayerOn(a.layers.config, isFast))
    {
        // An idle group holds no captions: the other group's budget is whole.
        (isFast ? a.layers.fastLabels : a.layers.labels) = LabelTally{};
        return;
    }
    if (!isFast)
    {
        TrackCache(a);
    }
    Refresh(a, isFast, nowMs);
}

}   // namespace

void ReleaseLayersOnDetach(app::App &a)
{
    const DWORD pid = a.session.IsOpen() ? a.session.Pid() : 0;
    if (a.rpc.IsConnected() && pid != a.rpcConnectedPid)
    {
        ClearAllLayers(a);
    }
}

void ClearAllLayers(app::App &a)
{
    if (!a.rpc.IsConnected())
    {
        return;
    }
    std::vector<std::string> keys;
    for (const LayerRuntime &rt : a.layers.runtime)
    {
        for (const auto &kv : rt.sent)
        {
            keys.push_back(kv.first);
        }
    }
    ClearKeys(a, keys);
    for (LayerRuntime &rt : a.layers.runtime)
    {
        rt.sent.clear();   // cleared, or gone with the connection either way
    }
}

void UpdateLayers(app::App &a)
{
    LayerSyncState &st = a.layers;
    TrackConnection(a);
    if (!a.rpc.IsConnected() || !a.session.IsOpen())
    {
        return;
    }
    // Paced like a refresh even when only releasing: a clear the agent does
    // not answer is retried on the timer, not every frame.
    const uint64_t now = GetTickCount64();
    if (IsDue(st.isRefreshForced, st.lastRefreshMs, st.config.refreshMs, now))
    {
        const uint32_t before = st.rpcCount;
        RunRefresh(a, false, now);
        st.rpcsLastRefresh = st.rpcCount - before;
    }
    if (IsDue(st.isFastRefreshForced, st.lastFastRefreshMs, st.config.fastRefreshMs, now))
    {
        const uint32_t before = st.rpcCount;
        TrackSpotAnims(a, now);
        RunRefresh(a, true, now);
        st.rpcsLastFastRefresh = st.rpcCount - before;
    }
}

void NotifyLayerConfigChanged(LayerSyncState &st)
{
    SanitizeConfig(st.config);
    st.isRefreshForced     = true;
    st.isFastRefreshForced = true;
}

void RetryLayer(LayerSyncState &st, LayerId id)
{
    LayerRuntime &rt = st.runtime[Idx(id)];
    rt.isRejected = false;
    rt.message.clear();
    st.agentMode           = AgentEntityMode::Unknown;
    st.isRefreshForced     = true;
    st.isFastRefreshForced = true;
}

uint32_t TotalSentDraws(const LayerSyncState &st)
{
    uint32_t n = 0;
    for (const LayerRuntime &rt : st.runtime)
    {
        n += static_cast<uint32_t>(rt.sent.size());
    }
    return n;
}

}
