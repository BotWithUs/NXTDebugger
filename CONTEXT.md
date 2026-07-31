# CONTEXT.md — NXTDebugger

Glossary specific to the debugger context. Cross-references the workspace
root `CONTEXT-MAP.md` and the producer's `NXTLibrary/CONTEXT.md`. When this
file's vocabulary diverges from the producer's, the producer's wire-side
spelling wins inside `src/wire/` (those decoders match `SharedLayout.h`
field names byte-for-byte); the debugger UI uses the names defined here.

## Core terms

- **Agent.** The producer process — the injected `NXTLibrary.dll` running
  inside `rs2client*.exe`. The debugger never confuses "the game process"
  with "the agent": when no DLL is loaded there is no `Local\nxt_snapshot_*`
  mapping and the attach fails.
- **Attach.** The handshake the debugger performs to bind to one agent:
  open the named mapping, verify magic + protocol version, hold the view +
  handle in an RAII `Session`. Detach releases both.
- **Session.** The RAII container around one open mapping. Owns the
  `HANDLE mapping`, the `void *view`, and the wide-string last-error
  buffer. Move-only; copying would double-close.
- **Tick** (server tick). One advance of `Snapshot::serverTick` — the server's
  600ms game-logic step, and the clock script authors reason in. The Snapshot
  panel leads with it and `kEventTick` fires once per advance.
- **Game cycle.** One advance of `Snapshot::gameCycle` — the client's own ~20ms
  main-loop counter, ~30 per server tick. This is the unit
  `ProjectileEntry::startCycle`/`endCycle` are stamped in.
- **Publish sequence.** One advance of `Snapshot::publishSeq` — the producer's
  own republish counter, same ~20ms cadence as the game cycle but a different
  number space (it starts at 1 when the agent attaches, so never compare the
  two). The debugger's status dot pulses on it because it is the field that
  moves on every republish; panels recompute from the new front buffer.
  Through v17 this field was misnamed `tickId`, which is why anything that
  paced off it ran ~30x fast.
- **Snapshot.** The POD struct at `Local\nxt_snapshot_<pid>` + double-buffer
  offset. The debugger reads it lock-free via acquire-load on `frontIdx`.
- **Event ring.** The SPMC ring inside the same mapping. The debugger keeps
  one `EventReader` per session and drains on every frame; the first drain
  primes `lastSeq` to the current head so backlog isn't replayed.
- **Drop.** A slot whose committed `seq` exceeds the reader's expected
  `seq` — the writer has wrapped over an unread slot. The debugger tracks
  the running drop count and surfaces it as a red pill in the Event tail.
- **Panel.** One ImGui window with a focused responsibility. Panels are
  POD entries in `App::panels` (name + open flag + function pointer);
  there is no panel base class — runtime polymorphism is banned per the
  workspace `cpp-rules`.
- **Card.** A bordered child region with a left amber stripe + bold title.
  The reusable section primitive defined in `app/Theme.{h,cpp}`
  (`BeginCard` / `EndCard`). Every panel composes its content out of
  cards.
- **Hero stat.** The large-font number + small-font caption combo used to
  give a single number top-of-panel visual weight (tick id, pid, hp, etc.)
  Defined in `app/Theme.h::HeroStat`.
- **Interface panel.** The Phase 5 three-pane browser
  (`panels/InterfacePanel.cpp`) for visually walking the live RS3 UI:
  open interfaces from the snapshot, full component tree from
  `get_interface_tree`, and the field dump of the selected component.
  As of Phase 7 it no longer owns a private pipe slot — it calls through
  the shared **Shared RPC client** (`App::rpc`, below) alongside the other
  round-trip panels.
- **Pick mode.** The Interface panel's toggle that polls the cursor at
  5 Hz, sends each cursor sample to the agent's new `find_component_at`
  RPC, and auto-selects whatever component the agent reports under the
  pointer. Bounded to one in-flight RPC at a time — moving the mouse
  faster doesn't queue calls.

## Shipped surfaces (formerly "reserved")

- **Topic / Tap / ScriptContext / Annotation** *(Phases 3–4, SHIPPED)*. The
  broker's `rpc.tap` processed-RPC stream renders in the **RPC Tap** panel;
  frameworks' `script.context` state / trace / annotation streams render in
  the **Script Context** panel. Both ride the shared `App::tap` push
  connection — one pipe slot for all push topics.
- **Manifest** *(SHIPPED)*. Covered by runtime `rpc.list_methods` discovery
  plus the curated param-hint table (`rpc/Methods.cpp`): the RPC console
  renders typed forms for the curated methods and raw params for the rest.

## Phase 7 vocabulary

- **Ground tab.** The 4th Entities-panel tab over `Snapshot::groundItems[]`
  — every alive ground-item stack in the loaded scene, with item icon /
  name / qty / tile and chebyshev distance from the local player.
- **Shared RPC client.** `App::rpc` — one synchronous `RpcClient` the whole
  debugger shares for round-trip RPC (Interface, Var Watcher, Obj Vars,
  Action History, Session). Connect / disconnect tracked in
  `App::UpdateRpcConnection`, mirroring the shared `App::tap` for push.
  Keeps the debugger at a single request/reply pipe slot.
- **Var Watcher.** Panel that polls watched varp / varc-int / varc-string
  ids over the batched read RPCs (`get_varps` / `get_varcs_*`) and flashes a
  row on change. Varbits have no read RPC, so varbit rows update only from
  `kEventVarbitChange`.
- **Obj vars.** Per-item variables (augment XP, charges, …) read via
  `get_obj_vars`; surfaced as an opt-in slot dot + tooltip in the Inventory
  panel, refreshed on `kEventObjVarChange`.
- **Session panel.** Token / session health — `get_token_refresher` expiry
  countdown + status dot, a `trigger_token_refresh` button, and a timeline
  of the token-refresh ring events (fired / refreshed / failed = 3 / 4 / 5).
