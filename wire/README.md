# `wire/` — the published wire surface

This directory is the schema NXTDebugger binds against, and it is the same
schema any third-party consumer binds against. The agent publishes two
cross-process surfaces, and both are open by design so clients can be written in
any language:

- **Snapshot SHM** — `Local\nxt_snapshot_<pid>`, a double-buffered POD region
  carrying per-tick game state plus an event ring.
- **MsgPack RPC** — `\\.\pipe\BotWithUs_<pid>`, `[u32 LE length][msgpack]`
  frames, `{id, method, params?}` request and `{id, result|error}` reply.

**Writing a consumer in another language?** [`PROTOCOL.md`](PROTOCOL.md) is the
normative byte-level description — region geometry, the double-buffer read
protocol, every struct offset, the event-ring algorithm and body layouts, pipe
framing, and the RPC method catalog. The headers below are the same contract
expressed in C++.

| File | What it defines |
|---|---|
| `ipc/SharedLayout.h` | Mapping layout, `kMagic`, `kProtocolVersion`, the `Snapshot` POD and every entry type in it. Every offset is pinned by `static_assert`. |
| `ipc/Events.h` | Event ring discriminators (`kEvent*`) and the POD body for each. |
| `ipc/WireCategory.h` | Stable semantic category for an interface component — the `category` field of the component map. |
| `rpc/MsgPack.h`, `rpc/MsgPack.cpp` | Minimal MsgPack reader/writer for the RPC pipe. Convenience for C++ consumers; the format is standard MsgPack, so other languages should use their own library. |

## Do not edit these files here

They are copied byte-for-byte from the producer, which is their source of
truth. To change the schema, change it there and re-run:

```powershell
.\tools\sync_wire.ps1          # refresh from a sibling checkout
.\tools\sync_wire.ps1 -Check   # report drift without writing
```

When a producer checkout is present next to this one, CMake diffs these files
at configure time and **fails the build** if they have drifted, so an
out-of-date copy cannot ship. A standalone clone has no producer beside it —
there the check is skipped and these files are simply the schema.

## Version compatibility

`kProtocolVersion` gates the whole snapshot layout. A consumer must refuse a
mapping whose version does not match the one it was built against; the fields
move between versions and there is no forward compatibility. This project pins
it at compile time in `src/attach/Session.h`:

```cpp
static_assert(nxt::ipc::kProtocolVersion == 19, ...);
```

so a schema bump that lands here without a matching consumer update is a build
error rather than a runtime misread. RPC methods and event types are additive
and do **not** bump the version — only the SHM snapshot layout does.
