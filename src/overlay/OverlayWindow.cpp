#include "overlay/OverlayWindow.h"

#include "app/App.h"
#include "app/Theme.h"

#include "game/Interfaces.h"   // WireCategory (NXTLibrary, cross-project include)

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

#include <Windows.h>
#include <dwmapi.h>
#include <gl/GL.h>

#include <cstdint>
#include <cstdio>

namespace nxtdbg::overlay
{

namespace
{

constexpr wchar_t kOverlayClass[] = L"NXTOverlayWindow";

// Re-resolve the game window at most this often while we don't have a valid one.
constexpr DWORD kResolveThrottleMs = 250;

// --- WGL ARB pixel-format / context-creation entry points ------------------

using PfnChoosePixelFormat = BOOL  (WINAPI *)(HDC, const int *, const FLOAT *,
                                              UINT, int *, UINT *);
using PfnCreateContext     = HGLRC (WINAPI *)(HDC, HGLRC, const int *);
using PfnSwapInterval      = BOOL  (WINAPI *)(int);

constexpr int WGL_DRAW_TO_WINDOW_ARB        = 0x2001;
constexpr int WGL_SUPPORT_OPENGL_ARB        = 0x2010;
constexpr int WGL_DOUBLE_BUFFER_ARB         = 0x2011;
constexpr int WGL_PIXEL_TYPE_ARB            = 0x2013;
constexpr int WGL_TYPE_RGBA_ARB             = 0x202B;
constexpr int WGL_COLOR_BITS_ARB            = 0x2014;
constexpr int WGL_ALPHA_BITS_ARB            = 0x201B;
constexpr int WGL_DEPTH_BITS_ARB            = 0x2022;
constexpr int WGL_STENCIL_BITS_ARB          = 0x2023;
constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int WGL_CONTEXT_PROFILE_MASK_ARB  = 0x9126;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT  = 0x00000001;

// ---------------------------------------------------------------------------
// Window class
// ---------------------------------------------------------------------------

LRESULT CALLBACK OverlayWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
        case WM_NCHITTEST:
            return HTTRANSPARENT;   // every point falls through to the game
        case WM_ERASEBKGND:
            return 1;               // GL owns all pixels; skip the GDI erase
        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------------------
// GL bring-up — alpha-capable pixel format + 3.3 core context
// ---------------------------------------------------------------------------

bool BootOverlayGL(OverlayWindow &o)
{
    o.dc = GetDC(o.hwnd);
    if (!o.dc)
    {
        return false;
    }
    auto choosePf  = reinterpret_cast<PfnChoosePixelFormat>(
        wglGetProcAddress("wglChoosePixelFormatARB"));
    auto createCtx = reinterpret_cast<PfnCreateContext>(
        wglGetProcAddress("wglCreateContextAttribsARB"));
    if (!choosePf || !createCtx)
    {
        return false;
    }
    const int pfAttrs[] = {
        WGL_DRAW_TO_WINDOW_ARB, 1,
        WGL_SUPPORT_OPENGL_ARB, 1,
        WGL_DOUBLE_BUFFER_ARB,  1,
        WGL_PIXEL_TYPE_ARB,     WGL_TYPE_RGBA_ARB,
        WGL_COLOR_BITS_ARB,     32,
        WGL_ALPHA_BITS_ARB,     8,   // destination alpha — required for DWM compositing
        WGL_DEPTH_BITS_ARB,     24,
        WGL_STENCIL_BITS_ARB,   8,
        0,
    };
    int  pf      = 0;
    UINT formats = 0;
    if (!choosePf(o.dc, pfAttrs, nullptr, 1, &pf, &formats) || formats == 0)
    {
        return false;
    }
    PIXELFORMATDESCRIPTOR pfd{};
    DescribePixelFormat(o.dc, pf, sizeof(pfd), &pfd);
    if (!SetPixelFormat(o.dc, pf, &pfd))
    {
        return false;
    }
    const int ctxAttrs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT,
        0,
    };
    o.rc = createCtx(o.dc, nullptr, ctxAttrs);
    return o.rc != nullptr;
}

// Hand the window's framebuffer alpha to DWM for per-pixel transparency. An
// empty blur region (right < left) means "no blur" — the alpha channel is
// honoured straight, with antialiased box edges blending cleanly. This is the
// same recipe GLFW uses for a transparent framebuffer.
void EnablePerPixelAlpha(HWND hwnd)
{
    HRGN region = CreateRectRgn(0, 0, -1, -1);
    DWM_BLURBEHIND bb{};
    bb.dwFlags  = DWM_BB_ENABLE | DWM_BB_BLURREGION;
    bb.hRgnBlur = region;
    bb.fEnable  = TRUE;
    DwmEnableBlurBehindWindow(hwnd, &bb);
    DeleteObject(region);
}

// ---------------------------------------------------------------------------
// ImGui context (display-only — no Win32 platform backend)
// ---------------------------------------------------------------------------

void InitImGui(OverlayWindow &o)
{
    ImGuiContext *prevCtx = ImGui::GetCurrentContext();
    HDC   prevDc = wglGetCurrentDC();
    HGLRC prevRc = wglGetCurrentContext();

    wglMakeCurrent(o.dc, o.rc);
    o.ctx = ImGui::CreateContext();   // own font atlas (default font is plenty)
    ImGui::SetCurrentContext(o.ctx);

    ImGuiIO &io    = ImGui::GetIO();
    io.IniFilename = nullptr;          // overlay persists no layout
    io.LogFilename = nullptr;
    io.Fonts->AddFontDefault();
    ImGui_ImplOpenGL3_Init("#version 130");

    auto swapInterval = reinterpret_cast<PfnSwapInterval>(
        wglGetProcAddress("wglSwapIntervalEXT"));
    if (swapInterval)
    {
        swapInterval(0);   // no vsync — avoid stutter from a second SwapBuffers/frame
    }

    ImGui::SetCurrentContext(prevCtx);
    wglMakeCurrent(prevDc, prevRc);
}

// ---------------------------------------------------------------------------
// Game-window tracking
// ---------------------------------------------------------------------------

struct FindWinCtx
{
    DWORD pid;
    HWND  found;
};

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp)
{
    auto *ctx = reinterpret_cast<FindWinCtx *>(lp);
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid != ctx->pid)             { return TRUE; }
    if (!IsWindowVisible(hwnd))       { return TRUE; }
    if (GetWindow(hwnd, GW_OWNER))    { return TRUE; }
    if (GetWindowTextLengthW(hwnd) == 0) { return TRUE; }
    ctx->found = hwnd;
    return FALSE;   // first top-level visible titled window wins
}

HWND ResolveGameWindow(DWORD pid)
{
    FindWinCtx ctx{ pid, nullptr };
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// "Focused enough to show": the game itself, the overlay, or any window of our
// own debugger process (so the boxes stay up while the user reads the panel).
bool GameIsForeground(HWND gameHwnd, HWND overlayHwnd)
{
    HWND fg = GetForegroundWindow();
    if (fg == gameHwnd || fg == overlayHwnd)
    {
        return true;
    }
    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    return fgPid == GetCurrentProcessId();
}

void HideOverlay(OverlayWindow &o)
{
    if (o.shown)
    {
        ShowWindow(o.hwnd, SW_HIDE);
        o.shown = false;
    }
}

// Resolve / reposition the overlay over the game's client area. Returns false
// (after hiding) when there's nothing to draw this frame; on success fills the
// client width/height.
bool UpdateTracking(OverlayWindow &o, bool followForeground, int &outW, int &outH)
{
    if (!o.gameHwnd || !IsWindow(o.gameHwnd))
    {
        DWORD now = GetTickCount();
        if (now - o.lastResolveTick >= kResolveThrottleMs)
        {
            o.lastResolveTick = now;
            o.gameHwnd        = ResolveGameWindow(o.gamePid);
        }
    }
    if (!o.gameHwnd || !IsWindow(o.gameHwnd) || IsIconic(o.gameHwnd)
        || !IsWindowVisible(o.gameHwnd)
        || (followForeground && !GameIsForeground(o.gameHwnd, o.hwnd)))
    {
        HideOverlay(o);
        return false;
    }
    RECT  cr{};
    POINT origin{ 0, 0 };
    GetClientRect(o.gameHwnd, &cr);
    ClientToScreen(o.gameHwnd, &origin);
    outW = cr.right - cr.left;
    outH = cr.bottom - cr.top;
    if (outW <= 0 || outH <= 0)
    {
        HideOverlay(o);
        return false;
    }
    SetWindowPos(o.hwnd, HWND_TOPMOST, origin.x, origin.y, outW, outH,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    if (!o.shown)
    {
        ShowWindow(o.hwnd, SW_SHOWNOACTIVATE);
        o.shown = true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Box rendering
// ---------------------------------------------------------------------------

void StyleForCategory(int32_t category, bool isHover, ImU32 &outCol, float &outThick)
{
    using nxt::game::interfaces::WireCategory;
    if (isHover)
    {
        outCol   = theme::kAccent;
        outThick = 2.5f;
        return;
    }
    switch (static_cast<WireCategory>(category))
    {
        case WireCategory::Layer:  outCol = theme::kBorderHi; outThick = 1.0f; break;
        case WireCategory::Button: outCol = theme::kAccent;   outThick = 1.5f; break;
        case WireCategory::Text:   outCol = theme::kInfo;     outThick = 1.0f; break;
        case WireCategory::Sprite:
        case WireCategory::Model:  outCol = theme::kGood;     outThick = 1.0f; break;
        default:                   outCol = theme::kTextDim;  outThick = 1.0f; break;
    }
}

void DrawOneBox(ImDrawList *dl, const app::OverlayBox &b, bool showLabel, bool isHover)
{
    ImVec2 p0(static_cast<float>(b.x), static_cast<float>(b.y));
    ImVec2 p1(static_cast<float>(b.x + b.w), static_cast<float>(b.y + b.h));

    ImU32 col   = theme::kTextDim;
    float thick = 1.0f;
    StyleForCategory(b.category, isHover, col, thick);

    if (isHover)
    {
        dl->AddRectFilled(p0, p1, (theme::kAccent & 0x00FFFFFFu) | (40u << 24));
    }
    dl->AddRect(p0, p1, col, 0.0f, 0, thick);

    const bool big = (b.w >= 36 && b.h >= 14);
    if (b.label[0] && (isHover || (showLabel && big)))
    {
        ImVec2 at(p0.x + 3.0f, p0.y + 1.0f);
        dl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 200), b.label);
        dl->AddText(at, theme::kTextHi, b.label);
    }
}

void DrawHint(ImDrawList *dl, const app::InterfaceView &v)
{
    const char *msg = (v.iface < 0)
        ? "NXTDebugger overlay: select an interface in the Interfaces panel"
        : "NXTDebugger overlay: no visible components";
    dl->AddText(ImVec2(9.0f, 9.0f), IM_COL32(0, 0, 0, 200), msg);
    dl->AddText(ImVec2(8.0f, 8.0f), theme::kAccent, msg);
}

void DrawBoxes(app::App &a)
{
    const app::InterfaceView &v = a.interfaceView;
    ImDrawList *dl = ImGui::GetForegroundDrawList();

    if (v.boxes.empty())
    {
        DrawHint(dl, v);
        return;
    }
    // Containers first (BFS order already puts parents before leaves), then the
    // hovered box last so it sits on top of everything it overlaps.
    int hoverIdx = -1;
    for (size_t i = 0; i < v.boxes.size(); ++i)
    {
        const app::OverlayBox &b = v.boxes[i];
        if (b.hidden == 1)
        {
            continue;
        }
        if (v.hoverComp >= 0 && b.comp == v.hoverComp && b.sub == v.hoverSub)
        {
            hoverIdx = static_cast<int>(i);
            continue;
        }
        DrawOneBox(dl, b, a.overlayShowAllLabels, false);
    }
    if (hoverIdx >= 0)
    {
        DrawOneBox(dl, v.boxes[static_cast<size_t>(hoverIdx)], true, true);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool RegisterWindowClass(HINSTANCE inst)
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_OWNDC;
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = inst;
    wc.lpszClassName = kOverlayClass;
    return RegisterClassExW(&wc) != 0;
}

void UnregisterWindowClass(HINSTANCE inst)
{
    UnregisterClassW(kOverlayClass, inst);
}

bool IsCreated(const OverlayWindow &o)
{
    return o.hwnd != nullptr;
}

bool Create(OverlayWindow &o, HINSTANCE inst, DWORD gamePid)
{
    o = OverlayWindow{};
    o.gamePid = gamePid;

    o.hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kOverlayClass, L"NXTDebugger Overlay", WS_POPUP,
        0, 0, 16, 16, nullptr, nullptr, inst, nullptr);
    if (!o.hwnd)
    {
        return false;
    }
    if (!BootOverlayGL(o))
    {
        Destroy(o);
        return false;
    }
    EnablePerPixelAlpha(o.hwnd);
    InitImGui(o);
    return true;
}

void RenderFrame(OverlayWindow &o, app::App &a)
{
    if (!o.hwnd || !o.ctx)
    {
        return;   // never created (or a failed Create) — nothing to drive
    }
    const float dtIn = ImGui::GetIO().DeltaTime;   // main context is current here

    int w = 0;
    int h = 0;
    if (!UpdateTracking(o, a.overlayFollowFg, w, h))
    {
        return;   // nothing to draw this frame; window already hidden
    }

    ImGuiContext *prevCtx = ImGui::GetCurrentContext();
    HDC   prevDc = wglGetCurrentDC();
    HGLRC prevRc = wglGetCurrentContext();

    wglMakeCurrent(o.dc, o.rc);
    ImGui::SetCurrentContext(o.ctx);

    ImGuiIO &io    = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));
    io.DeltaTime   = (dtIn > 0.0f) ? dtIn : (1.0f / 60.0f);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    DrawBoxes(a);
    ImGui::Render();

    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);   // fully transparent — DWM sees through
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SwapBuffers(o.dc);

    ImGui::SetCurrentContext(prevCtx);
    wglMakeCurrent(prevDc, prevRc);
}

void Destroy(OverlayWindow &o)
{
    if (o.ctx)
    {
        ImGuiContext *prevCtx = ImGui::GetCurrentContext();
        HDC   prevDc = wglGetCurrentDC();
        HGLRC prevRc = wglGetCurrentContext();

        wglMakeCurrent(o.dc, o.rc);
        ImGui::SetCurrentContext(o.ctx);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext(o.ctx);

        // Restore the caller's context BEFORE deleting the overlay GL context.
        ImGui::SetCurrentContext(prevCtx == o.ctx ? nullptr : prevCtx);
        wglMakeCurrent(prevDc, prevRc);
    }
    if (o.rc)
    {
        wglDeleteContext(o.rc);
    }
    if (o.dc && o.hwnd)
    {
        ReleaseDC(o.hwnd, o.dc);
    }
    if (o.hwnd)
    {
        DestroyWindow(o.hwnd);
    }
    o = OverlayWindow{};
}

}
