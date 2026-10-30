#pragma once
#include "overlay/Layers.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>

namespace nxtdbg::app { struct App; }

// ===========================================================================
// Overlay layers -- the driver. Owns what was sent to the agent per layer and
// keeps the agent's store in step with LayerConfig through the shared
// App::rpc client:
//
//   * refreshes on a timer (LayerConfig::refreshMs) and only while a world
//     layer is on; at most one debug_draw_set_batch and one debug_draw_clear
//     per layer per refresh, never one RPC per entity;
//   * unticking a layer clears exactly its keys, at once;
//   * detach and app exit clear every dbg: key it sent. It never calls
//     debug_draw_clear_all -- the host and scripts share the overlay;
//   * an agent without entity batch entries (refuses them as "unknown kind")
//     drops the session to one highlight_entity per draw, at most
//     kMaxEntityCallsPerRefresh per refresh; its loc / ground layers latch
//     with "needs the newer agent DLL" once it refuses those selectors;
//   * a layer the agent refuses is latched off with the agent's own error
//     and not retried until the user re-ticks it, retries, or reconnects.
// ===========================================================================

namespace nxtdbg::overlay
{

struct LayerRuntime
{
    SentMap     sent;
    bool        isRejected   = false;   // latched: the agent refuses this layer's entries
    std::string message;                // last agent / transport message, "" when clean
    bool        isWarning    = false;   // message is a warning (partial), not an error
    uint32_t    matched      = 0;       // M in "showing N of M"
    uint32_t    shown        = 0;       // N
    uint32_t    lastDropped  = 0;
};

struct LayerSyncState
{
    LayerConfig  config = DefaultLayerConfig();
    LayerRuntime runtime[kLayerCount];
    uint64_t     lastRefreshMs   = 0;
    bool         isRefreshForced = false;
    bool         wasConnected    = false;
    uint32_t     connectedPid    = 0;   // pid the agent mode was detected on
    // Batch, or per-entity for an agent without entity batch items. Detected
    // on the first layer batch of a connection; reset on reconnect and Retry.
    AgentEntityMode agentMode    = AgentEntityMode::Unknown;
    LabelTally   labels;               // "labels N of M" from the last refresh
    uint32_t     rpcsLastRefresh = 0;
    bool         wasCacheOpen    = false;
    // Memoised cache lookups. Names key on (kind << 32 | id); loc sizes on id.
    std::unordered_map<int64_t, std::string>                  names;
    std::unordered_map<int32_t, std::pair<int32_t, int32_t>>  locSizes;
};

// Once per frame BEFORE App::rpc follows a detach / pid change: while the old
// connection is still open, clear what it drew.
void ReleaseLayersOnDetach(app::App &a);

// Once per frame AFTER the rpc connection is settled.
void UpdateLayers(app::App &a);

// Clear every dbg: key this debugger sent (app exit). Safe when not connected.
void ClearAllLayers(app::App &a);

// Call after any settings edit (panel, ctl): sanitises, refreshes next frame.
void NotifyLayerConfigChanged(LayerSyncState &st);

// Re-arm a latched layer (the panel's Retry button, `layers retry=...`) and
// detect the agent mode again.
void RetryLayer(LayerSyncState &st, LayerId id);

uint32_t TotalSentDraws(const LayerSyncState &st);

}
