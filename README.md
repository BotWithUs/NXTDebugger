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

If a checkout of the agent that produces this wire happens to sit next to this
one, configure additionally diffs `wire/` against it and fails on drift, so an
in-house forgotten sync becomes a build error rather than a wrong decode at
runtime. Publicly there is no sibling, the check is skipped, and `wire/` is
simply the schema.

## Licence

MIT — see [LICENSE](LICENSE).
