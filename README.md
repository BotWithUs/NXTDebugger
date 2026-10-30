# NXTDebugger

A standalone Win32 observer for the BotWithUs agent wire surface.

NXTDebugger attaches to a running game client that has the BotWithUs agent
loaded, reads the shared-memory snapshot and event ring the agent publishes,
and renders them live — entities, interfaces, inventory, variables, script
context, and the raw event tail. It is a **read-only observer**: it decodes
what the agent publishes and issues query RPCs. It does not play the game.

## The `wire/` directory is the contract

`wire/` is the published schema — the normative description of the protocol,
vendored here byte-for-byte from the agent that produces it:

| path | what it is |
|---|---|
| `wire/PROTOCOL.md` | normative prose + the absolute offsets a consumer binds against |
| `wire/ipc/SharedLayout.h` | the snapshot struct |
| `wire/ipc/Events.h` | event discriminators and body structs |
| `wire/ipc/WireCategory.h` | category enum |
| `wire/rpc/MsgPack.{h,cpp}` | the RPC codec |

**You do not need the agent's source to build this repo, or to write your own
consumer.** `wire/PROTOCOL.md` is written to be sufficient on its own, and
everything it asserts is machine-checked (see below). The current protocol is
**v19**; the pin lives in `src/attach/Session.h` and a mismatched agent is
refused at attach time rather than mis-decoded.

## Two gates keep the documentation honest

A document that drifts from the code is worse than no document, because green
reads as verified. Two checks run automatically:

1. **`tools/check_protocol_doc.ps1`** (configure time) compares RPC method
   names, event discriminators and event body structs between `PROTOCOL.md`
   and the headers as **bidirectional set equality** — documented-but-absent
   fails just as loudly as present-but-undocumented. Every comparison refuses
   to pass on a suspiciously small parse, so a regex that silently matches
   nothing fails instead of vacuously passing.
2. **`src/wire/ProtocolDocPins.h`** (compile time) pins every absolute
   offset and size `PROTOCOL.md` prints to a real `offsetof`/`sizeof` — 170
   compile-time assertions in all.
   Change a layout without changing the document and the build stops.

Both work in a plain clone with no agent checkout, because both sides of every
comparison live in this repository.

## Build

Requirements: Windows 10/11 x64, Visual Studio 2022 (v143 toolset),
CMake 3.25+, PowerShell.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The binary lands in `build/Release/nxt_debugger.exe`, with `resources/` copied
beside it. Dear ImGui and FreeType are fetched at configure time; there are no
other third-party dependencies.

The build is warning-clean under `/W4 /WX`.

[cmkr](https://github.com/build-cpp/cmkr) generates `CMakeLists.txt` from
`cmake.toml` — edit `cmake.toml`, never `CMakeLists.txt`.

## Usage

Start the debugger, pick the client process in the **Attach** panel, and the
session binds once the magic and protocol version verify. The title bar shows
the attached pid and the negotiated protocol version.

### Layers (in-game overlay)

The **Layers** panel (View > Layers, or Overlay > Layers panel) draws game state
in the game itself, one toggle per layer: NPCs, Players, Self, Scene objects
(locs), Ground items, and Interfaces.

- **World layers** are entity highlights in the agent's own draw store, so the
  agent re-projects them in-process every tick and they track the camera
  exactly. Each layer has a colour, a label mode (none / name / name + id /
  name + id + distance), a max distance in tiles, a max count (nearest first,
  "showing N of M" when capped) and a filter (name substring, or an exact id).
- **Traffic.** While a world layer is on, the debugger refreshes on a timer
  (600 ms by default) and sends only what changed: at most one
  `debug_draw_set_batch` and one `debug_draw_clear` per layer per refresh.
  Draws carry a TTL of five refresh intervals, so a debugger that dies leaves
  nothing behind for long.
- **Keys** are `dbg:<layer>:<identity>`. Unticking a layer clears exactly its
  keys; detach and exit clear all of them. The debugger never calls
  `debug_draw_clear_all`: the host and scripts share the overlay.
- **Labels** each take one of the agent's 64 text slots, shared with every
  connection, so labels are budgeted across all layers (40 by default, nearest
  first). Boxes need no slot. Everything on one tile (a loc at its footprint
  origin) shares one caption across layers - "Zezima | Goblin x3 | Bank booth",
  self, player, npc, ground, loc order, cut at 48 chars with "... +N more" -
  and spends one label of the budget.
- **Locs** draw their real footprint: the debugger reads `sizeX`/`sizeY` from
  the cache loc definition (swapped for rotation 1 and 3) and sends it as
  `w`/`h`. With the cache closed, locs draw one tile.
- **Older agent DLLs** (released before `feat/overlay-loc-ground`) refuse
  entity batch entries as an unknown kind. The first such refusal switches the
  session to per-entity mode: NPCs, players and self go out as one
  `highlight_entity` call each, still only for adds and changes, at most 64
  calls per refresh (the rest go out on the next one). Locs and ground items
  are tried once that way and, when refused, latched with "needs the newer
  agent DLL". The panel shows the mode ("agent: batch entities" / "agent:
  legacy, per-entity"); it is detected again after a reconnect or a Retry.
- An agent that refuses a layer's entries otherwise gets the layer latched off
  with the agent's own error, and no retries, until you re-tick it, press
  Retry, or reconnect.
- **Interfaces** is the existing transparent overlay window that paints the
  Interfaces panel's selected interface. It sits just above the game in
  z-order (never topmost), shows only while the game - or the debugger over a
  non-minimised game - is in front and there are boxes to paint, and draws no
  text of its own: the Layers panel says when there is nothing to draw.

Settings persist in `imgui.ini` under `[NXTDebugger][Layers]`.

### Command line

```
nxt_debugger.exe [--attach auto|<pid>] [--ctl] [--minimized | --hidden] [--cs2-dir <path>]
```

| flag | effect |
|---|---|
| `--attach auto` | attach to the single `rs2client` that publishes an agent snapshot (what the Attach panel does). With none or several it keeps retrying every 3 s, and `status` says why (`attach_status`). |
| `--attach <pid>` | attach to that client pid, retrying every 3 s until its agent appears. |
| `--ctl` | open the control pipe (below). Off by default. |
| `--minimized` | start minimized without taking focus. |
| `--hidden` | no window at all; also neither loads nor saves the shared `imgui.ini`, so it cannot clobber an interactive instance's layout. Stop it with `nxtdbgctl quit`. |
| `--cs2-dir <path>` | the `cs2_asm` corpus the CS2 panel, `cs2` and `xref` read. |

A bad flag exits with code 2 and prints to the parent console, not a message box.

## Control pipe

`--ctl` exposes everything the debugger sees to an automation client, with no
GUI involved. It is as read-only as the rest of the debugger: no command queues
a game action, and `rpc` forwards only an allowlist of query methods. The one
thing it writes is the overlay layers (`layers`), which only draw.

- **Pipes.** `\\.\pipe\nxtdebugger-ctl-<debugger pid>`, plus the alias
  `\\.\pipe\nxtdebugger-ctl`, which the first `--ctl` instance on the box owns.
  Both are local-only and ACL'd to the current user.
- **Wire.** Line-delimited JSON. The request is
  `{"cmd": "<name>", "req_id"?: any, ...args}` (or the args nested under
  `"args"`). The reply is one line: `{"ok": true, "req_id"?, "cmd", ...result}`
  or `{"ok": false, "req_id"?, "error": "..."}`.
- **Threading.** Requests are queued and answered on the UI thread, once per
  frame, because that thread owns the single synchronous `App::rpc`. A request
  therefore never races a panel's RPC. A minimized or hidden window still
  services the pipe at about 60 Hz.
- **Agent pipe slots.** An attached debugger holds two of the agent's four pipe
  slots, `App::rpc` and `App::tap`, exactly as the GUI does. The control pipe
  adds none.

### nxtdbgctl.exe

`build/Release/nxtdbgctl.exe` is the client. It sends one request, prints the
reply line to stdout, and exits with 0 (ok), 1 (the debugger answered
`ok:false`), 2 (no debugger / pipe error) or 3 (usage).

```
nxtdbgctl [--pipe NAME | --pid DEBUGGER_PID] [--pretty] [--timeout MS] <cmd> [json | key=value ...]
nxtdbgctl changes|events --follow [--interval MS] [since=N] ['types=[...]']
nxtdbgctl --list                  # every nxtdebugger-ctl pipe on the box
```

Arguments are either one JSON object (`'{"iface": 1477}'`) or `key=value`
pairs. The value is parsed as JSON when it parses, else taken as a string, so
`varp=[1,2]` is a list and `name=COINS` is a string. The default pipe is
`$NXTDBGCTL_PIPE`, else the alias. Quote brackets and braces in Git Bash.

### Commands

Every agent-backed command answers `{"ok": false, "error": "not attached"}`
when no session is bound. The offline commands (`gameval`, `cache`, `cs2`,
`xref`, `help`) work without a client.

| command | arguments | returns |
|---|---|---|
| `status` | none | `attached`, `pid`, `protocol_version` (the agent's) and `debugger_protocol`, `attach_pending` / `attach_status` / `attach_error`. When attached it adds `game_state`, `server_tick`, `game_cycle`, `publish_seq`, `root_iface`, `player` (`tile_x`, `tile_y`, `plane`, `animation`, ...; `null` unless game state is 30) and `open_ifaces` (`[{id, name}]`). Also `rpc` connection state, `logs` heads and drops, `cache_open`, and the `ctl` pipe names. |
| `tree` | `iface`, `comp?` (root, default 0) | `get_interface_tree`, decoded: `nodes[]` with `index`, `hash` (`iface<<16 \| comp`), every field the agent sends (`iface comp sub type category x y w h raw_* *_mode abs_screen_pos text hidden sprite_id item_id item_amount triggers parent`), plus `category_name`, `gameval` and `item_name` (the last only when the cache is open). |
| `comp` | `iface`, `comp`, `sub?` | one component, same shape. With `sub` it is picked from the tree, because `get_component` cannot address a dynamic child. |
| `vars` | any of `varp`, `varbit`, `varc`, `varcstr` (an int or a list) | `{id, value, found, name}` rows per kind. Varbits are decoded from the backing varp with the cache definition (`source: "varp"`), else they fall back to the last ring event (`"event"`), else `null` (`"none"`). The agent has no varbit read. |
| `watch` | any of `varp`, `varbit`, `varc`, `varcstr`, `iface`; `var_interval_ms?` (100), `iface_interval_ms?` (300), `geometry?` (also diff x/y/w/h), `replace?` | the watch set with baseline values, plus `cursor` (the change-log head, to pass as `since`). Watches are polled from the frame loop; 0 ms means every frame. |
| `unwatch` | the same lists, or `all: true` | the remaining watch set |
| `changes` | `since?` (0), `max?` (500, cap 5000), `types?` (list of kinds) | `{head, oldest, truncated, next, changes[]}`. Pass `next` back as `since`. `truncated: true` means your cursor fell behind the 8192-entry ring. |
| `events` | `since?`, `max?`, `types?` (type names such as `var`, `chat`) | the same envelope over the decoded event ring: `ring_seq`, `type`, `type_id`, `text` (the exact Events-panel line), plus structured fields for var / varbit / varc / obj-var / chat / action / radio / login / tick / key events. Also `drops`, `paused`. |
| `rpc` | `method`, `params?` | `{method, result}`, with the reply msgpack converted to JSON field for field. Allowlist: `rpc.ping rpc.list_methods rpc.client_count get_game_cycle get_login_state get_action_queue_size get_action_history get_last_action_time are_actions_blocked get_auto_login get_token_refresher get_account_info get_current_world click_stats move_stats get_component get_components get_static_children get_dynamic_children get_interface_tree find_component_at get_varp get_varps get_varc_int get_varcs_int get_varc_string get_varcs_string get_obj_vars query_spot_anims query_world_map_elements _debug.entity_facing debug_draw_list debug_draw_stats debug_draw_probe_pixels`. Anything else is refused. |
| `cs2` | `id`, `decompiled?`, `dir?` | `{path, source, text}`: the `.asm` disassembly, or with `decompiled` the sibling `cs2_decompiled/<id>.cs2`. `dir` repoints the corpus, and is refused while an xref index is building. |
| `xref` | `kind` (`varbit`\|`varp`\|`const`), `id`, `dir?` | `{status: "building", files_done, files_total}` until the index is ready (the first call starts it), then `{status: "ready", total, scripts[]}`. |
| `gameval` | `kind`, then `id`, `name` (exact) or `search` (substring); `max?` (200) | an id gives `{id, name}`; a name or search gives `{total, matches[]}`. Kinds: `item obj npc loc if interface component varp varbit varc struct enum param inv seq sprite graphic model dbrow quest`. `component` takes `id` (the hash) or `iface` + `comp`. |
| `cache` | `type` (`npc item loc seq varbit enum struct inv param quest`), `id` | `{found, gameval, definition}`, decoded by NXTCache. The local cache opens on first use, and `NXTCache.dll` must sit beside the exe or on PATH. |
| `layers` | optional action `get` \| `set` \| `retry`; `<layer>=on\|off` and `<layer>.color` / `.labels` / `.max_distance` / `.max_count` / `.filter`, plus `refresh_ms`, `label_budget`, `iface` (layers: `npc` `player` `self` `loc` `ground`) | every layer's settings and state (`shown`, `matched`, `sent`, `rejected`, `message`), `labels_shown` / `labels_wanted`, `agent_mode` (`unknown` \| `batch` \| `per_entity`). A request with one bad argument applies none of them. |
| `attach` | `pid` or `auto: true` | binds the observer session (the Attach panel's button); nothing is sent to the game |
| `detach` | none | drops the session |
| `quit` | none | closes this debugger instance after the reply |
| `help` | none | the command list |

The change log also records these always, whether or not anything is watched:
`attach` / `detach`, `game_state`, and `iface_open` / `iface_close` (taken from
the snapshot's open-interface list). Watched items add:

- `varp`, `varbit`, `varc`, `varcstr`: `{id, old, new, source: "event" | "poll"}`.
  The ring event is preferred, so a value that flips and reverts between polls
  is still caught.
- `comp_changed`: `{iface, comp, sub, gameval, field, old, new}`, over `text`,
  `hidden`, `type`, `category`, `sprite_id`, `item_id`, `item_amount`,
  `triggers`, and also `x y w h` with `geometry`.
- `comp_added` / `comp_removed`.
- `iface_tree`: `{state: "empty" | "populated"}` when a watched interface
  closes or reopens.

The component map carries no right-click op strings; `triggers` (the script
triggers) is the closest thing the wire has, and it is what gets diffed.

Examples (Git Bash):

```sh
nxt_debugger.exe --attach auto --ctl --minimized &
nxtdbgctl status --pretty
nxtdbgctl tree iface=1477
nxtdbgctl comp iface=1477 comp=12 sub=3
nxtdbgctl vars 'varp=[1,2]' varbit=5000 varc=180 varcstr=2
nxtdbgctl watch 'varp=[3,4]' iface=1477 geometry=true
nxtdbgctl changes since=0 max=100
nxtdbgctl changes --follow                    # streams new changes, one per line
nxtdbgctl events --follow 'types=["chat"]'
nxtdbgctl rpc method=get_varps '{"params": {"ids": [1, 2]}}'
nxtdbgctl cs2 id=1 'dir=C:\path\to\cs2_asm'
nxtdbgctl xref kind=varbit id=5000
nxtdbgctl gameval kind=item name=COINS
nxtdbgctl gameval kind=component iface=1477 comp=0
nxtdbgctl cache type=item id=995
nxtdbgctl layers set npc=on loc=off npc.filter=Banker npc.labels=name_id
nxtdbgctl layers
nxtdbgctl quit
```

If a checkout of the agent that produces this wire happens to sit next to this
one, configure additionally diffs `wire/` against it and fails on drift, so an
in-house forgotten sync becomes a build error rather than a wrong decode at
runtime. Publicly there is no sibling, the check is skipped, and `wire/` is
simply the schema.

## Licence

MIT — see [LICENSE](LICENSE).
