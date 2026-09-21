# BotWithUs agent wire protocol — v21

How to read live RuneScape 3 state out of the BotWithUs agent, and how to drive
it, from **any language**. This is the normative description of the bytes; the
headers in this directory are the same contract expressed in C++.

Everything here is little-endian x64. Structs use natural alignment with no
packing pragmas. Every absolute offset and size below is pinned to a real
`offsetof` by compile-time assertions in the reference consumer
(`src/wire/ProtocolDocPins.h`), so a layout change breaks its build instead of
quietly invalidating this page; the event and method catalogues are held to the
producer the same way by `tools/check_protocol_doc.ps1`. Trust the numbers, but
prefer the header fields where one is offered (§2.2).

The agent exposes two independent surfaces:

| Surface | Object | Use it for |
|---|---|---|
| **Snapshot SHM** | `Local\nxt_snapshot_<pid>` | Polling live state. No round-trip, no handshake, read-only. |
| **RPC pipe** | `\\.\pipe\BotWithUs_<pid>` | Mutations and queries that need a reply. MsgPack request/response. |

`<pid>` is the decimal process id of the game client. Enumerate `rs2client*.exe`
to find candidates; a client with no agent has neither object.

Read state from the snapshot, not by polling RPC. The snapshot is republished
every client logic step (~20ms) and costs a consumer nothing but a memory read.

---

## 1. Versioning

`kProtocolVersion` is **21**.

- The version gates the **snapshot layout only**. Field offsets move between
  versions and there is **no forward compatibility**.
- **A consumer must refuse a mapping whose `version` != the one it was built
  against.** Reading a mismatched layout yields plausible-looking garbage, which
  is far worse than failing.
- **RPC methods and event types are additive** and do *not* bump the version. An
  unknown event discriminator or an unknown RPC method name is a normal
  condition — skip it, don't error.

---

## 2. Snapshot SHM

### 2.1 Opening

Open the file mapping `Local\nxt_snapshot_<pid>` read-only and map a view of it.
`Local\` scopes it to the logon session, so the consumer must run as the same
user as the client.

Then validate, in this order:

1. `magic` == `0x5354584E` (`'N','X','T','S'` LE). Wrong magic → not our region.
2. `version` == `21`. Mismatch → refuse (see §1).
3. `headerSize` == 64 and `snapshotSize` == 410800 as a sanity check.

### 2.2 Region geometry

| Constant | Value |
|---|---|
| `kMagic` | `0x5354584E` |
| `kProtocolVersion` | 21 |
| `sizeof(SharedHeader)` | 64 |
| `sizeof(Snapshot)` | 410800 |
| snapshot stride (padded to 64B) | 410816 |
| Snapshot[0] offset | 64 |
| Snapshot[1] offset | 410880 |
| Event ring offset | 821696 |
| Event ring size (padded) | 131136 |
| Total region size | 952832 |

Do not hardcode these blindly — the header carries `snapshotOff0`,
`snapshotOff1`, `ringOff` and `ringSize` for exactly this reason. Prefer reading
them.

### 2.3 `SharedHeader` (64 bytes, at offset 0)

| field | off | size | notes |
|---|---|---|---|
| `magic` | 0 | 4 | `'N','X','T','S'` |
| `version` | 4 | 4 | == 21 |
| `headerSize` | 8 | 4 | == 64 |
| `layoutId` | 12 | 4 | reserved, 0 |
| `snapshotSize` | 16 | 4 | == 410800 |
| `snapshotOff0` | 20 | 4 | byte offset of buffer 0 |
| `snapshotOff1` | 24 | 4 | byte offset of buffer 1 |
| `ringOff` | 28 | 4 | byte offset of the event ring |
| `ringSize` | 32 | 4 | bytes |
| `frontIdx` | 36 | 4 | **0 or 1** — the buffer safe to read |
| `targetPid` | 40 | 8 | the client's pid |

### 2.4 The double-buffer read protocol

Two snapshot buffers alternate. The producer writes the one `frontIdx` does not
point at, then flips `frontIdx`. It never touches the front buffer, so a reader
that reads the front buffer races with nothing.

```
idx  = atomic_load_acquire(header.frontIdx)     // 0 or 1
base = (idx == 0) ? header.snapshotOff0 : header.snapshotOff1
copy 410800 bytes from mapping[base]            // then parse the copy
```

Two rules that matter:

- **Load `frontIdx` with acquire semantics** (or a full barrier). Plain reads
  may be reordered against the buffer read.
- **Copy the buffer out before parsing it, and never hold a pointer into the
  mapping across frames.** The read is only safe while the producer hasn't
  published *twice* more; a bulk copy finishes in microseconds and takes that
  risk to zero. Parsing field-by-field directly out of the mapping over several
  milliseconds does not.

There is no reader registration and no backpressure — the producer never waits
for you.

### 2.5 `Snapshot` (410800 bytes)

| field | off | size | notes |
|---|---|---|---|
| `publishSeq` | 0 | 8 | u64, producer republish counter — **not a clock**, see §2.6 |
| `gameState` | 8 | 4 | `10` login, `20` lobby, `30` in-game |
| `ownIndex` | 12 | 4 | local player's server index; -1 unless `gameState == 30` |
| `rootIfaceId` | 16 | 4 | active root interface; -1 if none |
| `serverTick` | 20 | 4 | **600ms server tick — pace on this**, see §2.6 |
| `self` | 24 | 552 | `LocalPlayer`, §2.7 |
| `npcCount` | 576 | 4 | |
| `npcs` | 580 | 40960 | `NpcEntry[1024]`, stride 40 |
| `playerCount` | 41540 | 4 | |
| `players` | 41544 | 65536 | `PlayerEntry[2048]`, stride 32 |
| `locationCount` | 107080 | 4 | |
| `locations` | 107084 | 196608 | `LocationEntry[8192]`, stride 24 |
| `inventoryCount` | 303696 | 4 | |
| `inventories` | 303700 | 256 | `InventoryHeader[32]`, stride 8 |
| `invItemCount` | 303956 | 4 | |
| `invItems` | 303960 | 16384 | `InventoryItem[2048]`, stride 8 |
| `producer` | 320344 | 32 | `ProducerState`, §2.9 |
| `openIfaceCount` | 320376 | 4 | |
| `openIfaces` | 320380 | 256 | `int32[64]` — open sub-interface ids |
| `groundItemCount` | 320636 | 4 | |
| `groundItems` | 320640 | 16384 | `GroundItemEntry[1024]`, stride 16 |
| `projectileCount` | 337024 | 4 | |
| `projectiles` | 337028 | 8192 | `ProjectileEntry[256]`, stride 32 |
| `gameCycle` | 345220 | 4 | **~20ms client cycle**, see §2.6 |
| `dynRegion` | 345224 | 36 | `DynamicRegion` — instance descriptor scalars, §2.10 |
| `dynChunkCount` | 345260 | 4 | |
| `dynChunks` | 345264 | 65536 | `uint32[16384]` — packed chunk descriptors, §2.10 |

Every `*Count` is the live entry count; **entries past it are stale and must not
be read**. Counts saturate at the array cap and the producer truncates silently,
so a count equal to the cap may mean "there were more".

`dynChunks` is the one array the producer does **not** clear when empty: in a
static scene it publishes `dynChunkCount == 0` and leaves the 64 KB untouched
rather than memsetting it every tick. Reading past the count there returns the
previous instance's grid.

### 2.6 The three clocks — the easiest thing to get wrong

Three separate counters, three different number spaces. Through v17 only the
third existed, misnamed `tickId`; anything that paced off it ran ~30x fast.

| Field | Cadence | Use it for |
|---|---|---|
| `serverTick` | **600 ms** | **The clock to pace logic against.** Respawn timers, cooldowns, drop cadence are all denominated in server ticks. `-1` when no client is resolved yet. |
| `gameCycle` | ~20 ms | The unit `ProjectileEntry.startCycle`/`endCycle` are stamped in. Diff against it for flight progress. Runs in the lobby too, so `0` does **not** mean "not in a world" — `serverTick == -1` is that signal. |
| `publishSeq` | ~20 ms | This producer's own republish counter. A liveness signal only ("did the snapshot advance"). Two agents on the same client disagree on it. |

`gameCycle` and `publishSeq` share a cadence but **not** a number space — never
compare or substitute them.

### 2.7 `LocalPlayer` (552 bytes, at snapshot offset 24)

Zero-filled when not in-world — so `serverIndex` then reads `0`, not `-1`;
test `ownIndex == -1` for "no local player". One exception: `orientation`
holds the `0xFFFF` sentinel, because a zero there would read as a valid
facing (§2.8, *Entity orientation*).

| field | off | size |
|---|---|---|
| `serverIndex` | 0 | 4 |
| `combatLevel` | 4 | 4 |
| `tileX` | 8 | 2 |
| `tileY` | 10 | 2 |
| `plane` | 12 | 1 |
| `flags` | 13 | 1 |
| `followingIndex` | 14 | 2 |
| `animationId` | 16 | 4 |
| `stanceId` | 20 | 4 |
| `targetIndex` | 24 | 2 |
| `targetType` | 26 | 1 |
| `isMember` | 27 | 1 |
| `spotAnimId` | 28 | 4 |
| `orientation` | 32 | 2 | u16, **v21+**, §2.8 *Entity orientation*. Authoritative for the local player. |
| `_orientationPad` | 34 | 2 | zero |
| `skillCount` | 36 | 4 |
| `skills` | 40 | 512 | `SkillEntry[32]`, stride 16 |

### 2.8 Entry layouts

Tile coordinates are **absolute world tiles**. `plane` is 0..3.
`flags` bit 0 (`kFlagMoving`) is set when the entity is moving.

**`NpcEntry`** (40) — `serverIndex` 0, `typeId` 4, `tileX` 8, `tileY` 10,
`plane` 12, `flags` 13, `followingIndex` 14, `animationId` 16, `stanceId` 20,
`hp` 24, `maxHp` 28, `spotAnimId` 32, `orientation` 36 (u16, **v21+**),
`_pad0` 38 (u16, zero). `typeId` is -1 if unresolved;
`animationId`/`spotAnimId` are -1 when inactive; `followingIndex` -1 if none.

**`PlayerEntry`** (32) — `serverIndex` 0, `tileX` 4, `tileY` 6, `plane` 8,
`flags` 9, `followingIndex` 10, `animationId` 12, `stanceId` 16,
`combatLevel` 20, `spotAnimId` 24, `orientation` 28 (u16, **v21+**),
`_pad0` 30 (u16, zero).

**Entity orientation** (**v21+**) — `LocalPlayer.orientation`,
`NpcEntry.orientation` and `PlayerEntry.orientation` share one encoding.

- **Value domain (producer invariant).** Every published value is either
  `0..16383` (`0x0000..0x3FFF`) or exactly `0xFFFF`. Nothing in
  `0x4000..0xFFFE` is ever written; treat one as a producer bug.
- **Known vs unknown.** `0xFFFF` means unknown: the entity has no position yet
  (`tileX == -1`), the read failed, or the local player is not in the world.
  Test `orientation != 0xFFFF`. **Never** infer it from `tileX`: the
  out-of-world `LocalPlayer` block is zero-filled, so its `tileX` reads `0`.
- **What it measures.** The entity model's **current, rendered** facing: the
  rotation quaternion on its scene node, not the goal of a turn in progress and
  not a movement direction. The producer converts it with the client's own
  arithmetic, operation for operation, so the value is what the client itself
  would compute.
- **Units.** The client's 14-bit angle: **16384 units per full turn**, no rescale.
- **Convention** (from static analysis of client build 950-1; **STATIC, pending a
  live check**): `0` = south, `4096` = west, `8192` = north, `12288` = east —
  increasing **clockwise seen from above** with north (`+tileY`) up.
- **Compass degrees** (0 = north, 90 = east), with a non-negative `mod`:
  `compassDeg = ((raw - 8192) mod 16384) * 360 / 16384`

  | raw | compassDeg | direction |
  |---:|---:|---|
  | 8192 | 0 | N |
  | 10240 | 45 | NE |
  | 12288 | 90 | E |
  | 14336 | 135 | SE |
  | 0 | 180 | S |
  | 2048 | 225 | SW |
  | 4096 | 270 | W |
  | 6144 | 315 | NW |

  To bucket into eight directions: `round(compassDeg / 45) mod 8`.
- **Precision.** The client's conversion truncates, so a facing set from a
  server angle `j` can read back as `j - 1`. Compare with a tolerance of one
  unit, not for equality.
- **Self vs `players[]`.** `LocalPlayer.orientation` is read once per publish and
  the `players[]` row whose `serverIndex == ownIndex` copies it, so the two are
  byte-identical in every publish. Read either; `LocalPlayer` is authoritative.

*On `spotAnimId`*: this is the graphic playing **on** that entity, `-1` for none,
and it is a recent arrival — through most of v19 the producer read the wrong
object for entity-attached graphics, so the field was permanently `-1` on
`NpcEntry` / `PlayerEntry` / `LocalPlayer` and event type 72 only ever fired
world-anchored. Both now report. Treat a consumer that saw nothing here as
having been correct at the time rather than broken. A single graphic is reached
through several of the entity's child nodes, so the producer dedupes on the
owning entity: expect **one** event per graphic instance, not one per node. The
dedupe keys on identity rather than on `spotAnimId`, so two concurrent casts of
the same graphic stay distinct. World-anchored graphics are a separate list —
`query_spot_anims` (§4.4), not these fields.

**`LocationEntry`** (24) — scenery. `typeId` 0, `interactId` 4, `animationId` 8,
`tileX` 12, `tileY` 14, `plane` 16, `shape` 17, `rotation` 18, `flags` 19,
`resolvedId` 20 (**v20+**).
`flags` bits: `1` hidden, `2` combined-section, `4` deleted. A multi-tile object
emits a parent row plus one row per section; parents and sections legitimately
share `(tile, typeId)`, so **the combined-section bit is the only way to tell
them apart**. `interactId` is -1 for sections. Script-facing hosts call this a
*SceneObject*.

**The row carries two ids and they answer different questions.** The *base id*
is what the server sent and is where identity, hardcoded id sets and
interaction are keyed: it is `interactId` on a direct row and `typeId` on a
combined-section row — the same pick the combined-section bit already forces
you to make. `resolvedId` is the *appearance* id: the definition that actually
carries the name and the right-click options, with the morphvarp ("multiloc")
transform already applied by the producer.

Use `resolvedId` for **every name or option lookup**, and the base id for
identity and for addressing an interaction. A morph loc is published by the
server as one base id whose definition has an empty name and no options at all
— Ranges, bonfires, bank chests, construction hotspots and most instanced
scenery are all morph locs — so a consumer matching a base id against the cache
matches nothing. A Range published as base `125195` resolves to `125205`
"Range" with `["Cook-at"]`.

`resolvedId` is **always a usable id and never a sentinel**: it equals the row's
base id when the loc is not a multiloc, and also when the producer declines to
resolve (no transform table, an unreadable var, or a transform entry of -1). No
null handling and no second lookup is needed. Clicking is unaffected either way
— an interaction addressed to the base id already works, because the client
resolves the loc itself. Only *discovery* was ever broken.

**`GroundItemEntry`** (16) — `itemId` 0, `quantity` 4, `tileX` 8, `tileY` 10,
`plane` 12. One row per alive stack within the loaded scene.

**`ProjectileEntry`** (32) — `projectileId` 0, `startCycle` 4, `endCycle` 8,
`sourceIndex` 12, `sourceType` 14, `targetIndex` 16, `targetType` 18,
`startTileX` 20, `startTileY` 22, `endTileX` 24, `endTileY` 26, `plane` 28.
`sourceIndex`/`targetIndex` are -1 when that endpoint is a fixed tile.
Cycles are in `gameCycle` units. `plane` always equals the local player's plane
— the server only transmits projectiles on your own plane.

**`InventoryHeader`** (8) — `invId` 0, `slotCount` 4 (u16), `firstItemIdx` 6
(u16). Items live at `invItems[firstItemIdx .. firstItemIdx + slotCount)`.
Common ids: 93 backpack, 94 bank, 95 equipment.

**`InventoryItem`** (8) — `itemId` 0, `quantity` 4. `itemId == -1` is an empty
slot (reserved positions, e.g. equipment). `quantity` is 1 for non-stackables.

**`SkillEntry`** (16) — `typeId` 0, `experience` 4, `actualLevel` 8,
`boostedLevel` 12. `actualLevel` is the true level; `boostedLevel` includes
buffs/drains.

### 2.9 `ProducerState` (32 bytes)

| field | off | size |
|---|---|---|
| `actionQueueSize` | 0 | 4 |
| `actionsBlocked` | 4 | 1 |
| `onBreak` | 5 | 1 |
| `lastActionTimeMs` | 8 | 8 |
| `breakUntilMs` | 16 | 8 |
| `sceneVersion` | 24 | 4 |

`breakUntilMs == 0` means not on break. `sceneVersion` increments whenever the
loaded region changes — use it to invalidate per-scene caches instead of diffing
the whole `locations` array.

### 2.10 `DynamicRegion` + `dynChunks` — instances (v19+)

When the server builds an instance (player-owned house, Dungeoneering floor,
most boss instances) it does not stream mapsquares from cache. It sends a chunk
table, and the client stamps the scene out of 8x8-tile chunks **copied from
static source regions**. This block publishes that table, so a consumer holding
static map data can map an instance tile back to the tile it was copied from.

`DynamicRegion` (36 bytes, at snapshot offset 345224):

| field | off | size | notes |
|---|---|---|---|
| `isInstance` | 0 | 1 | **the authoritative flag** — branch on this |
| `truncated` | 1 | 1 | grid unusable, `dynChunkCount` is 0 — two causes, see below |
| `sceneMode` | 4 | 4 | `3` static, `4..7` dynamic size classes, **`-1` no scene**; diagnostic only |
| `originMapX` | 8 | 4 | min loaded **mapsquare** X — the grid index origin; `-1` if no scene |
| `originMapY` | 12 | 4 | `-1` if no scene |
| `maxMapX` | 16 | 4 | inclusive max loaded mapsquare X; `-1` if no scene |
| `maxMapY` | 20 | 4 | `-1` if no scene |
| `gridW` | 24 | 4 | descriptor width in **chunks**; 0 when static |
| `gridH` | 28 | 4 | |
| `requiredChunks` | 32 | 4 | `4 * gridW * gridH` — populated even past the cap, so an overflow is diagnosable; 0 when no grid was read |

**`-1` = no scene resolved.** Whenever no scene world resolves — before a world
is loaded, for instance — all five of `sceneMode` / `originMapX` / `originMapY` /
`maxMapX` / `maxMapY` publish `-1`.
Distinct from a real static scene, which carries `sceneMode == 3` and real
mapsquare indices. `isInstance` and `truncated` are both 0 either way, so this
only bites code that reads the scalars directly — `originMapX << 3` on `-1`
yields `-8`, not a miss.

**`truncated` has two causes**, and `dynChunkCount` is 0 for both (a partially
filled plane-major array cannot be indexed safely). The dimensions separate them:
cap overflow leaves `gridW`/`gridH`/`requiredChunks` **populated**; a descriptor
that did not read back as a grid at all leaves all three **0**. Code that assumes
the first case reports a meaningless `0x0`.

**Units trap:** `originMap*` / `maxMap*` are mapsquares (64 tiles); `gridW` /
`gridH` are chunks (8 tiles). Forgetting the `x8` yields a plausible-looking
answer 8 tiles from correct.

`dynChunks` is the grid flattened **plane-major**:

```
gx = (tileX >> 3) - (originMapX << 3)
gy = (tileY >> 3) - (originMapY << 3)

// Range-check BEFORE flattening. Not optional — see below.
// Always exactly 4 planes (0..3) — the formula assumes it, and it is why
// requiredChunks is 4 * gridW * gridH.
if (plane < 0 || plane >= 4 || gx < 0 || gx >= gridW || gy < 0 || gy >= gridH)
    -> no source chunk

index = ((plane * gridW) + gx) * gridH + gy

// Second guard: the count, not the cap.
if (index < 0 || index >= dynChunkCount)
    -> no source chunk
```

**The range check is load-bearing; bounds-checking `index` alone is not enough.**
Out-of-grid tiles are routine (see the `maxMapX/Y` note below), and an unchecked
`(gx, gy)` does not overflow the index — it **aliases onto a live cell belonging
to another location**. On a 32x32 grid (`dynChunkCount == 4096`), `gx=0, gy=32`
gives `index = 32`, which is cell `(1, 0)`; `gx=-1, gy=32` gives `index = 0`, the
origin cell. Both pass `0 <= index < dynChunkCount` and resolve to an unrelated
source chunk — a confident wrong answer instead of a miss.

Each entry is the game's own packed descriptor, copied verbatim:

| bits | field |
|---|---|
| 24-25 | source plane |
| 14-23 | source chunk X (10 bits) |
| 3-13 | source chunk Y (11 bits) |
| 1-2 | rotation, 0..3 |

A **negative** entry means "no source chunk" — a **hole** in the instance. A hole
reads as fully blocked: genuinely solid, not merely unknown, so collision code
treats those tiles as impassable rather than as data to fill in from the static
map.

Call the extracted 10- and 11-bit values `srcChunkX` / `srcChunkY`. Both convert
**per axis**: source mapsquare is `srcChunkX >> 3` / `srcChunkY >> 3`, source tile
origin is `srcChunkX * 8` / `srcChunkY * 8`. Rotation maps a destination-local
`(x, y)` inside the 8x8 chunk to source-local:

| rot | source-local |
|---|---|
| 0 | `(x, y)` |
| 1 | `(y, 7-x)` |
| 2 | `(7-x, 7-y)` |
| 3 | `(7-y, x)` |

Scenery rotations compose the same way — `(locRot + chunkRot) & 3` — and any
directional or wall collision bits must be rotated by the same amount. Copying
raw flags without rotating them is the classic way to get instanced collision
subtly wrong.

Two things that will bite a naive consumer:

- **`maxMapX/Y` do not bound the resolvable area.** The loaded window is often
  larger than the descriptor grid (a 5x5 mapsquare window against a 32-chunk
  grid is normal), so "inside the loaded window" does **not** imply "has a
  source". Treat an out-of-grid index as a miss, not an error.
- **Rotation has never been observed non-zero in the wild.** The semantics above
  are read off the client's own transform, but a player-owned house is a flat
  1:1 copy, so nothing has exercised rotation end-to-end yet.

---

## 3. Event ring

A lock-free ring at `ringOff` carrying transients that a polled snapshot would
miss (a hit landing, a chat line, a var flipping).

### 3.1 Layout

**`EventRing`** — `head` 0 (u64, monotonic), `slotCount` 8 (== 1024),
`slotMask` 12 (== 1023), `droppedCount` 16, `slots` 32.

**`EventSlot`** (128 bytes) — `seq` 0 (u64), `type` 8 (u32), `bodyLen` 12 (u32),
`body` 16 (112 bytes).

### 3.2 Reading

Each slot carries its own `seq`, which is how you detect overrun.

```
hd = atomic_load_acquire(ring.head)
for (seq = lastSeq; seq < hd; ++seq):
    slot = ring.slots[seq & ring.slotMask]
    s    = atomic_load_acquire(slot.seq)
    if   s == seq:  process(slot); lastSeq = seq + 1
    elif s >  seq:  missed += (s - seq); lastSeq = seq = s   // overrun, resync
    else:           break                                     // writer mid-write, retry later
```

At ~50 publishes/sec with 1024 slots a reader has roughly 17 seconds to drain.
`droppedCount` is an informational writer-side counter; **detect drops via the
`seq` mismatch above**, not from it.

On first attach, initialise `lastSeq = head` to skip history rather than
replaying a full ring of stale events.

### 3.3 Event types

`bodyLen` may be 0 — for those the `type` discriminator *is* the whole signal.
**Unknown types must be skipped, not treated as errors** (they are additive).

| # | Name | Body |
|---|---|---|
| 1 | login state change | `LoginStateChangeBody` |
| 2 | tick | `TickBody` |
| 3 | token refresh fired | `TokenRefreshFiredBody` |
| 4 | token refreshed | `TokenRefreshedBody` |
| 5 | token refresh failed | *(none)* |
| 10 | varp change | `VarChangeBody` |
| 11 | varbit change | `VarbitChangeBody` |
| 12 | varc change | `VarChangeBody` |
| 13 | obj var change | `ObjVarChangeBody` |
| 20 | chat message | `ChatMessageBody` |
| 30 | key input | `KeyInputBody` |
| 40 | action executed | `ActionExecutedBody` |
| 50 | break started | `BreakStartedBody` |
| 51 | break ended | *(none)* |
| 60 | walk arrived | `WalkBody` — *permanently reserved, never emitted* |
| 61 | walk cancelled | `WalkBody` — *permanently reserved, never emitted* |
| 62 | walk failed | `WalkBody` — *permanently reserved, never emitted* |
| 70 | hitmark | `HitmarkBody` |
| 71 | headbar | `HeadbarBody` |
| 72 | spot anim | `SpotAnimBody` |
| 80 | radio group select | `RadioGroupSelectBody` |

Types 10 and 12 share a body shape; **switch on the discriminator** to know
whether `varpId` is a varp or a varc id.

Types 60–62 are allocated but the agent never writes one — there is no
agent-side walker (§4.4). Their numbers stay reserved so they are never reused
for a different event; decode them if you already do, but do not wait on them.

### 3.4 Body layouts

All fields int32 at 4-byte strides unless noted.

| Body | Fields (offset) |
|---|---|
| `LoginStateChangeBody` (8) | `oldState` 0, `newState` 4 |
| `TickBody` (4) | `tick` 0 — the same value as `Snapshot.serverTick` |
| `VarChangeBody` (12) | `varpId` 0, `oldValue` 4, `newValue` 8 |
| `VarbitChangeBody` (12) | `varbitId` 0, `oldValue` 4, `newValue` 8 |
| `ObjVarChangeBody` (20) | `invId` 0, `slot` 4, `varId` 8, `oldValue` 12, `newValue` 16 |
| `ActionExecutedBody` (16) | `actionId` 0, `param1` 4, `param2` 8, `param3` 12 |
| `WalkBody` (8) | `targetX` 0, `targetY` 4 |
| `HitmarkBody` (20) | `targetServerIndex` 0, `targetType` 4 (i8), `hitmarkType` 8, `damage` 12, `cycle` 16 |
| `HeadbarBody` (16) | `targetServerIndex` 0, `targetType` 4 (i8), `headbarType` 8, `value` 12 |
| `SpotAnimBody` (20) | `targetServerIndex` 0 (-1 if world-anchored), `targetType` 4 (i8), `spotAnimId` 8, `tileX` 12 (i16), `tileY` 14 (i16), `plane` 16 (i8) |
| `KeyInputBody` (8) | `key` 0 (u32 Win32 VK code), `isAlt` 4, `isCtrl` 5, `isShift` 6 (u8 each) |
| `TokenRefreshFiredBody` (16) | `expirySec` 0 (u64), `secondsRemainingAtFire` 8 (i64) |
| `TokenRefreshedBody` (16) | `expirySec` 0 (u64), `secondsUntilExpiry` 8 (i64) |
| `BreakStartedBody` (24) | `durationSeconds` 0, *pad* 4, `fatigue` 8 (f64, [0,1]), `risk` 16 (f64) |
| `RadioGroupSelectBody` (20) | `ifaceId` 0, `componentId` 4, `subId` 8, `value` 12, `opcode` 16 |
| `ChatMessageBody` (112) | `msgType` 0, `senderLen` 4 (u16), `textLen` 6 (u16), `buf` 8 |

`targetType`: 0 = player, 1 = npc.

`ChatMessageBody.buf` holds both strings back to back, UTF-8, **not**
NUL-terminated: sender is `buf[0 .. senderLen)`, text is
`buf[senderLen .. senderLen + textLen)`. Combined they fit in 104 bytes.

---

## 4. RPC pipe

`\\.\pipe\BotWithUs_<pid>` — synchronous request/response in MsgPack, plus an
optional server-push stream.

### 4.1 Connection and framing

Open the named pipe in byte mode for read/write. Each message is:

```
[u32 little-endian byte length][msgpack payload]
```

Max frame is 4 MiB; reject anything larger. The length prefix is **not**
included in the count.

**The pipe accepts a limited number of concurrent clients (currently 4).** On
connect, `ERROR_PIPE_BUSY` means every slot is taken — that is a distinct
condition from "no agent", and worth surfacing to the user as such rather than
retrying blindly. Share one connection across your components where you can.

### 4.2 Envelopes

Three shapes, distinguished by which keys are present:

| Shape | Keys | Direction |
|---|---|---|
| Request | `{id, method, params?}` | client → agent |
| Reply | `{id, result}` or `{id, error}` | agent → client |
| Push | `{topic, data}` — **no `id`, no `method`** | agent → client |

`id` is a client-chosen integer echoed back; match replies by it. Requests may
be pipelined. A reader must demultiplex all three shapes, because a push can
arrive between a request and its reply — **key presence, not arrival order, is
what identifies a frame.**

### 4.3 Discovering methods

Call `rpc.list_methods` — the catalog is runtime-discoverable, which is the
right way to target a specific agent build rather than hardcoding this list.

### 4.4 Method catalog

| Group | Methods |
|---|---|
| Meta | `rpc.ping`, `rpc.list_methods`, `rpc.client_count` |
| Licensing | `agent.set_license` |
| Broker | `_debug.subscribe`, `_debug.unsubscribe`, `_debug.publish` |
| Clocks / state | `get_game_cycle`, `get_login_state` |
| Action queue | `queue_action`, `queue_actions`, `get_action_queue_size`, `clear_action_queue`, `get_action_history`, `get_last_action_time`, `set_actions_blocked`, `are_actions_blocked` |
| Session | `set_world`, `change_login_state`, `login_to_lobby`, `login_to_game`, `get_auto_login`, `set_auto_login`, `get_token_refresher`, `set_token_refresher`, `trigger_token_refresh`, `schedule_break`, `interrupt_break`, `get_account_info`, `get_current_world` |
| Capture | `take_screenshot`, `start_stream`, `stop_stream` |
| Scripting / input | `get_script_handle`, `execute_script`, `destroy_script_handle`, `send_key`, `send_click`, `record_move_path`, `click_stats`, `move_stats`, `_debug.inject_click` |
| Interfaces | `get_component`, `get_components`, `get_static_children`, `get_dynamic_children`, `get_interface_tree`, `find_component_at` |
| Variables | `get_varp`, `get_varps`, `get_varc_int`, `get_varcs_int`, `get_varc_string`, `get_varcs_string`, `get_obj_vars` |
| Scene queries | `query_spot_anims`, `query_world_map_elements`, `_debug.entity_facing` |
| Debug drawing | `debug_draw_set`, `debug_draw_set_batch`, `debug_draw_clear`, `debug_draw_clear_all`, `debug_draw_list`, `debug_draw_enable`, `debug_draw_stats`, `debug_draw_probe_pixels`, `highlight_component`, `highlight_entity`, `highlight_tile`, `highlight_area` |

Four gaps worth knowing before you design around them:

- **There is no `get_varbit`.** Varbits decode consumer-side from a backing varp
  plus the varbit definition's bit range, or arrive via event type 11.
- **There is no agent-side pathfinder**, and no Movement group. Nine walker
  placeholders (`walk_to`, `walk_world_path`, `walk_cancel`, `walk_status`,
  `is_reachable`, `find_path`, `find_world_path`, `region_cache_info`,
  `region_cache_clear`) were **removed** on 2026-08-09; they had always been
  inert sentinels that never moved a character. Calling one now returns
  `{id, error: "method not found: <name>"}`. Movement planning belongs to the
  consumer: the sanctioned path is `worldwalker.dll`, whose flat C ABI is
  FFI-shaped for any language, driving the agent through `queue_action` WALK
  (action id 23) clicks. Event types 60/61/62 stay allocated as permanently
  reserved and are never emitted.
- **`change_login_state` is no longer a stub, and can now fail.** It used to
  drain its params and report success without doing anything, so every caller
  believed it had advanced the client. It now performs the transition, keyed on
  `new_state` alone — `20` requires the login screen, `30` requires the lobby,
  and `old_state` is accepted but ignored because the producer reads the live
  state itself rather than trusting a value you sampled some ticks ago. Asking
  for a transition the client is not positioned to make returns an error
  (`not_on_login_screen`, `not_in_lobby`, `unsupported_new_state`) where it
  previously returned success. `login_to_lobby` and `login_to_game` are the
  explicit halves of the same pair. **`set_world` is still a stub**, as are the
  three Capture methods, which answer `{error: "not_implemented"}`.
- **`query_spot_anims` covers the world list only.** It walks the client's
  world/static spot-anim list and answers `{spot_anims: [{id, tile_x, tile_y}]}`,
  capped at 256 rows. Graphics playing *on* an NPC or player are not in that
  list — read those from `spotAnimId` on the snapshot's entity rows, or from
  event type 72 (§2.8).
- **`_debug.entity_facing` is a diagnostic, not a data source.** It exists to
  check the snapshot's `orientation` (§2.8, **v21+**) against the game: in one
  game-thread pass it reads the raw scene-node rotation quaternion and the
  unmasked converted units for the local player and the first 32 loaded NPCs.
  No params. Reply: `{in_world, self: row | nil, npcs: [row]}` with
  `row = {index, tile_x, tile_y, has_quat, qx, qy, qz, qw, known, units}`;
  `units` is `0..16384` when `known` (the snapshot publishes `units & 0x3FFF`)
  and `-1` otherwise, and the `q*` fields are `0` when `has_quat` is false.
  Outside the world it answers `{in_world: false, self: nil, npcs: []}`. Read
  facing from the snapshot, not from this.

#### Click telemetry — `click_stats` and `_debug.inject_click`

The agent writes the client's **two** mouse-click recorders when it dispatches an
action, so the server sees the click pattern a real user produces. The second of
those — the one the client serialises *first* each tick, and the one the server's
activity/AFK timer watches — is **not a telemetry sink**: its serializer never
advances the ring's read head, the client's own consumers (`MiniMenu`,
`InterfaceManager`) do. The agent therefore injects an entry, lets the client
ship it, and retracts it inside a single window where no consumer can observe it.

That sequence can be interfered with by a genuine hardware click arriving on
another thread, and **the interference cannot be fully prevented** — the client's
writer updates its head with a plain non-atomic increment that the agent cannot
change. `click_stats` is how that is made observable instead of silent.

`click_stats` takes no parameters and answers nine counters, monotonic and reset
only by agent reload except `pending`, which is a level:

- **`injected`** — entries that were shipped by the client's serializer **and**
  retracted cleanly. This is the success counter, and the only positive evidence
  the feature did anything.
- **`busy`** — skipped because the ring was not drained (`read != write`). This
  is the correct behaviour when the user is clicking: the agent only injects
  into an empty ring, because the serializer ships the entry at the *read* head,
  so injecting behind a pending entry would ship the user's click instead of the
  agent's and then consume theirs. **But a sustained non-zero rate on an idle
  client is itself an alarm.** This agent's whole premise is that the user is
  idle, so if `busy` keeps climbing with nobody at the keyboard, the client has
  stopped draining the ring — the AFK-logout condition returning by another route.
- **`not_in_world`** — skipped because the client was not in-game or the
  connection was down. Expected, and dominant at the lobby.
- **`no_ring`** — the agent was in-world at state 30 with the connection up and
  the ring still would not resolve. **A fault**, kept separate from
  `not_in_world` on purpose: folded together, the expected population would bury
  it.
- **`lost_claim`** — the agent's atomic claim on a ring slot lost to a genuine
  click. Nothing was written.
- **`collided`** — after shipping, the timestamp at the read head was not the
  one the agent wrote. **Rare and narrow**: the agent claims its slot before
  filling it, and the client's writer re-reads the head before every store, so a
  whole-entry clobber cannot happen. What can happen is one *delayed* store —
  the writer's last head load and its timestamp store are three instructions
  apart, so a thread deschedule spanning the agent's claim, fill and ship lands
  that store on an already-filled slot. What is then left pending carries the
  **agent's** coordinate with a foreign timestamp, so this counter is a
  phantom-click risk rather than a duplicated user click. Non-zero means look.
- **`raced`** — the ring's write head moved *at all* during the agent's window,
  **including when the retract then succeeded**. See the note below before
  acting on this one.
- **`no_ship`** — the serializer could not be called (null vtable or slot). The
  entry was still retracted, but nothing went on the wire.
- **`queue_occupied`** — a click was **refused** because the previous one had not
  been pumped yet. This is the single-slot *queue*, one stage before the ring, so
  it is **not** the same fact as `busy`: `busy` means the client had not drained
  its ring, while this means the **caller outran the tick**. It cannot be reached
  by the agent's own action dispatch, which queues at most one click per tick and
  drains it in the same tick, so a non-zero value means someone is driving
  `_debug.inject_click` faster than the game loop.
- **`pending`** — entries left in the ring immediately after the most recent
  attempt, `(write - read) mod capacity`. A level, not a cumulative count, and
  the only one that is not monotonic. **A correct retract leaves this at 0**, so
  it is the direct check that the retract worked rather than an inference from a
  later injection succeeding. It is sampled on *every* attempt, including the
  ones that skip — so when `busy` is climbing on an idle client, this is the
  field that tells you whether the ring really is backing up.

**`collided` and `raced` are different facts, and a consumer that watches only
one of them will draw the wrong conclusion about how often the race is live.**

- `collided` is a *detection*: the agent saw a genuine writer take its slot and
  correctly declined to retract.
- `raced` is *exposure*: the head moved inside the window, and **the agent
  cannot tell which side of the race the entry landed on.**

**A non-zero `raced` is unresolved exposure, not a clean bill of health.** It is
the rate to argue about, and it is the only signal that a torn genuine entry may
be pending.

That wording is deliberate, because the failure it covers is worse than a lost
telemetry entry. The client's own writer **re-reads the ring's write head before
every one of its six field stores and never caches the slot.** So a head change
landing mid-writer does not merely race the entry — it **tears it across two
slots**, and where the split falls decides how bad it is. Split after the first
store and the new slot gets a **real coordinate with a stale button id**. Split
later — after x and y have already gone to the old slot — and the new slot keeps
**stale coordinates too**, leaving a pending click at a wholly stale position.
Either way the game consumes it. The counters in that case read
`raced: 1, injected: 1` with everything else zero — **indistinguishable from the
benign interleaving.**

So: treat `injected` as the health signal; `collided`, `lost_claim`, `no_ring`
and `no_ship` as faults; `not_in_world` as an ordinary skip; `busy` as ordinary
*unless* it is sustained on an idle client; `queue_occupied` as a statement about
the **caller**, not the agent — harmless from a test driver, meaningless from the
action path, which cannot produce it; and `raced` as a rate meaning "the
race was live and the outcome is unknown". Do not read a non-zero `raced` with a
clean `collided` as evidence that nothing went wrong — `collided` is the narrow,
detectable corner of that race, and `raced` is the rest of it.

`_debug.inject_click({x, y, scale_milli?})` drives one injection with
caller-supplied coordinates and answers `{accepted: bool}`. `scale_milli` is the
client-pixel to interface-space factor times 1000, default 1000.

**`accepted` means the work was posted to the game thread, and nothing more.**
The reply is written before the queue attempt happens, so it does **not** mean a
click was queued and certainly not that one was injected — a caller going faster
than one call per tick gets `accepted: true` for a click the pending slot then
refuses, which surfaces only as `queue_occupied` on `click_stats`. **Assert on
`injected` advancing, never on `accepted`.** Measured before that counter
existed: 20 rapid calls, 12 injections, every counter zero.

**It bypasses the world-to-screen projection and the action-queue gates;
everything after `QueueActionClick` is the production path verbatim** — the same
pending-click slot, the same game-thread pump running after the client's own
tick, the same injection, serialization and retraction. It is not a test-only
reimplementation, and a scenario built on it exercises the real mechanism.

The gates it skips are worth naming: production reaches that point only through
the action queue's own tick, which additionally requires a non-empty queue, no
active break, the queue not blocked, and the dispatch itself to have succeeded.
**Notably this will inject during a scheduled break**, which the real path never
does.

`x` and `y` are **required**. They are not defaulted, because the client's
reader clamps anything `<= 0` to `0` — so an omitted coordinate would produce a
click at `(0, 0)` with no error, precisely the symptom this mechanism exists to
remove. Omitting either answers
`{accepted: false, error: "x_and_y_required"}`. The coordinates are supplied rather than projected so that a test
cannot fail for an unrelated reason: a target that projects behind the camera is
*correctly* suppressed by the production policy, which would make a projection-
driven assertion flaky rather than meaningful.

It moves no character and mutates no game state. The `_debug.` prefix is
wire-load-bearing — the dispatcher's auto-tap exclusion keys off those literal
seven bytes — but unlike the three broker methods this is not a broker call.

Neither method moves `kProtocolVersion`: both are additive over the RPC pipe and
neither touches `Snapshot`.

#### Move telemetry — `move_stats`

The agent also writes the client's **normal move ring** — the mouse-movement
samples the server's movement telemetry watches — and moves the client's own
cursor. `move_stats` is the observability for that path, the movement analogue
of `click_stats` above. It takes no parameters and answers thirteen fields.

Nine integer counters, monotonic and reset only by agent reload, except
`pending` which is a level:

- **`samples`** — move samples written into the ring.
- **`batches`** — flushes of a queued path.
- **`orphaned`** — a path was queued with **no click to flush it**. The client
  drains its move ring only when a click is pending or 2000 ms have passed, so
  an unpaired path can neither ship nor be left sitting in the ring.
- **`no_scale`** — the client-pixel to interface-space factor was unresolvable.
- **`queue_occupied`** — the pending slot already held work.
- **`dropped`** — samples discarded rather than written.
- **`bad_head`** — the ring's head did not read plausibly.
- **`not_drained`** — entries still in the ring after the flush. **Not cosmetic
  residue**: this is the phantom-hover exposure, and it is the one counter here
  whose non-zero value means the client is being told something false.
- **`pending`** — samples currently queued. A level, not a total.

Four doubles, each **read back out of the client after the write**:

- **`cursor_raw_x`**, **`cursor_raw_y`** — the cursor as the client's own scene
  pick reads it.
- **`cursor_iface_x`**, **`cursor_iface_y`** — the cursor as interface hover
  reads it.

**Assert on the cursor fields, not on `samples`.** Every counter above is an
agent-side tally that a write happened — which stayed true throughout a
coordinate type-pun bug while every coordinate actually reaching the server was
0. The four doubles are read back through the same offsets the client's scene
pick and interface hover use, so they move when the behaviour moves and cannot
be satisfied by a write that landed in the wrong place or the wrong format.

Additive over the RPC pipe — does **not** move `kProtocolVersion`.

#### Debug drawing

The agent retains a set of draw commands and renders them over the client. The
store is **not** in shared memory and nothing about it moves `kProtocolVersion`
— it is entirely additive over this pipe.

Three properties shape the API, and a consumer that assumes otherwise will get
surprises:

- **Commands are keyed, not handled.** Every command carries a caller-chosen
  string key of 1–47 bytes, and setting the same key again replaces rather than
  appends. There is no handle to leak and nothing to free; redrawing
  `"target-box"` every tick is idempotent — including at slot capacity, where
  replacing a `text` or `poly` key reuses the slot it already holds rather than
  competing for a free one.
- **Keys are scoped to the connection.** Two clients may both use
  `"target-box"` without colliding, `debug_draw_clear` only ever removes your
  own, and **when a connection closes the agent drops everything that
  connection drew.** A client that dies mid-script leaves nothing on screen.
- **TTL is mandatory.** Omitting `ttl_ms` gives you 3000 ms. `ttl_ms: 0` means
  "until replaced, cleared, or my connection closes" — it is not a way to draw
  something permanent from outside a live session.

| Method | Params | Returns |
|---|---|---|
| `debug_draw_set` | `{key, kind, space?, …geometry…, color?, thickness?, filled?, closed?, z?, ttl_ms?, text?, label?, value?, decimals?, font?}` | `{key}` |
| `debug_draw_set_batch` | `{items: [ …as above… ]}`, at most 256 | `{count, dropped, error}` |
| `debug_draw_clear` | `{key}` and/or `{keys: […]}` | `{removed}` |
| `debug_draw_clear_all` | `{scope: "mine" \| "all"}`, default `"mine"` | `{removed, scope}` |
| `debug_draw_list` | `{scope?, offset?, limit?}` | `{total, offset, returned, items: […]}` |
| `debug_draw_enable` | `{enabled}`, omit to read | `{enabled}` |
| `debug_draw_stats` | — | see below |
| `debug_draw_probe_pixels` | `{x, y, w, h, color, source?}` **or** `{key, color, source?, inset?}` | `{region, matched, total, all, occluded, exact?, occluder?}` (exact: source 1; occluder: source 0) |
| `highlight_component` | `{iface, comp, space?, color?, thickness?, filled?, z?, ttl_ms?, key?, label?, font?, value?, decimals?}` | `{key}` |
| `highlight_entity` | `{npc \| player \| self, w?, h?, plane?, …styling…, key?, label?}` | `{key}` |
| `highlight_tile` | `{x, y, plane?, …styling…, key?, label?}` | `{key}` |
| `highlight_area` | `{x, y, w, h, plane?, …styling…, key?, label?}` | `{key}` |

`kind` is one of `line`, `rect`, `ellipse`, `poly`, `text`, `component`, and
the geometry keys it reads depend on it: `line` takes `x1, y1, x2, y2`; `rect`
and `ellipse` take `x, y, w, h` (one command with a `filled` flag, not two
commands); `text` takes `x, y` and a UTF-8 `text`; `poly` takes a flat
`points: [x, y, x, y, …]` of 2–32 pairs; `component` takes `iface, comp` and no
geometry at all.
**Two further kinds exist and are deliberately not spellable as `kind`.**
`entity` and `tile` appear in `debug_draw_list` output, but `debug_draw_set`
rejects them with the ordinary `unknown kind` error. They carry semantics no
geometry key can express - which list an index belongs to, which plane a
footprint sits on - so they are reached only through `highlight_entity`,
`highlight_tile` and `highlight_area`, where every parameter is named for what
it means and a wrong one is reported by name. Spelling them here would accept
`{kind: "entity", x: 3, y: 4}` and draw something nobody asked for.

`highlight_component` generates a key when you omit one, formatted
**`comp:<iface>:<comp>`** — for example `comp:1473:5`. Decimal, no padding. That
format is part of the contract: without it a caller who omitted `key` has no way
to name the highlight again in order to clear it. The reply always echoes the key
actually used, so reading it back is the reliable route.

**`highlight_component` takes `z`, and on this wire version that matters.** The
styling parameters are resolved for every kind, so a highlight honours `z`,
`space`, `color`, `thickness`, `filled` and `ttl_ms` exactly as a `rect` does —
and since `z` now decides paint order, it is the only way to control which of
several overlapping highlights is visible where they cross. That is precisely
the "six highlights on screen" case labels exist to serve, so do not read the
absence of a rectangle in the params as an absence of styling.

It also takes an optional **`label`** (and a `font` for it), drawn against the
rect the agent resolved — above the box, or just inside the top edge when that
would leave the surface. The caption has to travel with the highlight precisely
because the caller never learns where the rect landed: with six highlights on
screen there is no other way to tell them apart. Note the parameter is `label`,
not `text`; sending `text` to a `component` is rejected with
`component names its caption "label", not "text"` rather than silently ignored.
`debug_draw_list` reports a component's label in the same `text` field a `text`
command uses.

That last one is the point of the semantic kinds. **A component's on-screen
rect is recomputed by the client on every layout pass**, so a highlight that
stored a rectangle would drift the moment the UI relaid out, the window
resized, or a scrollpane moved. The agent stores the `(iface, comp)` pair and
re-resolves the rect on the game thread once per tick. `debug_draw_list`
reports the last resolved rect as `rect` alongside a `resolved` flag; a
renderer skips an unresolved command rather than drawing a stale one.

### World space

**`"world"` is implemented.** Through the first two phases of this feature it
was accepted and then rejected with
`world space requires projection - not yet implemented`; **that message is now
unreachable**, and a consumer asserting on it will fail. No method name
changed, so `tools/check_protocol_doc.ps1` stays green across this change - it
compares method names only. This paragraph is the record.

`"screen"` (client-area pixels, origin top-left) remains the default.

**Coordinates are integers everywhere** - this wire has no float field and the
producer's writer has no float32 encoder, both deliberately. World coordinates
are fixed-point **`tile * 256 + subtile`**. Note that the client's own scene
graph uses 512 units per tile, exactly twice this; the agent owns that
conversion in one function and nothing on the wire ever sees the 512.

**A world position is resolved to a screen position once per tick on the game
thread** - the same retained/re-resolved contract `component` has, and for the
same reason: the camera moves, so a command that stored a pixel would be wrong
the moment the player turned. `debug_draw_list` reports the projected rect in
`rect` alongside `resolved`, exactly as it does for a component.

**In world space a POSITION is a world coordinate and an EXTENT is tiles.** A
world `rect` or `ellipse` is a ground footprint, not a screen-sized billboard.
`line` projects both endpoints. `text` projects its anchor. **`poly` is
rejected** with `world space does not support poly` - its points live in a side
slot with no room for a projected copy; send lines instead. `component` is
rejected too: it is already on screen and has no world position.

A projected footprint is reported as the **screen bounding box** of its four
projected corners, not as the perspective quadrilateral, so it over-covers at
oblique camera angles. It shrinks correctly with distance.

**`plane` (0..3) is accepted only in world space**; on a screen-space command
it is refused with `only world space takes a plane` rather than ignored. It is
currently honoured **by refusal**: the agent's only ground-height source is the
local player's own elevation, so a command naming a different plane resolves
`unavailable` rather than being drawn convincingly wrong at the player's floor.
For the same reason a world position on a slope or a staircase is drawn at the
player's height - correct horizontally, off vertically. An `entity` highlight
is exempt, because an entity carries its own height; entity highlights are
exact everywhere, which makes them the recommended path.

#### `project` - four outcomes, not a bool

Every world command's `debug_draw_list` row carries a **`project`** field
alongside `resolved`:

| project | resolved | meaning |
|---|---|---|
| **n/a** | - | nothing was projected. Every screen-space command, **including `highlight_component`** - a component's rect is re-read from the live interface tree, not projected, so no camera is involved and no projection outcome describes it |
| **ok** | true | in front of the camera and inside the scene viewport |
| **off_viewport** | true | in front of the camera, outside the viewport rect. The coordinates are **real and clampable** - negative values are normal - and are safe to draw an off-screen edge marker from |
| **behind_camera** | false | no usable screen position exists: at or behind the camera plane, a NaN in the transform, or the near-clip blow-up |
| **unavailable** | false | no scene, no window, or a plane the agent cannot height - pre-login, mid-teardown, another floor |

<!-- The values above are deliberately bold rather than backticked, and so is -->
<!-- the bare "self" in the auto-key table further down. -->
<!-- tools/check_protocol_doc.ps1 reads every backticked lowercase identifier -->
<!-- out of any table row in section 4.4 and diffs it against the producer's -->
<!-- g_methods[], so a backticked `ok` or `self` here is reported as an -->
<!-- undocumented RPC method and fails the build gate. -->
<!-- RULE: inside a section 4.4 table, a backticked bare lowercase word must -->
<!-- be an actual RPC method name. Anything else - an enum value, a keyword, -->
<!-- a parameter spelling - goes in bold. Words containing ':' or '<' are -->
<!-- safe either way, which is why `npc:<index>` needs no special handling. -->

`resolved` is one bit and cannot tell the last three apart. **A consumer that
treats `off_viewport` and `behind_camera` alike will draw a marker clamped to a
screen edge for something behind the player's head**, which is the specific
mistake this field exists to prevent.

When the result is not usable the agent does **not** put a sentinel in `rect`.
The coordinate check refuses `INT32_MIN` rather than clamping it, and a command
that drops out of `resolved` has its rect **zeroed**, so `rect` reads
`[0, 0, 0, 0]` whenever `resolved` is `false` - including for a marker that was
tracking correctly and then went behind the camera. That case is worth naming:
clearing only the flag would leave the last good rect in place, and a consumer
reading it would get a stale, plausible, wrong rectangle at the exact moment
its marker became invalid.

**`resolved: true` does not promise a paintable area.** A zero-extent rect is a
legitimate answer - a footprint far enough away that all four projected corners
round to one pixel, and `text`, whose resolved extent is zero by design - so a
world command can report `resolved: true` with `rect: [x, y, 0, 0]`. The
position is real; there is simply nothing to fill. A consumer that needs an
area to draw into must check the extent, not only the flag.

#### The named world highlights

`highlight_tile` draws exactly one tile and **refuses `w`/`h`**
(`use highlight_area`) rather than dropping them; `highlight_area` requires
both, in **tiles**. `highlight_entity` takes exactly one of `npc`, `player` or
`self` - two is an error and none is an error, because "which wins" is not a
rule a caller can guess from a reply that succeeded. `self` carries no index:
the local player's list slot is re-resolved every tick, so it survives a world
hop that would leave a stored index pointing at whoever took the seat. `w`/`h`
on an entity are its footprint in tiles, default 1x1.

**The tile helpers speak TILES; raw `space: "world"` primitives speak
sub-tiles.** `highlight_tile {x: 3200, y: 3200}` stores `819200`. The handler
is the single place the two meet, so nothing downstream ever sees two units.

#### Auto keys

When you omit `key`, one is generated. Decimal, no padding. Same contract as
`comp:<iface>:<comp>`: without a documented format, a caller who omitted `key`
has no way to name the highlight again in order to clear it.

| call | auto key |
|---|---|
| `highlight_tile` | `tile:<x>:<y>:<plane>` |
| `highlight_area` | `area:<x>:<y>:<w>:<h>:<plane>` |
| `highlight_entity` npc | `npc:<index>` |
| `highlight_entity` player | `player:<index>` |
| `highlight_entity` self | **self** (no index; see below) |

**The key names every field that distinguishes two highlights, and that is not
cosmetic.** A key is an identity and a set under an existing key *replaces* it,
so any distinguishing field the key omits is a field two highlights can
disagree on while colliding - and the loser disappears with no error, because
replacing is a legitimate operation. Plane is therefore in the tile key (the
same x/y on two floors is two tiles) and the extent is in the area key (two
areas sharing a corner at different sizes are two regions).

An entity key names the entity and nothing else, deliberately: two highlights
on one npc with different footprints *are* the same highlight restyled, and
replacing is the right answer. `self` carries no index because it does not have
one - the local player's slot is re-resolved every tick.

Colours are `0xAARRGGBB` packed into an unsigned integer. **`color` must fit
32 bits** — a signed 32-bit value is accepted too, so a Java caller may send
`0xFF00FF00` as the int `-16711936` — and anything outside that range is
rejected with `color must fit 32 bits as 0xAARRGGBB` rather than masked.

**`z` decides paint order, and it is honoured.** Higher `z` paints later, so it
paints on top; the default is 0. Commands sharing a `z` keep a stable order that does
not change between two presents of an unchanged store — but it is
*not* a documented order, so do not rely on which of two equal-`z` commands wins.
`z` is a **signed 16-bit** value: outside −32768..32767 the call is rejected with
`z must be -32768..32767`. It is not clamped and not narrowed. (Through phase 1
it *was* narrowed — `z: 100000` became `-31072` and silently reordered the
paint.)

### Text: `font`, and `value` for numbers

`text` commands — and a `component` highlight's `label` — take an optional
**`font`**, which names a style rather than a size: `"normal"` (the default),
`"small"`, `"large"`, `"heading"`. An unknown name is rejected with
`unknown font (want normal, small, large or heading)`. The producer owns the
size table deliberately, so the set of GDI font objects the agent holds is a
compile-time constant rather than something a script can grow.

**There is no float anywhere on this wire, and that includes text.** To label a
distance or a percentage, send a scaled integer and say what you scaled it by:

    {kind: "text", x: 40, y: 160, value: 1234, decimals: 2}   ->  "12.34"
    {kind: "text", x: 40, y: 160, value: -5,   decimals: 2}   ->  "-0.05"
    {kind: "text", x: 40, y: 160, value: -4200}               ->  "-4200"

`decimals` defaults to 0 and must be 0..9. The integer part is zero-padded, so
nothing ever renders as a bare `.05`. **`value` is a label source for
`highlight_component` too** — the same three spellings feed the same buffer, so
`{iface, comp, value: 1234, decimals: 2}` captions a highlight `12.34` without a
`label`. `decimals` without `value` is an error,
and so is combining `value` with `text` or `label` — these are three spellings
of one payload and exactly one is legal per command, because "which one wins"
is not a rule a caller can guess from a reply that succeeded.

A caption on a kind that draws none (`rect`, `ellipse`, `line`, `poly`) is
rejected with `only text and component commands take text, label or value`
rather than stored where nothing will read it. **`font` is rejected on those
same kinds**, with `only text and component commands take a font`. Through the
first cut of this feature it was not: a `font` on a `rect` was accepted, stored
and then ignored, while a `label` on the same `rect` was refused — two spellings
of one mistake getting opposite answers. If you build commands from a shared
style object, strip `font` for the shapes.

**Two coordinate spaces meet here, and only one of them is pixels.** Screen-space
draw commands are in the render surface's own pixels. A **component rect is
not** — the client lays its interfaces out in a smaller logical space and scales
that to fill the viewport, so the agent scales a resolved component rect by
`surface / layout` before drawing it. Measured on a 4K display at 225% scaling:
the ratio is identical in x and y at every window size and settles at the display
scale once the window is large enough to stop the layout clamping at its
~1024x600 floor. Consumers never see this — `debug_draw_list` reports the rect in
layout units, exactly as `get_component` does — but anyone comparing a reported
rect against a screenshot needs to know the factor exists.

**A probe region can be addressed BY KEY instead of by coordinates.**
`{key, color, source?, inset?}` probes wherever the named command's projection
actually put it, using that command's last resolved rect shrunk by `inset` on
all four sides (default 0). This exists because a world marker lands wherever
the camera says and no test can know that in advance - the scenario runner
matches exact values and cannot carry a number from one step into the next, so
a literal `x/y` would be unwriteable. `key` and `x/y/w/h` together are refused
rather than ranked. The key must name a **world** command this connection owns
that is currently resolved; anything else returns
`no resolved world command under that key`, which is an error naming the cause
rather than a silent `0 of 0` that reads like a renderer fault. A Component's
rect is deliberately not addressable this way: it is in interface layout space,
not surface pixels, and the two are not distinguishable once returned.

**Every reply carries `region`** - the `[x, y, w, h]` actually sampled.
Redundant when you passed coordinates, and the whole point when you passed a
key: without it a failed key probe says "0 of 400 matched" and nothing about
where it looked.

**`debug_draw_probe_pixels` is a verification surface, not a drawing one.** It
reads back what is actually on screen inside the overlay's target client rect and
counts pixels matching `color`, so a test can assert that pixels *reached the
screen* rather than that a counter moved. `source: 1` samples the overlay's own
surface instead, which splits "did it draw" from "did it reach the screen" — the
two halves have genuinely different causes, and separating them is what located
both renderer bugs found during phase 1. Coordinates are client-space, so a
mismatch also catches the overlay being aligned to the wrong window.

**The two sources answer the same question.** `matched` is an RGB comparison
against the premultiplied colour and `total` counts the region clipped to the
overlay surface, identically on both sides, so a surface count and a screen
count of one drawing are two measurements of one quantity and may be compared.
They were not comparable before phase 2: the surface compared all 32 bits while
the screen compared 24 with alpha masked, and `total` was clipped on one side
only, so the difference between them read as a rendering defect when it was a
difference between two questions.

The alpha half is reported separately rather than dropped. **`source: 1` also
returns `exact`**, a full 32-bit match including alpha — the check that catches
correct RGB written at alpha 0, which is what every GDI primitive does on a
per-pixel-alpha layered surface. A desktop capture carries no usable alpha, so
the screen source **omits `exact` entirely** rather than reporting `matched`
under that name: a number that looks measured and is not is worse than a
missing one.

**`occluded` is the field that decides whether a screen result means anything,
and `occluder` says what is responsible.**
A screen capture reads whatever is frontmost, so any window over the client
makes a working overlay report zero matching pixels. `occluded: true` means
**unknown**, never "did not draw" — assert `occluded: false` alongside every
screen expectation. The check asks whether the *game window* is on top, not
whether *some window of the client's process* is: through phase 1 it compared
process ids, and that is blind to the likeliest occluder of all, because a Debug
build's agent console lives in the client's own process. A console over the
capture region passed the old test, so the probe answered "not occluded, zero
pixels" — the worst available answer, because it points at the renderer.

**`occluder` carries the covering window's class name** (empty when clear,
absent entirely for `source: 1`, which nothing can cover). It is there because
`occluded: true` on its own is a dead end for whoever reads a run record, and
finding out cost three harness runs and a screenshot the first time.

**It is non-empty whenever `occluded` is true, without exception.** Five
conditions occlude without a nameable window — the region lies entirely off the
overlay surface (`(region off surface)`), part of it is on no monitor
(`(off-screen)`), no window is at the sampled point (`(no window at point)`),
there is no tracked game window (`(no target window)`), or the class could not
be read (`(unnamed window)`) — and each reports a bracketed reason rather than
an empty string, so "occluded" and "named" are the same question. **Gate on the
boolean anyway**: this field is the message, not the predicate.

`(region off surface)` is what a caller gets for coordinates that miss the
overlay entirely — a clipped region of zero pixels. It used to answer
`{matched: 0, total: 0, occluded: false}`: a clear view of nothing, which reads
as a renderer that drew nothing. Fix the coordinates, not the renderer.

`(off-screen)` is the one worth knowing about, and it is a REGION test, not a
point test — all four corners of the sampled region must be on a display.

A client moved entirely off every display still satisfies `WindowFromPoint`,
because an off-screen window keeps its place in the window manager's coordinate
space, so the window test alone reported `occluded: false, matched: 0` over a
frame the surface probe showed as correctly drawn — accusing the renderer of a
window-placement problem. A client collapsed to its title bar does the same.

The PARTIAL case is the one to design around, because it is the one that gets
believed. With the client dragged partly past a monitor edge — no change to any
probe coordinate, just an ordinary window drag — a region can have its centre on
a display and its edge off it. Measured at x = -200: `matched: 3200` of
`total: 5000`, which is exactly the fraction still on the desktop, and under a
point test it came back `occluded: false`. A zero count over a clear view looks
suspicious and gets investigated; a 64% count over a clear view looks like
partial coverage or a clipping bug and gets believed. **Do not treat a partial
`matched` as evidence about the renderer without checking `occluded` first.**

**Expect it to fire often.** A Debug agent always has a console, in the client's
own process, over the render view — and a Debug build is the only kind the
harness injects. So in harness runs occlusion is the DEFAULT condition, not an
occasional desktop accident. Structure a scenario accordingly: assert the thing
it is actually about against `source: 1`, which cannot be occluded, and put the
screen assertion after it, so a desktop condition fails at a step that is
unmistakably the screen check rather than masking the real claim.

Every cap a call can hit is a hard error rather than a silent truncation, but
**how you are told depends on which call you made, and only some of them touch
the counter**:

- `debug_draw_set` and `highlight_component` answer `{id, error}`. Exceeding the
  512 retained commands, the 64 text slots or the 64 polyline slots also
  increments `debug_draw_stats.dropped`. A **key longer than 47 bytes**, an
  out-of-range coordinate and a malformed command are rejected before the store
  is touched at all, so they error **without** incrementing `dropped` — do not
  use that counter to detect them.
- `debug_draw_set_batch` has three outcomes, and the dangerous one is that an
  error reply does **not** mean nothing drew. It answers `{id, error}` — a real
  envelope-level failure — in two cases. When the `items` array holds **more than
  256** entries (`debug_draw_set_batch accepts at most 256 items`) the handler
  returns before touching the store, so nothing is applied. But when an item is
  **structurally malformed**, the items *before* it have already been applied one
  by one; reading stops there and the call errors, leaving the store **partially
  written** with no indication of how far it got — and the handler errors before
  it writes its reply map, so the tally it had accumulated dies with the call: an
  aborted batch carries no `count` at all, and that number is not recoverable
  from the reply. Re-read the store with `debug_draw_list` rather than assuming.
  The third outcome is per-item *validation* failure, which is not an envelope
  error at all: those answer a normal `{id, result}` with
  `{count, dropped, error}` — the number applied, the number refused, and the
  **first** error string only (`nil` when none). A batch refused item-by-item
  still looks like a successful call, so a client that checks only for
  `{id, error}` will read a 256-item batch that drew nothing as a success. Check
  **both** the envelope and `dropped`, and treat an envelope error as *unknown*
  store state rather than a clean one.

**There are TWO coordinate bounds and they are not interchangeable.** A
world-space command is checked against both, at different points in its life.

**Screen bound: +/-1048576 (2^20).** It applies to every screen coordinate a
caller sends, and to every screen coordinate the projection produces a tick
later. Rect/ellipse extents are at most 16384; `thickness` is 1-64 and defaults
to 1; `w` and `h` must both be > 0. This is the check that makes a projected
point safe to hand back: **a point behind the camera projects to `INT32_MIN`,
which is rejected rather than clamped**, so you learn the point is not on
screen instead of getting a confident line to nowhere. 2^20 is already about
250x any real screen dimension - its looseness is all the slack a projected
coordinate gets before it is refused.

**World bound: +/-8388608 (2^23), i.e. 32768 tiles.** It applies to world
coordinates only. It is *derived*, not chosen, and the derivation is the thing
to preserve if any of its inputs move:

> A caller gets world coordinates out of this agent's own snapshot, and every
> absolute world tile the snapshot publishes - `NpcEntry.tileX`,
> `PlayerEntry`, `LocationEntry`, `GroundItemEntry`, `ProjectileEntry` - is an
> **`i16`**. So the widest tile magnitude any consumer can be holding is
> `|INT16_MIN|` = 32768, and a bound below that would refuse a coordinate this
> same producer published one tick earlier. At 256 sub-tiles per tile that is
> 2^23.

Two cross-checks confirm the derived number rather than producing it. It
**covers the map**: the instance chunk descriptor (section 2.10) packs a source
chunk Y in 11 bits and a chunk is 8 tiles, so the largest world tile the client
itself can name is 2048 x 8 = 16384 - half the bound, 2x headroom. And it
**survives float32**: the projection takes floats, a wire coordinate is doubled
into the client's 512-per-tile scene units on the way in, and 2^23 x 2 = 2^24
is exactly the largest magnitude at which float32 still represents every
integer. One power of two more and the far corner of the map would quantise,
putting a marker up to a sub-tile off with nothing to indicate it. That is why
the bound is not rounded up "to be safe".

**Do not merge them.** Raising the screen bound to fit a world coordinate
widens by 8x the exact check that catches a bad projection, and makes one
symbol mean two quantities with two derivations. The agent keeps them as
`kMaxDrawCoord` and `kMaxWorldCoord`.

A world rect's far corner is checked as a world coordinate in its own right,
so there is no separate world extent constant - the question "is this extent
legal" is exactly "does this corner exist". The **resolved** screen rect is
then subject to the screen extent cap, and a footprint that projects larger
than that simply fails to resolve (`resolved: false`) rather than being drawn
wrong.

One cap is **not** an error, because it is not reachable from a single call:
`kMaxResolveTargets` (64) bounds how many component highlights the agent
re-resolves per tick. Past it, the surplus is resolved on a following tick —
collection rotates, so nothing is starved — and `debug_draw_stats.resolve_overflow`
counts the ticks on which that happened. A non-zero value means some highlight
geometry is lagging the game by a tick or more.
Each `debug_draw_list` item carries `key`, `kind`, `space`, `plane`, `project`,
`color`, `thickness`, `z`, `filled`, `closed`, `geom`, `resolved`, `rect`,
`ttl_ms`, `font` and `text`. `text` is populated for `text` commands and for
the label on a `component`, `entity` or `tile` highlight, and is empty for
every other kind. `plane` is 0 for screen-space commands. `project` is `n/a`
for screen-space commands and one of the four world outcomes otherwise - see
"World space" above, and read it rather than collapsing it into `resolved`.

`debug_draw_list` is **paged** — `limit` defaults to and is capped at 128 rows,
which is what keeps the one response on this wire that could otherwise approach
the 4 MiB frame limit from doing so. Use `offset` with the returned `total` to
walk the rest.

`debug_draw_stats` is deliberately over-instrumented, because the failure mode
this feature has to avoid is an overlay that has silently died while looking
perfectly healthy. Alongside `count`, `capacity`, the slot gauges, `dropped`,
`resolve_failures`, `resolve_overflow` and `version`, it reports `backend`
(which renderer is actually live), `backend_ready`, `frames`,
`backend_presents`, `frames_undelivered`, `skipped_frames`, `last_present_us`,
`present_failures`, `last_present_error` and `surface: [w, h]`.

The three fields worth asserting on are **`present_failures`** (uploads that
failed — the health check, and falsifiable: break the present and it climbs),
**`has_presented`** (the renderer has drawn at least one frame), and
**`resolver_live`** (the game thread has returned resolved geometry at least
once — the positive trace that the per-tick refresh is running, since an
unresolved rect is also what you would see if it never ran at all).

**Do not build a check on `frames == backend_presents`.** They answer different
questions: `frames` counts what the pump handed to the renderer, and
`backend_presents` counts uploads that actually reached the screen. A frame
whose dirty region is empty is processed without uploading anything, so they
differ in normal operation. `frames_undelivered` is their difference, reported
as a diagnostic rather than a verdict.

### 4.5 Broker topics

Subscribe with `_debug.subscribe({topics: [...]})`; matching frames then arrive
as push envelopes. Only subscribed connections receive pushes, so a client that
never subscribes is unaffected.

The broker is topic-agnostic — it relays whatever is published. Two topics are
in use:

- **`rpc.tap`** — emitted by the agent for every processed RPC.
  `{method, id, t_us, dur_us, params, reply}`, where `params`/`reply` are
  verbatim MsgPack. `_debug.*` calls are excluded so subscribe traffic doesn't
  echo.
- **`script.context`** — published by scripting hosts, not the agent.
  `{script, connection?, t_us, kind, ...}` with `kind` one of `state`, `trace`,
  `annotation`.

The `_debug.` prefix is itself part of the contract — the auto-tap exclusion
matches those literal seven bytes.

Any client may publish to any topic via `_debug.publish({topic, data})`, which is
how a host surfaces its own state to a debugger.

---

## 5. Interface component categories

The component map returned by `get_component` / `get_interface_tree` /
`find_component_at` carries a `category` field. A component also has a raw type
byte, but that byte's meaning **drifts between game builds** — `category` is the
stable vocabulary and the one to write logic against.

| Value | Category | | Value | Category |
|---|---|---|---|---|
| 0 | Unknown | | 8 | List |
| 1 | Layer | | 9 | Input |
| 2 | Box | | 10 | Combo |
| 3 | Text | | 11 | Media |
| 4 | Sprite | | 12 | Tooltip |
| 5 | Model | | 13 | CrmView |
| 6 | Button | | 14 | Table |
| 7 | Divider | | 15 | Cutscene |

Append-only — existing values are never renumbered, so an unknown value from a
newer agent should degrade to "unknown", not fail.

---

## 6. Writing a consumer — checklist

1. Find the client pid; open `Local\nxt_snapshot_<pid>`.
2. Validate magic, then version. **Refuse on version mismatch.**
3. Read `frontIdx` with acquire, bulk-copy that buffer, parse the copy.
4. Initialise `lastSeq = ring.head` so you skip history.
5. Drain the event ring each frame using the `seq` protocol in §3.2.
6. Open the RPC pipe only if you need to mutate or query; handle
   `ERROR_PIPE_BUSY` distinctly, and demultiplex pushes from replies by key
   presence.
7. Pace game logic on `serverTick`, never on `publishSeq`.
