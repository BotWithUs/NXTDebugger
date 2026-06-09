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
- **Tick.** One advance of `Snapshot::tickId` — published by the producer's
  `MainLogic` detour. The debugger's status dot pulses on the frame the
  tick changes; panels recompute their state from the new front buffer.
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
  Owns its own pipe-slot — a third sync `RpcClient` alongside the RPC
  console and the tap.
- **Pick mode.** The Interface panel's toggle that polls the cursor at
  5 Hz, sends each cursor sample to the agent's new `find_component_at`
  RPC, and auto-selects whatever component the agent reports under the
  pointer. Bounded to one in-flight RPC at a time — moving the mouse
  faster doesn't queue calls.

## Reserved for later phases

- **Topic** *(Phase 3, no UI yet)*. A named subscription channel exposed by
  the agent's broker. Initial topics planned: `rpc.tap`, `script.context`,
  `script.trace`, `script.state`, `script.annotation`.
- **Tap** *(Phase 3, no UI yet)*. The specific topic the agent publishes
  its own processed-RPC stream onto: `rpc.tap`. A passive observer; no
  framework changes required.
- **ScriptContext** *(Phase 4, no UI yet)*. The umbrella topic family
  frameworks voluntarily publish into to surface script state / trace
  steps / annotations to debugger panels.
- **Annotation** *(Phase 4, no UI yet)*. One framework-emitted marker
  associated with a tick + tag (e.g. "started boss attempt", "entered
  bank state"). Rendered inline in the Event tail and on the (planned)
  scene view.
- **Manifest** *(Phase 2, no UI yet)*. The catalog of available RPC
  methods + their parameter shapes. Either client-side hand-maintained
  list or a `_meta.methods` RPC the agent exposes.
