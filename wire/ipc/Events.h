#pragma once
#include <cstdint>
#include "SharedLayout.h"

// Event type catalog and per-event body POD layouts. Lives separately from
// SharedLayout so we can extend the type space without churning the snapshot
// schema's version number — adding a new event type is backward-compatible
// (older readers see an unknown discriminator and skip the slot).
//
// Body invariants:
//   - Plain old data only: no pointers, no virtual dispatch, no strings
//     beyond fixed-size inline UTF-8 buffers.
//   - sizeof(Body) <= kEventBodyMax (112).
//   - All multi-byte fields are little-endian (Windows x64).
//   - Scalar-only bodies are packed via offsetof asserts; string bodies use
//     fixed inline buffers with a length prefix and a NUL terminator that
//     readers may rely on.

namespace nxt::ipc {

enum EventType : uint32_t {
    kEventNone               = 0,   // never published; reserved sentinel

    // Session / login lifecycle
    kEventLoginStateChange   = 1,   // game state transitioned (10/20/30 etc.)
    kEventTick               = 2,   // server tick advanced (600ms). Delta-detected
                                    // against the client's server-tick counter, so it
                                    // fires once per real tick — NOT once per ~20ms
                                    // client logic step. See Snapshot::serverTick.
    kEventTokenRefreshFired  = 3,   // an OAuth token refresh was kicked off;
                                    // response not yet received
    kEventTokenRefreshed     = 4,   // new expiry observed — fresh access_token landed
                                    // (also fires on first-seen token at login)
    kEventTokenRefreshFailed = 5,   // state_cookie stuck non-zero past deadline;
                                    // body-less (slot.bodyLen == 0)

    // Variables
    kEventVarChange          = 10,  // a varp value changed (player_var_domain)
    kEventVarbitChange       = 11,  // a varbit value changed (writer derives from varp+def)
    kEventVarcChange         = 12,  // a varc value changed (client_var_domain)
    kEventObjVarChange       = 13,  // a per-item obj var changed (inventory slot's ObjVarDomain)

    // Chat
    kEventChatMessage        = 20,

    // Input
    kEventKeyInput           = 30,

    // Action queue
    kEventActionExecuted     = 40,

    // Break manager
    kEventBreakStarted       = 50,
    kEventBreakEnded         = 51,

    // Walk / pathfinding — PERMANENTLY RESERVED. The agent has never emitted
    // these and never will: the producer-side walker RPCs were retired
    // 2026-08-09 and pathfinding lives in worldwalker.dll on the consumer
    // side, which publishes the equivalent signals into its own host-local
    // event stream. Consumers still decode 60/61/62, so the numbers stay
    // allocated and must never be reused for a different event.
    kEventWalkArrived        = 60,
    kEventWalkCancelled      = 61,
    kEventWalkFailed         = 62,

    // Combat / scene transients
    kEventHitmark            = 70,
    kEventHeadbar            = 71,
    kEventSpotAnim           = 72,

    // Interface
    kEventRadioGroupSelect   = 80,  // outbound radio-group option select observed
                                    // as the client sends it
};

// --- bodies (scalars first) -------------------------------------------------

struct LoginStateChangeBody {
    int32_t oldState;
    int32_t newState;
};
static_assert(sizeof(LoginStateChangeBody) <= kEventBodyMax);

// Carries the client's server-tick counter, the same value the
// snapshot publishes as Snapshot::serverTick — the event marks the edge, the
// snapshot field answers "what is it right now". int32 matches the game's own
// storage width for the counter.
struct TickBody {
    int32_t tick;
};
static_assert(sizeof(TickBody) <= kEventBodyMax);

// Reused for both kEventVarChange (varp, player_var_domain) and
// kEventVarcChange (varc, client_var_domain). The reader switches on the
// EventSlot::type discriminator to decide which ID space `varpId` lives in
// — the field name reflects the more common case and the wire shape is
// identical for both domains.
struct VarChangeBody {
    int32_t varpId;
    int32_t oldValue;
    int32_t newValue;
};
static_assert(sizeof(VarChangeBody) <= kEventBodyMax);

struct VarbitChangeBody {
    int32_t varbitId;
    int32_t oldValue;
    int32_t newValue;
};
static_assert(sizeof(VarbitChangeBody) <= kEventBodyMax);

// Per-item obj-var change (kEventObjVarChange). Obj vars live in an
// inventory slot's ObjVarDomain — a small hashmap of (varId -> int value)
// carried by the item instance in that slot (augmentation XP, charges, etc.).
// Keyed by (invId, slot, varId); the consumer correlates the slot with the
// snapshot's invItems[]. Value is read as int32 (same as the player domain).
struct ObjVarChangeBody {
    int32_t invId;
    int32_t slot;
    int32_t varId;
    int32_t oldValue;
    int32_t newValue;
};
static_assert(sizeof(ObjVarChangeBody) <= kEventBodyMax);

// Field naming matches Java's ActionExecutedEvent (actionId, paramN). The
// values are produced by whatever queue executes them — they're opaque IDs
// from the consumer's perspective, not the game's internal action type space.
struct ActionExecutedBody {
    int32_t actionId;
    int32_t param1;
    int32_t param2;
    int32_t param3;
};
static_assert(sizeof(ActionExecutedBody) <= kEventBodyMax);

// Walk events carry just the world-tile target the consumer asked for. The
// shape is shared across walk_arrived / walk_cancelled / walk_failed (the
// EventSlot::type discriminator carries the outcome). Java's WalkArrivedEvent
// et al. only model {targetX, targetY}; we don't carry plane/reason because
// no consumer reads them. Kept alongside the reserved 60/61/62 discriminators
// so the ring's body shape stays documented — the agent never writes one.
struct WalkBody {
    int32_t targetX;
    int32_t targetY;
};
static_assert(sizeof(WalkBody) <= kEventBodyMax);

struct HitmarkBody {
    int32_t targetServerIndex;
    int8_t  targetType;       // 0=player, 1=npc (matches game's EntityType discriminator subset)
    uint8_t _pad[3];
    int32_t hitmarkType;
    int32_t damage;
    int32_t cycle;
};
static_assert(sizeof(HitmarkBody) <= kEventBodyMax);

struct HeadbarBody {
    int32_t targetServerIndex;
    int8_t  targetType;
    uint8_t _pad[3];
    int32_t headbarType;
    int32_t value;            // bar fill in the game's units (0..hb max)
};
static_assert(sizeof(HeadbarBody) <= kEventBodyMax);

struct SpotAnimBody {
    int32_t targetServerIndex; // -1 if world-anchored
    int8_t  targetType;
    uint8_t _pad[3];
    int32_t spotAnimId;
    int16_t tileX;             // valid for world-anchored spot anims
    int16_t tileY;
    int8_t  plane;
    uint8_t _pad2[3];
};
static_assert(sizeof(SpotAnimBody) <= kEventBodyMax);

// Modifier state is carried as discrete bytes rather than a packed bitfield
// because Java's KeyInputEvent constructor takes individual booleans —
// keeping the wire shape and the consumer API aligned removes a translation
// step in the bridge that converts EventSlot bytes into the Java event.
struct KeyInputBody {
    uint32_t key;              // Win32 VK_* code
    uint8_t  isAlt;
    uint8_t  isCtrl;
    uint8_t  isShift;
    uint8_t  _pad;
};
static_assert(sizeof(KeyInputBody) <= kEventBodyMax);
static_assert(offsetof(KeyInputBody, isAlt) == 4, "KeyInputBody isAlt offset");

// break_started only. break_ended carries no body (slot.bodyLen == 0); the
// EventSlot::type discriminator alone signals the transition.
//
// Field order matches Java's BreakStartedEvent constructor argument order
// (durationSeconds, fatigue, risk). The 4-byte pad is explicit so the wire
// layout is independent of MSVC's padding heuristics.
// OAuth access-token lifecycle. `expirySec` is the token's expiry as a Unix
// epoch in seconds. `secondsRemaining` is signed because a token can
// already be past expiry when we first observe it — the host renders that as
// a negative value rather than wrapping. token_refresh_failed has no body
// (slot.bodyLen == 0); the EventSlot::type discriminator alone is the
// signal, mirroring kEventBreakEnded.
struct TokenRefreshFiredBody {
    uint64_t expirySec;
    int64_t  secondsRemainingAtFire;
};
static_assert(sizeof(TokenRefreshFiredBody) <= kEventBodyMax);

struct TokenRefreshedBody {
    uint64_t expirySec;
    int64_t  secondsUntilExpiry;
};
static_assert(sizeof(TokenRefreshedBody) <= kEventBodyMax);

struct BreakStartedBody {
    int32_t  durationSeconds;
    uint32_t _pad;
    double   fatigue;          // [0,1]
    double   risk;              // cumulative risk score
};
static_assert(sizeof(BreakStartedBody) <= kEventBodyMax);
static_assert(offsetof(BreakStartedBody, fatigue) == 8, "BreakStartedBody fatigue offset");
static_assert(offsetof(BreakStartedBody, risk)    == 16, "BreakStartedBody risk offset");

// Radio-group option select, observed as the client sends it. ifaceId /
// componentId decode the component hash ((iface << 16) | comp). subId is the
// sub-component id, -1 when none. value is a component-derived secondary id
// (-1 in practice for a plain select). opcode is the resolved ClientProt id,
// which encodes which option (1..10) was chosen. All -1 sentinels arrive
// sign-extended from the packet's 16-bit fields.
struct RadioGroupSelectBody {
    int32_t ifaceId;
    int32_t componentId;
    int32_t subId;
    int32_t value;
    int32_t opcode;
};
static_assert(sizeof(RadioGroupSelectBody) <= kEventBodyMax);

// --- string bodies (inline, fixed-capacity) ---------------------------------

// Chat messages: 4-byte type tag + 4-byte sender length + 4-byte text length
// + inline UTF-8 buffers. Reader reads sender from buf[0..senderLen) and text
// from buf[senderLen..senderLen+textLen). The remaining buffer is unused
// padding. Total body capped at kEventBodyMax (112B), so sender+text <= ~96.
struct ChatMessageBody {
    int32_t  msgType;          // game-defined chat type
    uint16_t senderLen;        // bytes (UTF-8)
    uint16_t textLen;
    uint8_t  buf[kEventBodyMax - 8];
};
static_assert(sizeof(ChatMessageBody) == kEventBodyMax);

}
