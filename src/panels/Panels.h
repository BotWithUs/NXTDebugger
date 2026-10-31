#pragma once

namespace nxtdbg::app { struct App; }

namespace nxtdbg::panels
{

void DrawAttach           (app::App &);
void DrawPlayerState      (app::App &);
void DrawSnapshotInspector(app::App &);
void DrawEntityBrowser    (app::App &);
void DrawInventoryPanel   (app::App &);
void DrawEventTail        (app::App &);
void DrawCacheLookup      (app::App &);
void DrawCacheBrowser     (app::App &);
void DrawCs2Script        (app::App &);
void DrawRpcConsole       (app::App &);
void DrawRpcTap           (app::App &);
void DrawScriptContext    (app::App &);
void DrawInterfacePanel   (app::App &);
void DrawVarWatcher       (app::App &);
void DrawSessionHealth    (app::App &);
void DrawDebugDraw        (app::App &);
void DrawLogPanel         (app::App &);
void DrawLayersPanel      (app::App &);

// Registers the [NXTDebugger][Layers] imgui.ini section. Call before the
// first frame (the ini loads on the first NewFrame).
void RegisterLayerSettings(app::App &);

}
