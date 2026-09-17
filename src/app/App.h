#pragma once
#include "app/InterfaceView.h"
#include "attach/Session.h"
#include "cache/CacheClient.h"
#include "cs2/Cs2Index.h"
#include "gameval/GameVal.h"
#include "rpc/RpcClient.h"
#include "rpc/TapClient.h"
#include "wire/EventReader.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nxtdbg::app
{

struct App;

struct Panel
{
    const char *name;
    bool        open;
    void      (*update)(App &);
};

struct App
{
    attach::Session       session;
    cache::CacheClient    cache;
    // Bundled gameval id -> symbolic-name tables (resources/gameval/*.json).
    // Fallback label wherever the cache has no display name — used by the cache
    // panels, the entity browser and the interface panel.
    gameval::GameVal      gameval;
    wire::EventReader     events;
    std::vector<wire::EventRecord> eventBacklog;
    std::vector<Panel>    panels;

    // CS2 disassembly cross-reference index (id -> referencing script ids),
    // built off the on-disk cs2_asm corpus. Shared by the Cache Browser's xref
    // card and the CS2 script viewer. `cs2RequestedScript` is the cross-panel
    // hand-off: the xref card writes a script id, the viewer consumes it.
    cs2::Cs2Index         cs2;
    int                   cs2RequestedScript    = -1;
    int                   cs2RequestedHighlight = -1;   // id to highlight in the asm

    // Single shared pipe-tap connection. Multiple panels (RPC tap,
    // script context, future broker topics) subscribe distinct topics on
    // this one connection so the debugger uses one pipe slot for all push
    // traffic, leaving spares for the RPC console and a second debugger.
    // Connect / disconnect tracked here in Update() (mirrors SHM attach).
    rpc::TapClient        tap;
    DWORD                 tapConnectedPid = 0;

    // Single shared synchronous request/reply RPC client. Every panel that
    // issues round-trip RPCs (Interfaces, Var Watcher, Obj Vars, Action
    // History, Session Health) calls through this one client so the debugger
    // spends a single pipe slot on all of them — same rationale as the shared
    // tap above. Connect / disconnect tracked here in Update()
    // (UpdateRpcConnection), mirroring the SHM attach. RpcClient serialises one
    // Call at a time on the UI thread, so sharing it across panels in a frame
    // is safe by construction.
    rpc::RpcClient        rpc;
    DWORD                 rpcConnectedPid = 0;

    // UI / animation state. Filled from the wire on Update(), read by panels.
    uint64_t              lastPublishSeqSeen = 0;
    float                 tickPulseT     = 0.0f;   // 1.0 on republish, decays
    float                 dotPhase       = 0.0f;   // 0..1 sec for status-dot pulse
    bool                  paused         = false;
    bool                  autoScrollLog  = true;
    bool                  autoScrollEvts = true;

    // Set true to (re)apply the curated default dock layout on the next frame:
    // by main.cpp on first run (no saved imgui.ini) and by Window > Reset layout.
    // Consumed in Update() once the dockspace exists, before panels are drawn.
    bool                  requestDefaultLayout = false;

    // External interface overlay. The Interfaces panel publishes the selected
    // interface's component boxes into `interfaceView`; main.cpp drives a
    // separate transparent, click-through, topmost OverlayWindow that paints
    // them on top of the live game. The overlay reads this read-only — it opens
    // no pipe and adds no RPC traffic (it rides the panel's existing fetch).
    InterfaceView         interfaceView;
    bool                  overlayEnabled       = false;
    bool                  overlayShowAllLabels = false;
    bool                  overlayFollowFg      = true;   // hide when game unfocused

    void Init();
    void Update();   // called once per frame after ImGui::NewFrame()
};

}
