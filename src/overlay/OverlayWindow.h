#pragma once
#include <Windows.h>

struct ImGuiContext;

namespace nxtdbg::app { struct App; }

namespace nxtdbg::overlay
{

// A transparent, topmost, click-through window that paints interface-component
// bounding boxes on top of the live game. It owns its own OpenGL + ImGui
// context, fully separate from the main debugger window, and tracks the game
// window's client area each frame so the boxes line up pixel-for-pixel.
//
// Display-only: WS_EX_TRANSPARENT routes every click to the game beneath, and
// the overlay reads app::App::interfaceView read-only — it opens no pipe and
// issues no RPC.
struct OverlayWindow
{
    HWND          hwnd     = nullptr;
    HDC           dc       = nullptr;
    HGLRC         rc       = nullptr;
    ImGuiContext *ctx      = nullptr;

    DWORD         gamePid         = 0;
    HWND          gameHwnd        = nullptr;
    DWORD         lastResolveTick = 0;
    bool          shown           = false;
};

// Register / unregister the overlay window class. Call once at startup and once
// at shutdown — symmetric with the main window class. RegisterWindowClass
// returns false on failure (Create will then fail too).
bool RegisterWindowClass(HINSTANCE inst);
void UnregisterWindowClass(HINSTANCE inst);

bool IsCreated(const OverlayWindow &o);

// Build the window + GL context + ImGui context for `gamePid`. Must be called
// with the main GL context current (used to resolve the WGL ARB entry points).
// Returns false and leaves `o` cleared on failure.
bool Create(OverlayWindow &o, HINSTANCE inst, DWORD gamePid);

// Track the game window and paint one overlay frame. Captures and restores the
// caller's GL + ImGui current state, so the main render loop is unaffected.
void RenderFrame(OverlayWindow &o, app::App &a);

// Tear down the ImGui context, GL context, and window. Safe to call on a
// partially-constructed `o` (used as Create's failure cleanup).
void Destroy(OverlayWindow &o);

}
