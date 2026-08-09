# BotWithUs agent wire protocol — v19

How to read live RuneScape 3 state out of the BotWithUs agent, and how to drive
it, from **any language**. This is the normative description of the bytes; the
headers in this directory are the same contract expressed in C++.

Everything here is little-endian x64. Structs use natural alignment with no
packing pragmas, and every offset below was generated from the headers rather
than written by hand.

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

`kProtocolVersion` is **19**.

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
2. `version` == `19`. Mismatch → refuse (see §1).
3. `headerSize` == 64 and `snapshotSize` == 365744 as a sanity check.

### 2.2 Region geometry

| Constant | Value |
|---|---|
| `kMagic` | `0x5354584E` |
| `kProtocolVersion` | 19 |
| `sizeof(SharedHeader)` | 64 |
| `sizeof(Snapshot)` | 365744 |
| snapshot stride (padded to 64B) | 365760 |
| Snapshot[0] offset | 64 |
| Snapshot[1] offset | 365824 |
| Event ring offset | 731584 |
| Event ring size (padded) | 131136 |
| Total region size | 862720 |

Do not hardcode these blindly — the header carries `snapshotOff0`,
`snapshotOff1`, `ringOff` and `ringSize` for exactly this reason. Prefer reading
them.

### 2.3 `SharedHeader` (64 bytes, at offset 0)

| field | off | size | notes |
|---|---|---|---|
| `magic` | 0 | 4 | `'N','X','T','S'` |
| `version` | 4 | 4 | == 19 |
| `headerSize` | 8 | 4 | == 64 |
| `layoutId` | 12 | 4 | reserved, 0 |
| `snapshotSize` | 16 | 4 | == 365744 |
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
copy 365744 bytes from mapping[base]            // then parse the copy
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

### 2.5 `Snapshot` (365744 bytes)

| field | off | size | notes |
|---|---|---|---|
| `publishSeq` | 0 | 8 | u64, producer republish counter — **not a clock**, see §2.6 |
| `gameState` | 8 | 4 | `10` login, `20` lobby, `30` in-game |
| `ownIndex` | 12 | 4 | local player's server index; -1 unless `gameState == 30` |
| `rootIfaceId` | 16 | 4 | active root interface; -1 if none |
| `serverTick` | 20 | 4 | **600ms server tick — pace on this**, see §2.6 |
| `self` | 24 | 552 | `LocalPlayer`, §2.7 |
| `npcCount` | 576 | 4 | |
| `npcs` | 580 | 36864 | `NpcEntry[1024]`, stride 36 |
| `playerCount` | 37444 | 4 | |
| `players` | 37448 | 57344 | `PlayerEntry[2048]`, stride 28 |
| `locationCount` | 94792 | 4 | |
| `locations` | 94796 | 163840 | `LocationEntry[8192]`, stride 20 |
| `inventoryCount` | 258640 | 4 | |
| `inventories` | 258644 | 256 | `InventoryHeader[32]`, stride 8 |
| `invItemCount` | 258900 | 4 | |
| `invItems` | 258904 | 16384 | `InventoryItem[2048]`, stride 8 |
| `producer` | 275288 | 32 | `ProducerState`, §2.9 |
| `openIfaceCount` | 275320 | 4 | |
| `openIfaces` | 275324 | 256 | `int32[64]` — open sub-interface ids |
| `groundItemCount` | 275580 | 4 | |
| `groundItems` | 275584 | 16384 | `GroundItemEntry[1024]`, stride 16 |
| `projectileCount` | 291968 | 4 | |
| `projectiles` | 291972 | 8192 | `ProjectileEntry[256]`, stride 32 |
| `gameCycle` | 300164 | 4 | **~20ms client cycle**, see §2.6 |
| `dynRegion` | 300168 | 36 | `DynamicRegion` — instance descriptor scalars, §2.10 |
| `dynChunkCount` | 300204 | 4 | |
| `dynChunks` | 300208 | 65536 | `uint32[16384]` — packed chunk descriptors, §2.10 |

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

Zeroed when not in-world; `serverIndex == -1` means "no local player".

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
| `skillCount` | 36 | 4 |
| `skills` | 40 | 512 | `SkillEntry[32]`, stride 16 |

### 2.8 Entry layouts

Tile coordinates are **absolute world tiles**. `plane` is 0..3.
`flags` bit 0 (`kFlagMoving`) is set when the entity is moving.

**`NpcEntry`** (36) — `serverIndex` 0, `typeId` 4, `tileX` 8, `tileY` 10,
`plane` 12, `flags` 13, `followingIndex` 14, `animationId` 16, `stanceId` 20,
`hp` 24, `maxHp` 28, `spotAnimId` 32. `typeId` is -1 if unresolved;
`animationId`/`spotAnimId` are -1 when inactive; `followingIndex` -1 if none.

**`PlayerEntry`** (28) — `serverIndex` 0, `tileX` 4, `tileY` 6, `plane` 8,
`flags` 9, `followingIndex` 10, `animationId` 12, `stanceId` 16,
`combatLevel` 20, `spotAnimId` 24.

**`LocationEntry`** (20) — scenery. `typeId` 0, `interactId` 4, `animationId` 8,
`tileX` 12, `tileY` 14, `plane` 16, `shape` 17, `rotation` 18, `flags` 19.
`flags` bits: `1` hidden, `2` combined-section, `4` deleted. A multi-tile object
emits a parent row plus one row per section; parents and sections legitimately
share `(tile, typeId)`, so **the combined-section bit is the only way to tell
them apart**. `interactId` is -1 for sections. Script-facing hosts call this a
*SceneObject*.

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

`DynamicRegion` (36 bytes, at snapshot offset 300168):

| field | off | size | notes |
|---|---|---|---|
| `isInstance` | 0 | 1 | **the authoritative flag** — branch on this |
| `truncated` | 1 | 1 | grid exceeded the cap; `dynChunkCount` is 0 |
| `sceneMode` | 4 | 4 | `3` static, `4..7` dynamic size classes; diagnostic only |
| `originMapX` | 8 | 4 | min loaded **mapsquare** X — the grid index origin |
| `originMapY` | 12 | 4 | |
| `maxMapX` | 16 | 4 | inclusive max loaded mapsquare X |
| `maxMapY` | 20 | 4 | |
| `gridW` | 24 | 4 | descriptor width in **chunks**; 0 when static |
| `gridH` | 28 | 4 | |
| `requiredChunks` | 32 | 4 | `4 * gridW * gridH` — always written, so a truncation is diagnosable |

**Units trap:** `originMap*` / `maxMap*` are mapsquares (64 tiles); `gridW` /
`gridH` are chunks (8 tiles). Forgetting the `x8` yields a plausible-looking
answer 8 tiles from correct.

`dynChunks` is the grid flattened **plane-major**:

```
gx    = (tileX >> 3) - (originMapX << 3)
gy    = (tileY >> 3) - (originMapY << 3)
index = ((plane * gridW) + gx) * gridH + gy        // 0 <= index < dynChunkCount
```

Each entry is the game's own packed descriptor, copied verbatim:

| bits | field |
|---|---|
| 24-25 | source plane |
| 14-23 | source chunk X (10 bits) |
| 3-13 | source chunk Y (11 bits) |
| 1-2 | rotation, 0..3 |

A **negative** entry means "no source chunk" — a hole in the instance. Source
mapsquare is `srcChunk >> 3`; source tile origin is `srcChunk * 8`. Rotation maps
a destination-local `(x, y)` inside the 8x8 chunk to source-local:

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
| Session | `set_world`, `change_login_state`, `login_to_lobby`, `get_auto_login`, `set_auto_login`, `get_token_refresher`, `set_token_refresher`, `trigger_token_refresh`, `schedule_break`, `interrupt_break`, `get_account_info`, `get_current_world` |
| Capture | `take_screenshot`, `start_stream`, `stop_stream` |
| Scripting / input | `get_script_handle`, `execute_script`, `destroy_script_handle`, `send_key`, `send_click`, `record_move_path` |
| Interfaces | `get_component`, `get_components`, `get_static_children`, `get_dynamic_children`, `get_interface_tree`, `find_component_at` |
| Variables | `get_varp`, `get_varps`, `get_varc_int`, `get_varcs_int`, `get_varc_string`, `get_varcs_string`, `get_obj_vars` |
| World map | `query_world_map_elements` |

Three gaps worth knowing before you design around them:

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
- `set_world` and `change_login_state` are currently stubs.

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
