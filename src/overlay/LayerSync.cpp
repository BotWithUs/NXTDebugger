#include "LayerSync.h"

#include "app/App.h"
#include "app/EntityName.h"
#include "cache/Json.h"
#include "ctl/Commands.h"
#include "wire/SnapshotReader.h"

#include <Windows.h>

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

rpc::CallStatus Call(app::App &a, const char *method, const std::vector<uint8_t> &body,
                     std::vector<uint8_t> &outReply)
{
    ++a.layers.rpcsLastRefresh;
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

void SendLayer(app::App &a, LayerId id, const Selection &sel, uint64_t nowMs)
{
    LayerRuntime   &rt   = a.layers.runtime[Idx(id)];
    const int       rms  = a.layers.config.refreshMs;
    const LayerDiff diff = DiffLayer(rt.sent, sel.draws, nowMs, ResendAgeMs(rms));
    if (!diff.toSend.empty())
    {
        std::vector<uint8_t> body;
        std::vector<uint8_t> reply;
        if (!EncodeLayerBatch(id, sel.draws, diff.toSend, DrawTtlMs(rms), body))
        {
            rt.message = "batch too large to encode";
            return;
        }
        const rpc::CallStatus status = Call(a, "debug_draw_set_batch", body, reply);
        ApplyBatchResult(rt, sel, diff,
                         ClassifyBatchReply(status, reply,
                                            static_cast<uint32_t>(diff.toSend.size())),
                         nowMs);
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

void SelectAll(app::App &a, const nxt::ipc::Snapshot &snap, Selection (&sel)[kLayerCount])
{
    LayerSyncState    &st = a.layers;
    const EntityLookup lookup{ &LookupName, &LookupLocSize, &a };
    std::vector<LayerCandidate> candidates;
    for (int i = 0; i < kLayerCount; ++i)
    {
        const LayerSettings &s  = st.config.layers[i];
        LayerRuntime        &rt = st.runtime[i];
        if (!s.isEnabled || rt.isRejected)
        {
            continue;
        }
        CollectCandidates(IdAt(i), snap, s.maxDistance, lookup, candidates);
        sel[i]     = SelectLayer(IdAt(i), s, candidates);
        rt.matched = sel[i].matched;
        rt.shown   = static_cast<uint32_t>(sel[i].draws.size());
    }
}

void Refresh(app::App &a, uint64_t nowMs)
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
    SelectAll(a, *snap, sel);
    st.labels = ApplyLabels(st.config, sel);
    for (int i = 0; i < kLayerCount; ++i)
    {
        if (st.config.layers[i].isEnabled && !st.runtime[i].isRejected)
        {
            SendLayer(a, IdAt(i), sel[i], nowMs);
        }
    }
}

// The agent drops everything a connection drew when it closes, and a new
// connection owns nothing: either way the sent maps describe nothing real.
void TrackConnection(app::App &a)
{
    LayerSyncState &st          = a.layers;
    const bool      isConnected = a.rpc.IsConnected();
    if (isConnected != st.wasConnected)
    {
        for (LayerRuntime &rt : st.runtime)
        {
            rt.sent.clear();
            rt.isRejected = false;   // a new agent may support what the old did not
            rt.message.clear();
        }
        st.isRefreshForced = true;
        st.wasConnected    = isConnected;
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
    const uint64_t now   = GetTickCount64();
    const bool     isDue = st.isRefreshForced || st.lastRefreshMs == 0
                        || now - st.lastRefreshMs >= static_cast<uint64_t>(st.config.refreshMs);
    if (!isDue)
    {
        return;
    }
    st.isRefreshForced = false;
    st.lastRefreshMs   = now;
    st.rpcsLastRefresh = 0;
    ReleaseDisabled(a);
    if (AnyWorldLayerOn(st.config))
    {
        TrackCache(a);
        Refresh(a, now);
    }
}

void NotifyLayerConfigChanged(LayerSyncState &st)
{
    SanitizeConfig(st.config);
    st.isRefreshForced = true;
}

void RetryLayer(LayerSyncState &st, LayerId id)
{
    LayerRuntime &rt = st.runtime[Idx(id)];
    rt.isRejected = false;
    rt.message.clear();
    st.isRefreshForced = true;
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
