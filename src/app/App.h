#pragma once
#include "attach/Session.h"
#include "cache/CacheClient.h"
#include "cs2/Cs2Index.h"
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

    // UI / animation state. Filled from the wire on Update(), read by panels.
    uint64_t              lastTickSeen   = 0;
    float                 tickPulseT     = 0.0f;   // 1.0 on tick advance, decays
    float                 dotPhase       = 0.0f;   // 0..1 sec for status-dot pulse
    bool                  paused         = false;
    bool                  autoScrollLog  = true;
    bool                  autoScrollEvts = true;

    void Init();
    void Update();   // called once per frame after ImGui::NewFrame()
};

}
