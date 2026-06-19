#include "App.h"
#include "Theme.h"

#include "panels/Panels.h"
#include "wire/EventReader.h"
#include "wire/SnapshotReader.h"

#include "imgui.h"

#include <algorithm>

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
    if (snap->tickId != a.lastTickSeen)
    {
        a.lastTickSeen = snap->tickId;
        a.tickPulseT   = 1.0f;
    }
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
    if (ImGui::BeginMenu("Capture"))
    {
        ImGui::MenuItem("Pause event drain",  nullptr, &a.paused);
        ImGui::Separator();
        ImGui::MenuItem("Auto-scroll events", nullptr, &a.autoScrollEvts);
        ImGui::MenuItem("Auto-scroll log",    nullptr, &a.autoScrollLog);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

}

void App::Init()
{
    panels = {
        { "Attach",       true,  &panels::DrawAttach            },
        { "Player",       true,  &panels::DrawPlayerState       },
        { "Snapshot",     true,  &panels::DrawSnapshotInspector },
        { "Entities",     true,  &panels::DrawEntityBrowser     },
        { "Events",       true,  &panels::DrawEventTail         },
        { "Cache lookup", false, &panels::DrawCacheLookup       },
        { "Cache Browser",true,  &panels::DrawCacheBrowser      },
        { "CS2 script",   true,  &panels::DrawCs2Script         },
        { "RPC console",  true,  &panels::DrawRpcConsole        },
        { "RPC tap",      true,  &panels::DrawRpcTap            },
        { "Script ctx",   true,  &panels::DrawScriptContext     },
        { "Interfaces",   true,  &panels::DrawInterfacePanel    },
        { "Log",          true,  &panels::DrawLogPanel          },
    };
    theme::Apply();
}

void App::Update()
{
    DrainEvents(*this);
    UpdateTapConnection(*this);
    UpdateTickPulse(*this);

    // Implicit dockspace covering the main viewport. PassthruCentralNode keeps
    // the central area transparent so the GL clear colour shows through when
    // nothing is docked there; every Begin()/End() in panels becomes a
    // dockable window automatically.
    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                 ImGuiDockNodeFlags_PassthruCentralNode);

    DrawMenuBar(*this);

    for (auto &p : panels)
    {
        if (p.open)
        {
            p.update(*this);
        }
    }
}

}
