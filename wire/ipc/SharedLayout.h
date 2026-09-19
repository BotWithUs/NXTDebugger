#pragma once
#include <cstdint>
#include <cstddef>

// Wire-stable layout for the cross-process shared memory region. This file is
// the single source of truth — the consumer-side client library binds against
// the same offsets, so any field change here MUST bump kProtocolVersion and
// the client library must be rebuilt. Static_asserts pin every offset so a
// compiler that pads differently fails to build instead of silently breaking
// readers in another process.
//
// Layout of the mapping:
//   +0x0000  SharedHeader                           (64 bytes, cache-line)
//   +0x0040  Snapshot[0]                            (kSnapshotStride bytes)
//   +0x????  Snapshot[1]                            (kSnapshotStride bytes)
//   +0x????  (event ring — phase 2, currently unused)
//
// All fields are little-endian (Windows x64). No pointers — only integers and
// indices, because virtual addresses do not translate across processes.

namespace nxt::ipc {

// Magic = 'N' 'X' 'T' 'S' little-endian. Consumers verify before binding.
inline constexpr uint32_t kMagic           = 0x5354584Eu;
// v20 widened LocationEntry from 20 to 24 bytes with a trailing resolvedId —
// the loc id after the producer applies the morphvarp ("multiloc") transform.
// Scene locs whose look and menu are chosen by a var are published by the
// server as a base id whose definition has an empty name and empty options, so
// every consumer that matched a Range, a bonfire, a bank chest, a construction
// hotspot or most instanced scenery by name or option matched nothing. The
// base id stays exactly where it was in typeId/interactId, because that is
// what identity, hardcoded id sets and interaction are keyed on; resolvedId is
// additive beside it and is what a definition lookup must use. Growing the row
// shifts every offset past locations[], so this is a hard version bump —
// sizeof(Snapshot) goes from 365744 to 398512.
//
// v19 appended the dynRegion + dynChunks[] tail block — the RS3 "dynamic
// region" (instance) descriptor. When the server rebuilds a scene from a chunk
// table (POH, Dungeoneering floor, most boss instances) the client stops
// reading mapsquares from cache and instead stamps 8x8-tile chunks copied out
// of static source regions, driven by a grid of packed 26-bit descriptors.
// Publishing that grid lets a consumer map an instance tile back to the static
// tile it was copied from, which is what a navigation layer baked against
// static map data needs in order to path inside an instance at all. Appending
// the block grows sizeof(Snapshot) from 300168 to 365744, so this is a hard
// version bump.
//
// v18 made the snapshot's three time bases separately readable and honestly
// named. The u64 at offset 0 was called tickId but is neither a tick nor the
// client's cycle counter — it is this producer's own publish counter, +1 per
// client logic step, so it is now publishSeq. Alongside it the snapshot gained
// the two clocks a consumer actually reasons in: serverTick (the 600ms
// server-side game-logic step) and gameCycle (the
// client's live 20ms counter — the
// number ProjectileEntry::startCycle/endCycle are measured in, which had no
// snapshot source before). Both new fields land in slots that were already
// reserved padding (_reserved0 at +20, _padAfterProjectiles at the tail), so
// sizeof(Snapshot) and every downstream offset are unchanged from v17 — but a
// v17 consumer would read the new fields as the pad bytes it was told to
// ignore, so this is still a hard version bump.
//
// v17 added the projectileCount + projectiles[] tail block — per-tick snapshot
// of every in-flight projectile (a thrown spell/arrow graphic travelling from a
// source to a target). Each row
// carries the graphic id, the launch/land game-cycle stamps, the source/target
// entity server index + type tag (index -1 when the endpoint is a fixed tile),
// and the start/end world tiles. Snapshot-array only — there is no projectile
// event on the ring in v17. Appending the block shifts the total snapshot size,
// so this is a hard version bump.
//
// v16 added a per-entity spotAnimId field to NpcEntry / PlayerEntry / LocalPlayer
// — the first active spot animation (graphic) playing on that entity this tick,
// or -1 if none. Entities can carry several concurrent spot anims; this field
// surfaces only the first for polling, while the kEventSpotAnim event ring
// reports every newly-started one (see Events.h). Growing the three per-entity
// records shifts every downstream snapshot offset, so this is a hard version
// bump.
//
// v15 added the groundItemCount + groundItems[] tail block — per-tick snapshot
// of every alive ground-item stack within the loaded-scene tile bounds.
// Pairs with the host-side rewire of GroundItems and
// SceneObjects facades onto the SHM snapshot (parity with NPCs/Players), and
// retires query_locations / query_ground_items RPCs (and their probe
// subcommands). Bumping the version forces a hard "expected version 15"
// failure if an older consumer binds.
//
// v14 added the openIfaceCount + openIfaces[] tail block — a per-tick snapshot
// of every sub-interface the client currently has open. Replaces the RPC
// round-trip scripts would otherwise pay for an "is-open" probe; the keyset is
// small (~20 ids) so consumers scan it locally.
//
// v13 dropped the per-interface ifaceVersions[] array. Interface state is now
// read fresh on demand via the RPC handlers, so the consumer no longer caches
// component results behind an invalidation token.
inline constexpr uint32_t kProtocolVersion = 20;

// Caps mirror the game's own protocol caps:
//   - NPCs: the client tracks at most 1024 loaded NPCs.
//   - Players: protocol slot space is 2048; the client usually holds fewer,
//     but we size for the worst case so the ring never overflows.
inline constexpr uint32_t kNpcCap    = 1024;
inline constexpr uint32_t kPlayerCap = 2048;
// Skills cap leaves headroom over RS3's ~27 player skills.
inline constexpr uint32_t kSkillCap  = 32;
// Inventories: backpack/bank/equipment + a few specialty inventories
// (deposit box, beast of burden, currency pouch, etc.) — 32 is comfortable.
// Total items across all inventories capped to 2048; bank alone can be ~600,
// so 2048 covers bank + everything else with room to spare.
inline constexpr uint32_t kInventoryCap     = 32;
inline constexpr uint32_t kInventoryItemCap = 2048;

// Scene Locations cap. A heavily-decorated scene (Prif city centre, GE,
// Varrock crowded by other players' summoned objects) can push past 4k
// entries; 8192 leaves headroom and costs 160 KB at 20 B per entry. Any
// overflow is silently truncated — the producer stops appending once it
// hits the cap.
inline constexpr uint32_t kLocationCap = 8192;

// Open sub-interfaces cap. Bounds the per-tick snapshot of the client's set of
// open sub-interfaces. Typical live counts are <20
// (HUD root + a couple of dialogs + bank + skill guides); 64 leaves plenty
// of headroom and costs 260 B per snapshot buffer. Overflow truncates
// silently — the producer stops appending once it hits the cap.
inline constexpr uint32_t kOpenIfaceCap = 64;

// Ground items cap. Ground-item tracking is bounds-gated by the loaded scene
// (~104x104 tiles) and most tiles carry no drops, so realistic live counts
// rarely exceed a couple hundred even in drop-heavy areas (GE, Flash Events).
// 1024 matches kNpcCap, costs 16 KB per snapshot buffer at 16 B per entry,
// and silently truncates on overflow.
inline constexpr uint32_t kGroundItemCap = 1024;

// In-flight projectiles cap (v17+). Live projectiles are sparse even in busy
// multi-combat (a barrage volley tops out around a dozen concurrent flights);
// 256 leaves ample headroom and costs 8 KB per snapshot buffer at 32 B per
// entry. Overflow truncates silently — the producer stops appending once it
// hits the cap.
inline constexpr uint32_t kProjectileCap = 256;

// Dynamic-region chunk-descriptor cap (v19+). Sized 4 planes x 64 x 64.
//
// The only size class measured live is scene mode 6, which is a 32x32 chunk
// grid and needs 4*32*32 = 4096 entries. The headroom is deliberate: the grid
// dimensions arrive on the wire as two raw u8s, modes 4/5/7 have never been
// observed, and a grid that overflows this cap publishes ZERO chunks — so an
// undersized cap does not degrade, it makes the whole block read as a static
// scene. A silent no-op is the worst failure this feature can have, and 64 KB
// per snapshot buffer is cheap insurance against shipping a v20 to fix it.
inline constexpr uint32_t kDynChunkCap = 16384;

// Bit flags shared between NpcEntry and PlayerEntry.
inline constexpr uint8_t kFlagMoving = 1u << 0;

// LocationEntry::flags bits. The combined-section bit lets consumers tell
// a top-level scenery object apart from a sub-tile of a multi-tile combined
// scenery: parents and their sections legitimately share (tile, typeId) but
// not the bit. `deleted` is reported for direct LOCATIONs only — sections share
// their parent's lifetime, so the section path always reports deleted=0.
inline constexpr uint8_t kLocFlagHidden           = 1u << 0;
inline constexpr uint8_t kLocFlagCombinedSection  = 1u << 1;
inline constexpr uint8_t kLocFlagDeleted          = 1u << 2;

struct NpcEntry {
    int32_t  serverIndex;     // 0  server-assigned entity slot index
    int32_t  typeId;          // 4  -1 if the type didn't resolve
    int16_t  tileX;           // 8  absolute world tile X; -1 if the entity has no position yet
    int16_t  tileY;           // 10 absolute world tile Y
    int8_t   plane;           // 12 0..3
    uint8_t  flags;           // 13 kFlagMoving etc.
    int16_t  followingIndex;  // 14 server index of follow target; -1 if none
    int32_t  animationId;     // 16 -1 if not playing
    int32_t  stanceId;        // 20
    int32_t  hp;              // 24 current hitpoints
    int32_t  maxHp;           // 28 maximum hitpoints
    int32_t  spotAnimId;      // 32 first active spot anim (graphic) id; -1 if none
};
static_assert(sizeof(NpcEntry) == 36);
static_assert(alignof(NpcEntry) == 4);

struct PlayerEntry {
    int32_t  serverIndex;     // 0
    int16_t  tileX;           // 4
    int16_t  tileY;           // 6
    int8_t   plane;           // 8
    uint8_t  flags;           // 9
    int16_t  followingIndex;  // 10
    int32_t  animationId;     // 12
    int32_t  stanceId;        // 16
    int32_t  combatLevel;     // 20  0 if the client has not resolved it yet
    int32_t  spotAnimId;      // 24  first active spot anim (graphic) id; -1 if none
};
static_assert(sizeof(PlayerEntry) == 28);
static_assert(alignof(PlayerEntry) == 4);

// Per-skill snapshot: experience, the true level (max attainable, e.g. 99), and
// the boosted/drained current display. typeId identifies the skill (0..26 in
// RS3, with some reserved). Caller reads skillCount and walks
// skills[0..skillCount).
struct SkillEntry {
    int32_t  typeId;          // 0
    int32_t  experience;      // 4
    int32_t  actualLevel;     // 8   true level
    int32_t  boostedLevel;    // 12  current including buffs/drains
};
static_assert(sizeof(SkillEntry) == 16);
static_assert(alignof(SkillEntry) == 4);

// Self-state block. Populated only when gameState == 30 and the local player
// has resolved; zeroed otherwise (serverIndex == -1 means "no local player").
struct LocalPlayer {
    int32_t  serverIndex;     // 0   matches Snapshot::ownIndex when in-world
    int32_t  combatLevel;     // 4
    int16_t  tileX;           // 8
    int16_t  tileY;           // 10
    int8_t   plane;           // 12
    uint8_t  flags;           // 13  kFlagMoving etc.
    int16_t  followingIndex;  // 14
    int32_t  animationId;     // 16
    int32_t  stanceId;        // 20
    int16_t  targetIndex;     // 24
    int8_t   targetType;      // 26
    uint8_t  isMember;        // 27

    int32_t  spotAnimId;      // 28  first active spot anim (graphic) id; -1 if none
    // Keep sizeof(LocalPlayer) a multiple of 8. `self` is followed much further
    // down Snapshot by ProducerState (alignof 8); if any per-entity record's
    // size shifts the running offset off an 8-boundary, the compiler inserts
    // implicit padding before producer that the Java side cannot see by formula
    // (it reads every downstream offset arithmetically). spotAnimId alone would
    // make this 548 (4 mod 8); this pad restores 552. Same rationale as
    // Snapshot::_reserved0 / _padAfterLocations. NpcEntry/PlayerEntry grow by 4
    // each but their *arrays* stay multiples of 8, so they need no such pad.
    uint32_t _pad0;           // 32
    uint32_t   skillCount;    // 36
    SkillEntry skills[kSkillCap];  // 40; 32 * 16 = 512 → ends at 552
};
static_assert(offsetof(LocalPlayer, skills) == 40, "skills offset");
static_assert(sizeof(LocalPlayer) == 40 + sizeof(SkillEntry) * kSkillCap,
              "LocalPlayer has unexpected trailing padding");

// Single item slot. itemId == -1 indicates an empty slot (the game stores
// these for inventories with reserved positions, e.g. equipment). quantity
// is the stack size; for non-stackable items it's 1.
struct InventoryItem {
    int32_t itemId;
    int32_t quantity;
};
static_assert(sizeof(InventoryItem) == 8);
static_assert(alignof(InventoryItem) == 4);

// Per-inventory descriptor. slotCount is the live size of the inventory's
// items vector this tick; firstItemIdx is the offset into Snapshot::invItems
// where this inventory's slots start. Items occupy a contiguous slice
// [firstItemIdx, firstItemIdx + slotCount).
struct InventoryHeader {
    int32_t  invId;          // game inventory ID (e.g. 93 = backpack, 95 = equipment, 94 = bank)
    uint16_t slotCount;
    uint16_t firstItemIdx;
};
static_assert(sizeof(InventoryHeader) == 8);
static_assert(alignof(InventoryHeader) == 4);

// One snapshot row per scene Location — a piece of scenery. A location is
// either a direct one or a section of a multi-tile "combined" object; the
// producer emits both, tagging sections with kLocFlagCombinedSection so
// consumers can filter for the role they care about. Parents and their sections
// legitimately share (tile, typeId), so that flag is the only way to tell them
// apart. interactId is meaningful for direct locations only — sections share
// their parent's type and have no independent runtime id, reported as -1.
//
// Host-side vocabulary note: this is a "SceneObject" to script authors.
struct LocationEntry {
    int32_t typeId;        // 0   cache config type id; -1 if unresolved
    int32_t interactId;    // 4   runtime interaction id; -1 for sections
    int32_t animationId;   // 8   -1 if not animating
    int16_t tileX;         // 12  absolute world tile X
    int16_t tileY;         // 14  absolute world tile Y
    int8_t  plane;         // 16  0..3; 0 if unknown
    uint8_t shape;         // 17  scenery shape code
    uint8_t rotation;      // 18  0..3
    uint8_t flags;         // 19  kLocFlagHidden | kLocFlagCombinedSection | kLocFlagDeleted
    // v20. The loc id a name / option lookup must use for this row, with the
    // morphvarp ("multiloc") transform already applied by the producer.
    //
    // The base id stays authoritative for identity and for the action the row
    // takes: it is what the server sent, what scripts hardcode and what
    // interaction is addressed by, and it is unchanged in typeId/interactId.
    // This field is the *appearance* id — the definition that actually carries
    // the name and the options the player sees. A Range published as base
    // 125195 has no name and no options at all; resolved it is 125205 "Range"
    // with "Cook-at".
    //
    // Always a usable id, never a sentinel: it equals the row's base id when
    // the loc is not a multiloc, and also when the producer declines to
    // resolve (no table, unreadable var, or a transform entry of -1). A
    // consumer needs no null handling and no second lookup.
    int32_t resolvedId;    // 20
};
static_assert(sizeof(LocationEntry) == 24);
static_assert(alignof(LocationEntry) == 4);
static_assert(offsetof(LocationEntry, resolvedId) == 20, "resolvedId offset");

// One snapshot row per alive ground-item stack. The producer sweeps the
// loaded-scene tile bounds and emits a row per alive stack with itemId >= 0. No
// center/radius filter — consumers spatially filter on the immutable
// snapshot. itemId is the cache item type; quantity is the stack size
// (1 for non-stackables). Layout mirrors the field order of the retired
// RPC GroundItemRow so the producer fill is a direct copy.
struct GroundItemEntry {
    int32_t itemId;        // 0
    int32_t quantity;      // 4
    int16_t tileX;         // 8   absolute world tile X
    int16_t tileY;         // 10  absolute world tile Y
    int8_t  plane;         // 12  0..3
    uint8_t _pad[3];       // 13..15 keep stride at 16 for 4-byte alignment
};
static_assert(sizeof(GroundItemEntry) == 16);
static_assert(alignof(GroundItemEntry) == 4);

// One snapshot row per in-flight projectile (v17+). The producer walks the
// client's live projectile list and emits one row per active projectile.
// sourceIndex/targetIndex are the server indices of
// the originating / receiving entity (-1 when that endpoint is a fixed tile
// rather than an entity); sourceType/targetType are the engine's entity-type
// tags passed through raw (interpret consumer-side). Tile coords are absolute
// world tiles (game fine coords >> 9). startCycle/endCycle are the game-cycle
// stamps bracketing the flight — consumers diff against the live game cycle to
// compute flight progress.
struct ProjectileEntry {
    int32_t projectileId;  // 0   graphic (spot-anim) id of the projectile
    int32_t startCycle;    // 4   game cycle the projectile was launched
    int32_t endCycle;      // 8   game cycle it lands
    int16_t sourceIndex;   // 12  server index of source entity; -1 if tile-anchored
    int16_t sourceType;    // 14  source entity-type tag (raw passthrough)
    int16_t targetIndex;   // 16  server index of target entity; -1 if tile target
    int16_t targetType;    // 18  target entity-type tag (raw passthrough)
    int16_t startTileX;    // 20  absolute world tile X of the launch point
    int16_t startTileY;    // 22  absolute world tile Y of the launch point
    int16_t endTileX;      // 24  absolute world tile X of the target point
    int16_t endTileY;      // 26  absolute world tile Y of the target point
    int8_t  plane;         // 28  0..3; ALWAYS equals the local player's plane —
                           //     the server only transmits projectiles on the
                           //     player's own plane, so the projectile node has
                           //     no plane field and none is needed. Consumers can
                           //     rely on this rather than filtering by plane.
    uint8_t _pad[3];       // 29..31 keep stride at 32 for 4-byte alignment
};
static_assert(sizeof(ProjectileEntry) == 32);
static_assert(alignof(ProjectileEntry) == 4);

// Producer-side state surfaced for diagnostics. The RPC handlers are the
// canonical access path for scripts; this block is a passive read-through
// so probes / monitoring hosts can see queue depth and break status without
// a round-trip. breakUntilMs == 0 means "not on break".
struct ProducerState {
    uint32_t actionQueueSize;
    uint8_t  actionsBlocked;
    uint8_t  onBreak;
    uint8_t  _pad[2];
    uint64_t lastActionTimeMs;
    uint64_t breakUntilMs;
    // Monotonic scene-load counter. The producer increments it whenever the
    // set of loaded map squares changes (a different region streamed in), so
    // consumers can invalidate per-scene caches without
    // diffing the full locations array. Was 8 bytes of _reserved in v11;
    // taking 4 here for sceneVersion plus 4 of explicit pad keeps the
    // struct at 32 bytes — no downstream static_assert cascade.
    uint32_t sceneVersion;
    uint32_t _pad4;
};
static_assert(sizeof(ProducerState) == 32);
static_assert(offsetof(ProducerState, actionQueueSize)  == 0);
static_assert(offsetof(ProducerState, actionsBlocked)   == 4);
static_assert(offsetof(ProducerState, onBreak)          == 5);
static_assert(offsetof(ProducerState, lastActionTimeMs) == 8);
static_assert(offsetof(ProducerState, breakUntilMs)     == 16);
static_assert(offsetof(ProducerState, sceneVersion)     == 24);

// Scene-level scalars for the dynamic-region (instance) block (v19+).
//
// UNITS TRAP: originMapX/Y and maxMapX/Y are MAPSQUARE indices (64 tiles each);
// gridW/gridH are CHUNK counts (8 tiles each). The x8 conversion between them
// is the consumer's, and it is the easiest thing on this block to get wrong —
// forgetting it yields a plausible-looking answer 8 tiles from correct.
//
// isInstance is the authoritative flag: it mirrors the producer finding a live
// chunk-descriptor table for the loaded scene, which is the same condition the
// client itself branches on. sceneMode is the raw engine value and is carried
// for diagnostics only (it is also the only way anyone will ever measure the
// unobserved size classes) — branch on isInstance, not on sceneMode.
struct DynamicRegion {
    uint8_t isInstance;      // 0  1 when the scene is a dynamic region
    uint8_t truncated;       // 1  1 when the grid exceeded kDynChunkCap
    uint8_t _pad0[2];        // 2
    int32_t sceneMode;       // 4  3 = static, 4..7 = dynamic size classes
    int32_t originMapX;      // 8  min loaded mapsquare X (grid index origin)
    int32_t originMapY;      // 12
    int32_t maxMapX;         // 16 inclusive max loaded mapsquare X
    int32_t maxMapY;         // 20
    int32_t gridW;           // 24 descriptor width in chunks; 0 when static
    int32_t gridH;           // 28 descriptor height in chunks
    // 4 * gridW * gridH — the entry count the grid actually needed. Always
    // written, including when it exceeds the cap, so a truncation is
    // diagnosable ("40x40 grid, cap 64x64") instead of merely flagged.
    int32_t requiredChunks;  // 32
};
static_assert(sizeof(DynamicRegion)  == 36);
static_assert(alignof(DynamicRegion) == 4);
static_assert(offsetof(DynamicRegion, isInstance)     == 0);
static_assert(offsetof(DynamicRegion, truncated)      == 1);
static_assert(offsetof(DynamicRegion, sceneMode)      == 4);
static_assert(offsetof(DynamicRegion, originMapX)     == 8);
static_assert(offsetof(DynamicRegion, originMapY)     == 12);
static_assert(offsetof(DynamicRegion, maxMapX)        == 16);
static_assert(offsetof(DynamicRegion, maxMapY)        == 20);
static_assert(offsetof(DynamicRegion, gridW)          == 24);
static_assert(offsetof(DynamicRegion, gridH)          == 28);
static_assert(offsetof(DynamicRegion, requiredChunks) == 32);

// Per-tick snapshot. The producer writes the back buffer once per client
// logic step, then publishes by flipping SharedHeader::frontIdx. Readers in any process
// load frontIdx with acquire and read buffers[frontIdx]; the writer never
// touches the front buffer, so the read is race-free as long as the reader
// completes before the *next* publish overruns the back-becomes-front swap.
// At 60 Hz publish and ~80 KB snapshot, a memcpy reader finishes in microseconds.
struct Snapshot {
    // This producer's publish counter: +1 per client logic step (~20ms), even
    // when not in-game, starting at 1 when the producer attaches. It is NOT a
    // server tick and NOT the client's cycle counter — read serverTick or
    // gameCycle below for those. Useful only as a "did the snapshot advance"
    // liveness signal; two agents attached to the same client disagree on it.
    uint64_t publishSeq;

    int32_t  gameState;    // 10 = login, 20 = lobby, 30 = in-game
    int32_t  ownIndex;     // -1 unless gameState == 30; matches self.serverIndex when in-world
    int32_t  rootIfaceId;  // active root interface (e.g. 1477 in resizable HUD mode); -1 if none
    // Server-tick counter (600ms), read from the client's own server-tick
    // counter. This is the clock scripts pace against — respawn timers,
    // cooldowns and drop cadence are all denominated in it. -1 when no client
    // is resolved yet. Occupies what was _reserved0 in v11..v17: ProducerState below
    // is 8-byte aligned and the count fields between here and there add an odd
    // number of u32s, so this slot has to exist regardless — v18 just gave the
    // padding a job. Both sides still agree on every downstream offset.
    int32_t  serverTick;

    LocalPlayer self;      // own-player block; zeroed when not in-world

    uint32_t    npcCount;
    NpcEntry    npcs[kNpcCap];

    uint32_t    playerCount;
    PlayerEntry players[kPlayerCap];

    // Scene Locations block. locationCount entries in locations[]; producer
    // truncates silently at kLocationCap. Direct LOCATIONs and combined-
    // location sections are both emitted as LocationEntry rows, tagged by
    // kLocFlagCombinedSection in flags.
    uint32_t      locationCount;
    LocationEntry locations[kLocationCap];
    // 4-byte explicit pad. locationCount (u32) + locations (kLocationCap*24)
    // is 196612 bytes, which lands at 4 mod 8. Without this pad the compiler
    // inserts implicit padding before ProducerState (alignof 8) and the Java
    // side cannot see the gap by formula. Same pattern as Snapshot::_reserved0.
    uint32_t      _padAfterLocations;

    // Inventories block. inventoryCount entries in inventories[]; each
    // points into invItems[] via firstItemIdx. invItemCount is the total
    // number of items used across all inventories — it never exceeds
    // kInventoryItemCap. Empty inventories produce a header with
    // slotCount == 0 and firstItemIdx unspecified.
    uint32_t        inventoryCount;
    InventoryHeader inventories[kInventoryCap];
    uint32_t        invItemCount;
    InventoryItem   invItems[kInventoryItemCap];

    ProducerState   producer;

    // Open sub-interfaces (v14+). Snapshot of every sub-interface the client
    // had open this tick. Membership in
    // this array is the canonical "is X open" signal — scripts scan it
    // locally instead of paying the per-tick RPC round-trip.
    // openIfaceCount is the live entry count (0..kOpenIfaceCap); entries
    // beyond that index are stale and must not be read.
    uint32_t        openIfaceCount;
    int32_t         openIfaces[kOpenIfaceCap];

    // Ground items block (v15+). groundItemCount entries in groundItems[];
    // producer truncates silently at kGroundItemCap. One row per live ground
    // item stack with itemId >= 0.
    // Replaces the retired query_ground_items RPC. The block lands the
    // running offset at 0 mod 8 (openIfaces ended at 4 mod 8, the new
    // count(4) + 16384 bytes of data) — no trailing pad needed for the
    // alignof-8 Snapshot. The v14 _padTail is gone.
    uint32_t        groundItemCount;
    GroundItemEntry groundItems[kGroundItemCap];

    // Projectiles block (v17+). projectileCount entries in projectiles[];
    // producer truncates silently at kProjectileCap. One row per active
    // in-flight projectile. The preceding groundItems block lands the running
    // offset at 0 mod 8, so projectileCount(4) + projectiles (kProjectileCap*32)
    // ends at 4 mod 8 — the gameCycle field below restores the alignof-8
    // Snapshot to an exact size both sides compute by formula (it was the
    // anonymous _padAfterProjectiles slot through v17).
    uint32_t        projectileCount;
    ProjectileEntry projectiles[kProjectileCap];

    // The client's own game-cycle counter, advancing once per 20ms client
    // main-loop iteration. This
    // is the number the projectiles block above is denominated in: diff a row's
    // startCycle/endCycle against this to get flight progress. Reads 0 only
    // until the client has populated it; verified
    // live 2026-07-30 to be already running in the lobby (gameState 20), so
    // do NOT treat 0 as meaning "not in a world" — serverTick == -1 is that
    // signal. Distinct from publishSeq, which
    // shares the cadence but not the number space. Doubles as the tail pad that
    // keeps sizeof(Snapshot) 8-aligned, so v18 costs zero bytes over v17.
    int32_t         gameCycle;

    // Dynamic-region (instance) block (v19+). See the DynamicRegion comment
    // above for what the scalars mean and for the mapsquare-vs-chunk units trap.
    //
    // dynChunks is the instance's chunk-descriptor grid, flattened PLANE-MAJOR:
    //
    //     index = ((plane * gridW) + gx) * gridH + gy
    //     gx    = (tileX >> 3) - (originMapX << 3)
    //     gy    = (tileY >> 3) - (originMapY << 3)
    //
    // Each entry is the game's own packed 26-bit descriptor, copied verbatim
    // rather than decoded — it is half the bytes for identical information, and
    // the producer fill becomes a straight copy:
    //
    //     negative (-1)  no source chunk (a hole in the instance)
    //     bits 24-25     source plane
    //     bits 14-23     source chunk X (10 bits)
    //     bits  3-13     source chunk Y (11 bits)
    //     bits  1-2      rotation, 0..3
    //
    // Source mapsquare is srcChunk >> 3; source tile origin is srcChunk * 8.
    // Rotation maps a dest-local (x, y) inside the 8x8 chunk to source-local:
    // r0 (x,y), r1 (y,7-x), r2 (7-x,7-y), r3 (7-y,x). Loc rotations compose as
    // (locRot + chunkRot) & 3, and directional/wall collision bits must be
    // rotated by the same amount.
    //
    // TWO CONTRACT POINTS CONSUMERS DEPEND ON:
    //  1. In a static scene the producer publishes dynChunkCount == 0 and
    //     leaves these bytes STALE — it deliberately does not memset 64 KB per
    //     tick for nothing. Never read past dynChunkCount.
    //  2. When truncated is set, dynChunkCount is 0 but gridW/gridH stay
    //     populated, so a consumer can report the size it would have needed.
    //
    // The preceding gameCycle lands the running offset at 0 mod 8, and
    // DynamicRegion(36) + dynChunkCount(4) + kDynChunkCap*4 keeps it there, so
    // sizeof(Snapshot) stays exact with no trailing pad.
    DynamicRegion   dynRegion;
    uint32_t        dynChunkCount;
    uint32_t        dynChunks[kDynChunkCap];
};

static_assert(offsetof(Snapshot, publishSeq)     == 0,                                         "publishSeq offset");
static_assert(offsetof(Snapshot, gameState)      == 8,                                         "gameState offset");
static_assert(offsetof(Snapshot, ownIndex)       == 12,                                        "ownIndex offset");
static_assert(offsetof(Snapshot, rootIfaceId)    == 16,                                        "rootIfaceId offset");
static_assert(offsetof(Snapshot, serverTick)     == 20,                                        "serverTick offset");
static_assert(offsetof(Snapshot, self)           == 24,                                        "self offset");
static_assert(offsetof(Snapshot, npcCount)       == 24 + sizeof(LocalPlayer),                  "npcCount offset");
static_assert(offsetof(Snapshot, npcs)           == 28 + sizeof(LocalPlayer),                  "npcs offset");
static_assert(offsetof(Snapshot, playerCount)    == 28 + sizeof(LocalPlayer)
                                                      + sizeof(NpcEntry) * kNpcCap,            "playerCount offset");
static_assert(offsetof(Snapshot, players)        == 32 + sizeof(LocalPlayer)
                                                      + sizeof(NpcEntry) * kNpcCap,            "players offset");
static_assert(offsetof(Snapshot, locationCount)  == 32 + sizeof(LocalPlayer)
                                                      + sizeof(NpcEntry)    * kNpcCap
                                                      + sizeof(PlayerEntry) * kPlayerCap,      "locationCount offset");
static_assert(offsetof(Snapshot, locations)      == 36 + sizeof(LocalPlayer)
                                                      + sizeof(NpcEntry)    * kNpcCap
                                                      + sizeof(PlayerEntry) * kPlayerCap,      "locations offset");
static_assert(offsetof(Snapshot, inventoryCount) == 40 + sizeof(LocalPlayer)
                                                      + sizeof(NpcEntry)     * kNpcCap
                                                      + sizeof(PlayerEntry)  * kPlayerCap
                                                      + sizeof(LocationEntry)* kLocationCap,   "inventoryCount offset");
static_assert(offsetof(Snapshot, inventories)    == 44 + sizeof(LocalPlayer)
                                                      + sizeof(NpcEntry)     * kNpcCap
                                                      + sizeof(PlayerEntry)  * kPlayerCap
                                                      + sizeof(LocationEntry)* kLocationCap,   "inventories offset");
static_assert(offsetof(Snapshot, producer)       == 48 + sizeof(LocalPlayer)
                                                      + sizeof(NpcEntry)        * kNpcCap
                                                      + sizeof(PlayerEntry)     * kPlayerCap
                                                      + sizeof(LocationEntry)   * kLocationCap
                                                      + sizeof(InventoryHeader) * kInventoryCap
                                                      + sizeof(InventoryItem)   * kInventoryItemCap,
                                                                                                "producer offset");
static_assert(offsetof(Snapshot, openIfaceCount) == 48 + sizeof(LocalPlayer)
                                                      + sizeof(NpcEntry)        * kNpcCap
                                                      + sizeof(PlayerEntry)     * kPlayerCap
                                                      + sizeof(LocationEntry)   * kLocationCap
                                                      + sizeof(InventoryHeader) * kInventoryCap
                                                      + sizeof(InventoryItem)   * kInventoryItemCap
                                                      + sizeof(ProducerState),                  "openIfaceCount offset");
static_assert(offsetof(Snapshot, openIfaces)     == 52 + sizeof(LocalPlayer)
                                                      + sizeof(NpcEntry)        * kNpcCap
                                                      + sizeof(PlayerEntry)     * kPlayerCap
                                                      + sizeof(LocationEntry)   * kLocationCap
                                                      + sizeof(InventoryHeader) * kInventoryCap
                                                      + sizeof(InventoryItem)   * kInventoryItemCap
                                                      + sizeof(ProducerState),                  "openIfaces offset");
static_assert(offsetof(Snapshot, groundItemCount)
                                                 == 52 + sizeof(LocalPlayer)
                                                      + sizeof(NpcEntry)        * kNpcCap
                                                      + sizeof(PlayerEntry)     * kPlayerCap
                                                      + sizeof(LocationEntry)   * kLocationCap
                                                      + sizeof(InventoryHeader) * kInventoryCap
                                                      + sizeof(InventoryItem)   * kInventoryItemCap
                                                      + sizeof(ProducerState)
                                                      + sizeof(int32_t)         * kOpenIfaceCap, "groundItemCount offset");
static_assert(offsetof(Snapshot, groundItems)
                                                 == 56 + sizeof(LocalPlayer)
                                                      + sizeof(NpcEntry)        * kNpcCap
                                                      + sizeof(PlayerEntry)     * kPlayerCap
                                                      + sizeof(LocationEntry)   * kLocationCap
                                                      + sizeof(InventoryHeader) * kInventoryCap
                                                      + sizeof(InventoryItem)   * kInventoryItemCap
                                                      + sizeof(ProducerState)
                                                      + sizeof(int32_t)         * kOpenIfaceCap, "groundItems offset");
static_assert(offsetof(Snapshot, projectileCount)
                                                 == 56 + sizeof(LocalPlayer)
                                                      + sizeof(NpcEntry)        * kNpcCap
                                                      + sizeof(PlayerEntry)     * kPlayerCap
                                                      + sizeof(LocationEntry)   * kLocationCap
                                                      + sizeof(InventoryHeader) * kInventoryCap
                                                      + sizeof(InventoryItem)   * kInventoryItemCap
                                                      + sizeof(ProducerState)
                                                      + sizeof(int32_t)         * kOpenIfaceCap
                                                      + sizeof(GroundItemEntry) * kGroundItemCap, "projectileCount offset");
static_assert(offsetof(Snapshot, projectiles)
                                                 == 60 + sizeof(LocalPlayer)
                                                      + sizeof(NpcEntry)        * kNpcCap
                                                      + sizeof(PlayerEntry)     * kPlayerCap
                                                      + sizeof(LocationEntry)   * kLocationCap
                                                      + sizeof(InventoryHeader) * kInventoryCap
                                                      + sizeof(InventoryItem)   * kInventoryItemCap
                                                      + sizeof(ProducerState)
                                                      + sizeof(int32_t)         * kOpenIfaceCap
                                                      + sizeof(GroundItemEntry) * kGroundItemCap, "projectiles offset");
// Tail field — was the anonymous _padAfterProjectiles slot through v17, so this
// assert is what pins v18's reuse of it to the exact bytes the old pad occupied.
static_assert(offsetof(Snapshot, gameCycle)
                                                 == 60 + sizeof(LocalPlayer)
                                                      + sizeof(NpcEntry)        * kNpcCap
                                                      + sizeof(PlayerEntry)     * kPlayerCap
                                                      + sizeof(LocationEntry)   * kLocationCap
                                                      + sizeof(InventoryHeader) * kInventoryCap
                                                      + sizeof(InventoryItem)   * kInventoryItemCap
                                                      + sizeof(ProducerState)
                                                      + sizeof(int32_t)         * kOpenIfaceCap
                                                      + sizeof(GroundItemEntry) * kGroundItemCap
                                                      + sizeof(ProjectileEntry) * kProjectileCap,
                                                                                                "gameCycle offset");
static_assert(offsetof(Snapshot, dynRegion)
                                                 == 64 + sizeof(LocalPlayer)
                                                      + sizeof(NpcEntry)        * kNpcCap
                                                      + sizeof(PlayerEntry)     * kPlayerCap
                                                      + sizeof(LocationEntry)   * kLocationCap
                                                      + sizeof(InventoryHeader) * kInventoryCap
                                                      + sizeof(InventoryItem)   * kInventoryItemCap
                                                      + sizeof(ProducerState)
                                                      + sizeof(int32_t)         * kOpenIfaceCap
                                                      + sizeof(GroundItemEntry) * kGroundItemCap
                                                      + sizeof(ProjectileEntry) * kProjectileCap,
                                                                                                "dynRegion offset");
static_assert(offsetof(Snapshot, dynChunkCount)
                                                 == 64 + sizeof(DynamicRegion)
                                                      + sizeof(LocalPlayer)
                                                      + sizeof(NpcEntry)        * kNpcCap
                                                      + sizeof(PlayerEntry)     * kPlayerCap
                                                      + sizeof(LocationEntry)   * kLocationCap
                                                      + sizeof(InventoryHeader) * kInventoryCap
                                                      + sizeof(InventoryItem)   * kInventoryItemCap
                                                      + sizeof(ProducerState)
                                                      + sizeof(int32_t)         * kOpenIfaceCap
                                                      + sizeof(GroundItemEntry) * kGroundItemCap
                                                      + sizeof(ProjectileEntry) * kProjectileCap,
                                                                                                "dynChunkCount offset");
static_assert(offsetof(Snapshot, dynChunks)
                                                 == 68 + sizeof(DynamicRegion)
                                                      + sizeof(LocalPlayer)
                                                      + sizeof(NpcEntry)        * kNpcCap
                                                      + sizeof(PlayerEntry)     * kPlayerCap
                                                      + sizeof(LocationEntry)   * kLocationCap
                                                      + sizeof(InventoryHeader) * kInventoryCap
                                                      + sizeof(InventoryItem)   * kInventoryItemCap
                                                      + sizeof(ProducerState)
                                                      + sizeof(int32_t)         * kOpenIfaceCap
                                                      + sizeof(GroundItemEntry) * kGroundItemCap
                                                      + sizeof(ProjectileEntry) * kProjectileCap,
                                                                                                "dynChunks offset");
static_assert(sizeof(Snapshot) == 68 + sizeof(DynamicRegion)
                                + sizeof(LocalPlayer)
                                + sizeof(NpcEntry)        * kNpcCap
                                + sizeof(PlayerEntry)     * kPlayerCap
                                + sizeof(LocationEntry)   * kLocationCap
                                + sizeof(InventoryHeader) * kInventoryCap
                                + sizeof(InventoryItem)   * kInventoryItemCap
                                + sizeof(ProducerState)
                                + sizeof(int32_t)         * kOpenIfaceCap
                                + sizeof(GroundItemEntry) * kGroundItemCap
                                + sizeof(ProjectileEntry) * kProjectileCap
                                + sizeof(uint32_t)        * kDynChunkCap,
              "Snapshot has unexpected trailing padding");

// Absolute pins on the v20 tail. Every other assert above is expressed
// relatively, which means two simultaneous cap edits could cancel out and still
// pass the whole chain. These two cannot — update them deliberately, never
// mechanically, and only when the wire genuinely moved. Both moved by
// kLocationCap * 4 in v20, which is the whole cost of LocationEntry::resolvedId
// (was 300168 / 365744 in v19).
static_assert(offsetof(Snapshot, dynRegion) == 332936, "v20 dynRegion offset drifted");
// Literal, deliberately NOT written as `332976 + sizeof(uint32_t) * kDynChunkCap`
// — that form is parameterised on the cap and would keep passing through a cap
// change, which is exactly the drift this assert exists to catch.
static_assert(sizeof(Snapshot) == 398512, "v20 Snapshot size drifted");
static_assert(sizeof(Snapshot) % 8 == 0, "Snapshot must stay 8-aligned end-to-end");

// Header sits at offset 0. 64-byte aligned so it sits on a single cache line.
// Field order is chosen so all natural alignments are satisfied without any
// internal padding — keeps the wire layout regular and easy for the consumer
// to bind. frontIdx is the only field readers and writers race on; a single
// volatile int32 (manipulated via InterlockedExchange on the writer side and
// acquire-load on the reader side) is sufficient because the writer never
// touches the buffer indicated by the *current* value of frontIdx.
struct SharedHeader {
    uint8_t  magic[4];        // 'N','X','T','S' — read as kMagic in u32 LE
    uint32_t version;         // == kProtocolVersion
    uint32_t headerSize;      // == sizeof(SharedHeader)
    uint32_t layoutId;        // reserved; 0 in v1

    uint32_t snapshotSize;    // == sizeof(Snapshot)
    uint32_t snapshotOff0;    // byte offset of Snapshot[0] within mapping
    uint32_t snapshotOff1;    // byte offset of Snapshot[1] within mapping

    uint32_t ringOff;         // 0 in v1 (event ring not yet present)
    uint32_t ringSize;        // 0 in v1

    int32_t  frontIdx;        // 0 or 1; written via InterlockedExchange
    uint64_t targetPid;       // RS3 process pid (== GetCurrentProcessId() inside the DLL)

    uint32_t _reserved[3];    // pad to 64; future use
};

static_assert(offsetof(SharedHeader, magic)        == 0,  "magic offset");
static_assert(offsetof(SharedHeader, version)      == 4,  "version offset");
static_assert(offsetof(SharedHeader, headerSize)   == 8,  "headerSize offset");
static_assert(offsetof(SharedHeader, layoutId)     == 12, "layoutId offset");
static_assert(offsetof(SharedHeader, snapshotSize) == 16, "snapshotSize offset");
static_assert(offsetof(SharedHeader, snapshotOff0) == 20, "snapshotOff0 offset");
static_assert(offsetof(SharedHeader, snapshotOff1) == 24, "snapshotOff1 offset");
static_assert(offsetof(SharedHeader, ringOff)      == 28, "ringOff offset");
static_assert(offsetof(SharedHeader, ringSize)     == 32, "ringSize offset");
static_assert(offsetof(SharedHeader, frontIdx)     == 36, "frontIdx offset");
static_assert(offsetof(SharedHeader, targetPid)    == 40, "targetPid offset");
static_assert(sizeof(SharedHeader) == 64);

// Stride between snapshot buffers, padded up to a 64-byte boundary so each
// buffer starts on a fresh cache line — avoids false sharing between the
// writer (back buffer) and any reader still finishing the front buffer.
inline constexpr uint32_t kSnapshotStride = (sizeof(Snapshot) + 63u) & ~63u;
inline constexpr uint32_t kSnapshotOff0   = sizeof(SharedHeader);
inline constexpr uint32_t kSnapshotOff1   = sizeof(SharedHeader) + kSnapshotStride;

// ---------------------------------------------------------------------------
// Event ring
//
// Lock-free SPMC ring (single-producer-multi-consumer for now; the algorithm
// works with multi-producer if writers go through InterlockedIncrement on
// `head`, which our PushEvent does). Each slot carries its own seq, so a
// reader can detect overrun by comparing slot.seq to the seq it expected.
//
// Writer:
//   1. seq = InterlockedIncrement64(&head) - 1   // claim a slot
//   2. write slot.type, slot.bodyLen, slot.body
//   3. write slot.seq = seq                       // commit (release on x64)
//
// Reader:
//   hd = head with acquire load
//   for seq = lastSeq; seq < hd; ++seq:
//     slot = slots[seq & slotMask]
//     s = slot.seq with acquire load
//     if s == seq:                process(slot); ++lastSeq
//     elif s > seq:                missed += (s - seq); lastSeq = seq = s
//     else:                        // writer hasn't finished slot — break, retry next tick
//
// At 60 Hz with 1024 slots, a reader has ~17 seconds to drain before the
// writer wraps. droppedCount is a coarse counter the writer increments when
// the slot it's about to overwrite still holds an unread event (consumer
// far behind) — informational only; readers detect drops via seq mismatch.
// ---------------------------------------------------------------------------

inline constexpr uint32_t kEventRingSlots = 1024;       // power of two
inline constexpr uint32_t kEventBodyMax   = 112;        // 128B slot - 16B header
inline constexpr uint32_t kEventSlotSize  = 128;

struct EventSlot {
    uint64_t seq;       // matches the head value at write time; stale slots show smaller seq
    uint32_t type;      // ipc::EventType discriminator (see Events.h)
    uint32_t bodyLen;   // 0..kEventBodyMax
    uint8_t  body[kEventBodyMax];
};
static_assert(sizeof(EventSlot) == kEventSlotSize);
static_assert(alignof(EventSlot) == 8);

struct EventRing {
    uint64_t  head;             // monotonic; only the InterlockedIncrement on this races
    uint32_t  slotCount;        // == kEventRingSlots
    uint32_t  slotMask;         // == slotCount - 1
    uint32_t  droppedCount;     // writer-side counter for slots overwritten with unread data
    uint32_t  _reserved[3];     // pad header to 32B so slots[] starts cache-aligned
    EventSlot slots[kEventRingSlots];
};
static_assert(offsetof(EventRing, slots) == 32, "EventRing header must be 32 bytes");
static_assert(sizeof(EventRing) == 32 + kEventSlotSize * kEventRingSlots);

inline constexpr uint32_t kRingStride = (sizeof(EventRing) + 63u) & ~63u;
inline constexpr uint32_t kRingOff    = sizeof(SharedHeader) + 2 * kSnapshotStride;
inline constexpr uint32_t kRegionSize = kRingOff + kRingStride;

// Mapping name format: Local\nxt_snapshot_<pid>. Local\ scopes the kernel
// object to the current logon session, which is what we want — the consumer
// runs as the same user as the injected client. Decimal pid (max 10 digits
// for u32) keeps the name human-readable in WinObj / handle dumps.
inline constexpr size_t kMaxMappingNameChars = 32;  // L"Local\\nxt_snapshot_" (19) + 10 digits + NUL

}
