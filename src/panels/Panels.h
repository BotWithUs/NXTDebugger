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
void DrawLogPanel         (app::App &);

}
