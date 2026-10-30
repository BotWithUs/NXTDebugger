#pragma once
#include "ipc/SharedLayout.h"
#include "rpc/RpcClient.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ===========================================================================
// Overlay layers -- the pure half.
//
// The Layers panel draws live game state IN the game: one toggle per world
// layer (npcs, players, self, locs, ground items), each pushed to the agent's
// retained draw store as entity highlights it re-projects in-process. This
// header is everything about that which does not need ImGui, App or a pipe:
// settings, snapshot -> candidate collection, filter / distance / cap
// selection, the key scheme, the diff against what was last sent, the batch
// encoding and the reply classification. overlay/LayerSync.cpp drives it;
// tests/LayerTests.cpp drives the same entry points.
//
// Wire contract: Work/overlay-layers/contract.md. Keys are
// `dbg:<layer>:<identity>`; the debugger only ever clears its own keys and
// NEVER calls debug_draw_clear_all (the host and scripts share the overlay).
// ===========================================================================

namespace nxtdbg::overlay
{

enum class LayerId : uint8_t
{
    Npc,
    Player,
    Self,
    Loc,
    Ground,
};
constexpr int kLayerCount = 5;

enum class LabelMode : uint8_t
{
    None,
    Name,
    NameId,
    NameIdDistance,
};
constexpr int kLabelModeCount = 4;

// --- limits ----------------------------------------------------------------

constexpr int      kMaxLayerCount    = 256;   // debug_draw_set_batch item cap
constexpr int      kMaxLayerDistance = 104;   // a loaded scene is 104 tiles across
constexpr int      kMinRefreshMs     = 100;
constexpr int      kMaxRefreshMs     = 10000;
constexpr int      kDefaultRefreshMs = 600;   // about one server tick
constexpr uint32_t kMaxDrawKeyBytes  = 47;    // agent kMaxKeyChars 48 incl. NUL
// Labels each take one of the agent's 64 text slots (kMaxTextSlots), shared with
// every other connection (host, scripts, the Debug Draw panel). Boxes cost none,
// so the debugger caps its labels across all layers, nearest first; the default
// leaves 24 slots for everyone else.
constexpr int      kDefaultLabelBudget = 40;
constexpr int      kMaxLabelBudget     = 64;
// Retained commands store-wide, every connection together (kMaxDrawCmds).
constexpr uint32_t kAgentStoreCap    = 512;

constexpr size_t   kFilterChars = 64;

struct LayerSettings
{
    bool      isEnabled   = false;
    uint32_t  colorArgb   = 0xFFFFAB47u;
    LabelMode labels      = LabelMode::Name;
    bool      showCoords  = false;   // append "(x,y,plane)" to the label
    int       maxDistance = 20;
    int       maxCount    = 100;
    char      filter[kFilterChars] = {};
};

struct LayerConfig
{
    LayerSettings layers[kLayerCount];
    int           refreshMs   = kDefaultRefreshMs;
    int           labelBudget = kDefaultLabelBudget;   // labels across every layer
};

const char *LayerToken(LayerId id);   // "npc", "player", "self", "loc", "ground"
const char *LayerTitle(LayerId id);   // "NPCs", "Players", ...
bool        ParseLayerToken(std::string_view s, LayerId &outId);
const char *LabelModeToken(LabelMode m);   // "none", "name", "name_id", "name_id_dist"
const char *LabelModeTitle(LabelMode m);
bool        ParseLabelMode(std::string_view s, LabelMode &outMode);

LayerSettings DefaultLayerSettings(LayerId id);
LayerConfig   DefaultLayerConfig();

// Clamp every numeric field into its legal range and NUL-terminate the filter.
// Applied after every edit source (panel, ctl, ini) so none can hold a value
// the sync would have to second-guess.
void SanitizeConfig(LayerConfig &io);

bool AnyWorldLayerOn(const LayerConfig &c);

// --- candidates --------------------------------------------------------------

enum class NameKind : uint8_t { Npc, Loc, Item };

// Cache-backed lookups, injected so collection stays testable without a cache.
// `locSize` answers a loc definition's sizeX / sizeY (unrotated); false when
// unknown, and the loc then draws one tile.
struct EntityLookup
{
    std::string (*name)(void *ctx, NameKind kind, int32_t id) = nullptr;
    bool        (*locSize)(void *ctx, int32_t locId, int32_t &outX, int32_t &outY) = nullptr;
    void        *ctx = nullptr;

    std::string Name(NameKind kind, int32_t id) const;
    bool        LocSize(int32_t locId, int32_t &outX, int32_t &outY) const;
};

// A loc's on-ground footprint: the definition size, x and y swapped for
// rotation 1 and 3. The agent draws caller w/h verbatim from the origin tile
// (batch-shape.md: it cannot read the definition size itself yet).
void RotatedFootprint(int32_t sizeX, int32_t sizeY, int rotation, int32_t &outW, int32_t &outH);

// One thing a layer could draw. `selectorId` is what the wire selector carries
// (npc / player server index; for a loc the id the snapshot row reports --
// interactId for a direct location, typeId for a combined section, never the
// appearance id; the item id); `shownId` is the id
// a label shows and `altId` a second id the filter also matches (npc
// morph-resolved type, loc appearance id). -1 where a layer has none.
struct LayerCandidate
{
    int32_t     selectorId = -1;
    int32_t     shownId    = -1;
    int32_t     altId      = -1;
    int32_t     tileX      = 0;
    int32_t     tileY      = 0;
    int32_t     plane      = 0;
    int32_t     distance   = 0;   // chebyshev tiles from the local player
    int32_t     quantity   = 0;   // ground items only
    int32_t     w          = 0;   // footprint in tiles; 0 sends no w/h (locs send it)
    int32_t     h          = 0;
    std::string name;
};

// Everything on the local player's plane within `maxDistance` tiles, with
// names (and loc footprints) resolved. Empty when the snapshot has no local
// player in-world.
void CollectCandidates(LayerId id, const nxt::ipc::Snapshot &s, int maxDistance,
                       const EntityLookup &lookup, std::vector<LayerCandidate> &out);

// Name substring (case-insensitive) or, when the filter is all digits, an
// exact match on any of the candidate's ids. An empty filter matches all.
bool MatchesFilter(const LayerCandidate &c, const char *filter);

// `dbg:<layer>:<identity>`. Identity: server index for npc / player, nothing
// for self, `<id>:<x>:<y>:<plane>` for loc / ground (the contract's auto-key
// shape, so two stacks of one item on one tile are one highlight).
std::string LayerKey(LayerId id, const LayerCandidate &c);
// The prefix every key of a layer starts with: "dbg:npc:" ... and "dbg:self".
std::string LayerKeyPrefix(LayerId id);

struct DesiredDraw
{
    std::string    key;
    LayerCandidate candidate;
    std::string    label;      // filled by ApplyLabels; empty draws no caption
    uint32_t       colorArgb = 0;
};

struct Selection
{
    std::vector<DesiredDraw> draws;   // nearest first, at most maxCount
    uint32_t                 matched = 0;   // M in "showing N of M"
};

// Filter, de-duplicate by key, sort nearest first (key breaks ties so the
// order is stable between refreshes) and cap. `candidates` is consumed.
Selection SelectLayer(LayerId id, const LayerSettings &s,
                      std::vector<LayerCandidate> &candidates);

std::string FormatLabel(LabelMode mode, LayerId id, const LayerCandidate &c,
                        bool showCoords = false);
// True when the layer's settings produce any caption at all.
bool        WantsLabel(const LayerSettings &s);

struct LabelTally
{
    uint32_t given  = 0;   // N in "labels N of M"
    uint32_t wanted = 0;   // M: tiles holding a draw whose layer's label mode is not None
};

// A merged tile caption is cut to this many chars (the agent allows 127).
constexpr size_t kMaxTileLabelChars = 48;

// One caption for every label wanted on one tile, given in caption order:
// identical labels merge as "Name x3", different ones join with " | " (the
// agent draws a caption as a single line), and what does not fit in
// kMaxTileLabelChars ends "... +N more".
std::string MergeTileLabels(const std::vector<std::string> &labels);

// Labels are grouped by world tile (x, y, plane; a loc at its footprint
// origin) across every layer, so entities sharing a tile do not stack their
// captions. Each tile gets one caption, carried by its first draw in layer
// order (self, player, npc, ground, loc), nearest first within a layer; the
// tile's other draws keep their box with no label. At most cfg.labelBudget
// tiles get a caption, nearest to the player first (ties: the carrier's
// layer order). A draw past the budget is still drawn, just without a
// caption. Keys never change: a carrier moving to another draw is a label
// change on both, which the diff re-sends as a restyle.
LabelTally ApplyLabels(const LayerConfig &cfg, Selection (&sel)[kLayerCount]);

// --- diff --------------------------------------------------------------------

struct SentDraw
{
    std::string signature;
    uint64_t    sentAtMs = 0;
};
using SentMap = std::unordered_map<std::string, SentDraw>;

// What a draw looks like on the wire beyond its key: a resend is needed only
// when this changes (or the TTL is running out).
std::string DrawSignature(const DesiredDraw &d);

// Draws are sent with a TTL several refresh intervals long, so a debugger that
// dies leaves nothing behind for more than a few seconds. An unchanged draw is
// re-sent once it is older than ResendAgeMs, two intervals before it expires.
uint32_t DrawTtlMs(int refreshMs);
uint64_t ResendAgeMs(int refreshMs);

struct LayerDiff
{
    std::vector<size_t>      toSend;    // indices into the selection's draws
    std::vector<std::string> toClear;   // sent before, not wanted now
};

LayerDiff DiffLayer(const SentMap &sent, const std::vector<DesiredDraw> &want,
                    uint64_t nowMs, uint64_t resendAgeMs);

// Record a batch the agent applied cleanly, and the clears it confirmed.
void CommitSends(SentMap &io, const std::vector<DesiredDraw> &want,
                 const std::vector<size_t> &sentIdx, uint64_t nowMs);
void CommitClears(SentMap &io, const std::vector<std::string> &cleared);

// Keep every key (it may be on screen and still needs clearing) but forget
// what it carried, so the next diff re-sends what is wanted and clears the
// rest. Used when the store state is unknown (aborted or partial batch).
void MarkAllStale(SentMap &io);

// --- wire --------------------------------------------------------------------

// {items: [...]} for the selected draws. False on encode overflow.
bool EncodeLayerBatch(LayerId id, const std::vector<DesiredDraw> &want,
                      const std::vector<size_t> &idx, uint32_t ttlMs,
                      std::vector<uint8_t> &out);
// {keys: [...]}.
bool EncodeClearKeys(const std::vector<std::string> &keys, std::vector<uint8_t> &out);

enum class BatchOutcome : uint8_t
{
    Applied,     // every item stored
    Partial,     // some refused (`dropped` > 0), e.g. store or text-slot cap
    Rejected,    // nothing stored: the agent does not accept these entries
    Transport,  // no reply (pipe gone, timeout); nothing known
};

struct BatchResult
{
    BatchOutcome outcome = BatchOutcome::Transport;
    uint32_t     count   = 0;
    uint32_t     dropped = 0;
    std::string  error;
};

// Classify one debug_draw_set_batch reply. Every item of a layer batch has the
// same shape, so an envelope error (the handler stops at the first malformed
// item) means the first item failed and nothing was applied: Rejected. So is
// a normal reply that refused every item -- what an agent without entity batch
// entries, or without loc / ground_item support, answers.
BatchResult ClassifyBatchReply(rpc::CallStatus status, const std::vector<uint8_t> &reply,
                               uint32_t itemCount);

// --- per-entity fallback -----------------------------------------------------
//
// Agents released before feat/overlay-loc-ground refuse `kind: "entity"` batch
// items with "unknown kind (want line, rect, ellipse, poly, text or component)"
// but do answer the single highlight_entity RPC for npc / player / self. On
// that refusal the session drops to one highlight_entity per draw, capped per
// refresh so a crowd cannot flood the pipe. Detected once per connection and
// detected again after a reconnect or a Retry.

enum class AgentEntityMode : uint8_t
{
    Unknown,     // no layer batch answered yet on this connection
    Batch,       // debug_draw_set_batch carries entity items
    PerEntity,   // old agent: one highlight_entity call per draw
};

const char *AgentModeToken(AgentEntityMode m);   // "unknown", "batch", "per_entity"
const char *AgentModeTitle(AgentEntityMode m);   // "agent: batch entities", ...

// highlight_entity calls across every layer per refresh in PerEntity mode;
// the rest wait for the next refresh (they are still in the diff then).
constexpr uint32_t kMaxEntityCallsPerRefresh = 64;

// What a per-entity agent's loc / ground layer is latched with.
constexpr const char *kNeedsNewerAgentMessage =
    "needs the newer agent DLL (feat/overlay-loc-ground)";

// The session's mode after one layer batch reply. An "unknown kind" refusal
// means an agent without entity batch items: PerEntity. A reply that stored
// something settles Batch. Anything else (transport, another refusal) leaves
// the mode as it was.
AgentEntityMode ModeAfterBatch(AgentEntityMode current, const BatchResult &res);

// The first min(ioBudget, toSend.size()) indices of toSend (nearest first, the
// diff's order); ioBudget is reduced by what was taken.
std::vector<size_t> TakeCallBudget(const std::vector<size_t> &toSend, uint32_t &ioBudget);

// highlight_entity params for one draw: the batch item without `kind`.
bool EncodeHighlightEntity(LayerId id, const DesiredDraw &d, uint32_t ttlMs,
                           std::vector<uint8_t> &out);

enum class EntityCallOutcome : uint8_t
{
    Applied,
    Capacity,    // store / text slots full: shared, frees up on its own
    Refused,     // the agent will not draw this entry
    Transport,   // no reply; nothing known
};

struct EntityCallResult
{
    EntityCallOutcome outcome = EntityCallOutcome::Transport;
    std::string       error;
};

EntityCallResult ClassifyEntityReply(rpc::CallStatus status, const std::vector<uint8_t> &reply);

// The message a refused layer is latched with. A loc / ground layer refused
// in PerEntity mode is an agent that predates loc / ground_item selectors, so
// it names the DLL needed instead of the agent's raw "needs npc, player or
// self" error.
std::string RefusalMessage(LayerId id, AgentEntityMode mode, const std::string &agentError);

}
