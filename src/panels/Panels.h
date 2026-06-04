#pragma once

namespace nxtdbg::app { struct App; }

namespace nxtdbg::panels
{

void DrawAttach           (app::App &);
void DrawPlayerState      (app::App &);
void DrawSnapshotInspector(app::App &);
void DrawEntityBrowser    (app::App &);
void DrawEventTail        (app::App &);
void DrawCacheLookup      (app::App &);
void DrawRpcConsole       (app::App &);
void DrawRpcTap           (app::App &);
void DrawLogPanel         (app::App &);

}
