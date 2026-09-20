#include "App.h"
#include "Theme.h"

#include "panels/Panels.h"
#include "wire/EventReader.h"
#include "wire/SnapshotReader.h"

#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder* — programmatic default layout

#include <algorithm>
#include <cstring>

namespace nxtdbg::app
{

namespace
{

constexpr size_t kEventBacklogCap = 4096;

void DrainEvents(App &a)
{
    if (a.paused || !a.session.IsOpen())
    {
        return;
    }
    std::vector<wire::EventRecord> fresh;
    a.events.Drain(a.session, fresh);
    for (auto &r : fresh)
    {
        a.eventBacklog.push_back(r);
    }
    if (a.eventBacklog.size() > kEventBacklogCap)
    {
        a.eventBacklog.erase(
            a.eventBacklog.begin(),
            a.eventBacklog.begin() + (a.eventBacklog.size() - kEventBacklogCap));
    }
}

// Match the shared TapClient's connection to the current SHM attach. The
// per-panel topic subscriptions re-register themselves in their own
// EnsureConnection helpers when the pid changes.
void UpdateTapConnection(App &a)
{
    DWORD pid = a.session.IsOpen() ? a.session.Pid() : 0;
    if (pid == a.tapConnectedPid && (pid == 0 || a.tap.IsConnected())) return;
    if (pid != a.tapConnectedPid)
    {
        a.tap.Disconnect();
        a.tapConnectedPid = pid;
    }
    if (pid != 0 && !a.tap.IsConnected())
    {
        a.tap.Connect(pid);
    }
}

// Match the shared synchronous RPC client to the current SHM attach. Mirrors
// UpdateTapConnection, but for the request/reply pipe used by the Interfaces,
// Var Watcher, Obj Vars, Action History and Session Health panels — they all
// share this one connection so the debugger uses a single pipe slot for every
// round-trip RPC, leaving headroom under kPipeMaxInstances.
void UpdateRpcConnection(App &a)
{
    DWORD pid = a.session.IsOpen() ? a.session.Pid() : 0;
    if (pid == a.rpcConnectedPid && (pid == 0 || a.rpc.IsConnected()))
    {
        return;
    }
    if (pid != a.rpcConnectedPid)
    {
        a.rpc.Disconnect();
        a.rpcConnectedPid = pid;
    }
    if (pid != 0 && !a.rpc.IsConnected())
    {
        a.rpc.Connect(pid);
    }
}

void UpdateTickPulse(App &a)
{
    float dt = ImGui::GetIO().DeltaTime;
    a.dotPhase += dt;
    if (a.dotPhase > 1000.0f)
    {
        a.dotPhase = 0.0f;
    }
    a.tickPulseT = std::max(0.0f, a.tickPulseT - dt * 4.5f);
    if (!a.session.IsOpen())
    {
        return;
    }
    const auto *snap = wire::CurrentSnapshot(a.session);
    if (!snap)
    {
        return;
    }
    // Pulse on publishSeq, not serverTick: this is a "the agent is alive and
    // republishing" indicator, so the ~20ms field is the right input. A pulse
    // driven off serverTick would sit dark for 600ms at a time and read as a hang.
    if (snap->publishSeq != a.lastPublishSeqSeen)
    {
        a.lastPublishSeqSeen = snap->publishSeq;
        a.tickPulseT         = 1.0f;
    }
}

// Curated "clean start" set — the panels open by default. Everything else
// (cache tools, cs2, the RPC stream panels) starts closed, one click away in
// the View menu. Single source of truth for both Init and Reset layout.
constexpr const char *kDefaultOpenPanels[] = {
    "Attach", "Snapshot", "Player", "Entities", "Inventory", "Events", "Log", "Interfaces",
};

bool IsDefaultOpen(const char *name)
{
    for (const char *n : kDefaultOpenPanels)
    {
        if (std::strcmp(name, n) == 0)
        {
            return true;
        }
    }
    return false;
}

void ApplyDefaultVisibility(App &a)
{
    for (auto &p : a.panels)
    {
        p.open = IsDefaultOpen(p.name);
    }
}

// Carve the dockspace into an IDE-style frame and assign every window a home,
// so panels land in sensible, tab-grouped regions instead of piling up. Window
// names must match each panel's ImGui::Begin() title exactly (note "RPC Tap" /
// "Script Context" differ from their View-menu labels).
void BuildDefaultLayout(ImGuiID dockId)
{
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockId;
    ImGuiID left   = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left,  0.18f, nullptr, &center);
    ImGuiID right  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down,  0.30f, nullptr, &center);

    // Left — entry point + small query tool.
    ImGui::DockBuilderDockWindow("Attach",        left);
    ImGui::DockBuilderDockWindow("Cache lookup",  left);

    // Centre — the working surface (hero), specialised viewers tabbed behind it.
    ImGui::DockBuilderDockWindow("Interfaces",    center);
    ImGui::DockBuilderDockWindow("Entities",      center);
    ImGui::DockBuilderDockWindow("Inventory",     center);
    ImGui::DockBuilderDockWindow("Cache Browser", center);
    ImGui::DockBuilderDockWindow("CS2 script",    center);
    ImGui::DockBuilderDockWindow("Debug Draw",    center);

    // Right — live inspectors.
    ImGui::DockBuilderDockWindow("Player",         right);
    ImGui::DockBuilderDockWindow("Snapshot",       right);
    ImGui::DockBuilderDockWindow("Script Context", right);
    ImGui::DockBuilderDockWindow("Var Watcher",    right);
    ImGui::DockBuilderDockWindow("Session",        right);

    // Bottom — streaming / log surfaces.
    ImGui::DockBuilderDockWindow("Log",         bottom);
    ImGui::DockBuilderDockWindow("Events",      bottom);
    ImGui::DockBuilderDockWindow("RPC console", bottom);
    ImGui::DockBuilderDockWindow("RPC Tap",     bottom);

    ImGui::DockBuilderFinish(dockId);
}

void DrawMenuBar(App &a)
{
    if (!ImGui::BeginMainMenuBar())
    {
        return;
    }
    if (ImGui::BeginMenu("View"))
    {
        for (auto &p : a.panels)
        {
            ImGui::MenuItem(p.name, nullptr, &p.open);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window"))
    {
        if (ImGui::MenuItem("Reset layout"))
        {
            ApplyDefaultVisibility(a);
            a.requestDefaultLayout = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Open all panels"))
        {
            for (auto &p : a.panels)
            {
                p.open = true;
            }
        }
        if (ImGui::MenuItem("Close all panels"))
        {
            for (auto &p : a.panels)
            {
                p.open = false;
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Capture"))
    {
        ImGui::MenuItem("Pause event drain",  nullptr, &a.paused);
        ImGui::Separator();
        ImGui::MenuItem("Auto-scroll events", nullptr, &a.autoScrollEvts);
        ImGui::MenuItem("Auto-scroll log",    nullptr, &a.autoScrollLog);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Overlay"))
    {
        ImGui::MenuItem("Game overlay", nullptr, &a.overlayEnabled);
        ImGui::TextDisabled("draws the Interfaces panel's");
        ImGui::TextDisabled("selected iface on the live game");
        ImGui::Separator();
        ImGui::MenuItem("Show all labels",        nullptr, &a.overlayShowAllLabels);
        ImGui::MenuItem("Only when game focused", nullptr, &a.overlayFollowFg);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

}

void App::Init()
{
    // `open` here is a placeholder — the curated default-open set is applied by
    // ApplyDefaultVisibility (single source of truth: kDefaultOpenPanels).
    panels = {
        { "Attach",       false, &panels::DrawAttach            },
        { "Player",       false, &panels::DrawPlayerState       },
        { "Snapshot",     false, &panels::DrawSnapshotInspector },
        { "Entities",     false, &panels::DrawEntityBrowser     },
        { "Inventory",    false, &panels::DrawInventoryPanel    },
        { "Events",       false, &panels::DrawEventTail         },
        { "Cache lookup", false, &panels::DrawCacheLookup       },
        { "Cache Browser",false, &panels::DrawCacheBrowser      },
        { "CS2 script",   false, &panels::DrawCs2Script         },
        { "RPC console",  false, &panels::DrawRpcConsole        },
        { "RPC tap",      false, &panels::DrawRpcTap            },
        { "Script ctx",   false, &panels::DrawScriptContext     },
        { "Interfaces",   false, &panels::DrawInterfacePanel    },
        { "Var Watcher",  false, &panels::DrawVarWatcher        },
        { "Session",      false, &panels::DrawSessionHealth     },
        { "Debug Draw",   false, &panels::DrawDebugDraw         },
        { "Log",          false, &panels::DrawLogPanel          },
    };
    ApplyDefaultVisibility(*this);
    gameval.Init();
    theme::Apply();
}

void App::Update()
{
    DrainEvents(*this);
    UpdateTapConnection(*this);
    UpdateRpcConnection(*this);
    UpdateTickPulse(*this);

    // Implicit dockspace covering the main viewport. PassthruCentralNode keeps
    // the central area transparent so the GL clear colour shows through when
    // nothing is docked there; every Begin()/End() in panels becomes a
    // dockable window automatically.
    ImGuiID dockId = ImGui::DockSpaceOverViewport(
        0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

    DrawMenuBar(*this);

    // Apply the curated default layout when requested (first run / Reset layout).
    // Runs after the dockspace exists and before panel windows Begin(), so the
    // re-dock takes effect this same frame.
    if (requestDefaultLayout)
    {
        BuildDefaultLayout(dockId);
        requestDefaultLayout = false;
    }

    for (auto &p : panels)
    {
        if (p.open)
        {
            p.update(*this);
        }
    }
}

}
