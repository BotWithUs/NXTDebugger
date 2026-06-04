#include "app/App.h"
#include "app/Theme.h"
#include "log/Log.h"

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_win32.h"
#include "misc/freetype/imgui_freetype.h"

#include <Windows.h>
#include <ShlObj.h>
#include <gl/GL.h>

#include <cstdio>
#include <string>

// Forward-declared by ImGui's Win32 backend; the macro avoids dragging the
// backend's full header into this TU.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{

constexpr wchar_t kClassName[] = L"NXTDebuggerWindow";
constexpr wchar_t kTitle[]     = L"NXTDebugger";

struct WGLBoot
{
    HWND  hwnd = nullptr;
    HDC   dc   = nullptr;
    HGLRC rc   = nullptr;
};

using PFNWGLCREATECTXATTR = HGLRC(WINAPI *)(HDC, HGLRC, const int *);

constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int WGL_CONTEXT_PROFILE_MASK_ARB  = 0x9126;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT  = 0x00000001;
constexpr int WGL_CONTEXT_FLAGS_ARB         = 0x2094;
constexpr int WGL_CONTEXT_DEBUG_BIT_ARB     = 0x00000001;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l))
    {
        return 1;
    }
    switch (m)
    {
        case WM_SIZE:
            glViewport(0, 0, LOWORD(l), HIWORD(l));
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_SYSCOMMAND:
            if ((w & 0xFFF0) == SC_KEYMENU) return 0;
            break;
    }
    return DefWindowProcW(h, m, w, l);
}

bool RegisterWindowClass(HINSTANCE inst)
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    return RegisterClassExW(&wc) != 0;
}

int SetBasicPixelFormat(HDC dc)
{
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize        = sizeof(pfd);
    pfd.nVersion     = 1;
    pfd.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType   = PFD_TYPE_RGBA;
    pfd.cColorBits   = 32;
    pfd.cDepthBits   = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType   = PFD_MAIN_PLANE;

    int idx = ChoosePixelFormat(dc, &pfd);
    if (!idx)
    {
        return 0;
    }
    if (!SetPixelFormat(dc, idx, &pfd))
    {
        return 0;
    }
    return idx;
}

bool BootWGL(WGLBoot &b)
{
    b.dc = GetDC(b.hwnd);
    if (!b.dc || !SetBasicPixelFormat(b.dc))
    {
        return false;
    }
    HGLRC legacy = wglCreateContext(b.dc);
    if (!legacy || !wglMakeCurrent(b.dc, legacy))
    {
        return false;
    }
    auto create = reinterpret_cast<PFNWGLCREATECTXATTR>(
        wglGetProcAddress("wglCreateContextAttribsARB"));
    if (!create)
    {
        b.rc = legacy;
        return true;  // fall back to legacy ctx (still good enough for ImGui)
    }
    const int attrs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT,
        0,
    };
    HGLRC core = create(b.dc, nullptr, attrs);
    if (!core)
    {
        b.rc = legacy;
        return true;
    }
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(legacy);
    wglMakeCurrent(b.dc, core);
    b.rc = core;
    return true;
}

void ShutdownWGL(WGLBoot &b)
{
    if (b.rc)
    {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(b.rc);
        b.rc = nullptr;
    }
    if (b.dc && b.hwnd)
    {
        ReleaseDC(b.hwnd, b.dc);
        b.dc = nullptr;
    }
}

std::string ResolveIniPath()
{
    wchar_t local[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local)))
    {
        return "imgui.ini";
    }
    wchar_t dir[MAX_PATH];
    std::swprintf(dir, MAX_PATH, L"%s\\NXTDebugger", local);
    CreateDirectoryW(dir, nullptr);

    wchar_t path[MAX_PATH];
    std::swprintf(path, MAX_PATH, L"%s\\imgui.ini", dir);

    char utf8[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
    return utf8;
}

// Build an absolute path to the named Windows font (e.g. "segoeui.ttf"). Uses
// GetWindowsDirectoryW so a non-default Windows install root still resolves.
std::string ResolveWindowsFontPath(const wchar_t *fontFileName)
{
    wchar_t winDir[MAX_PATH];
    UINT got = GetWindowsDirectoryW(winDir, MAX_PATH);
    if (got == 0 || got >= MAX_PATH) return {};
    wchar_t path[MAX_PATH];
    std::swprintf(path, MAX_PATH, L"%s\\Fonts\\%s", winDir, fontFileName);
    char utf8[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
    return utf8;
}

// Returns the DPI scale factor of the system. System DPI awareness is enough
// for a single-window tool — we don't need per-monitor reconfiguration.
float QuerySystemDpiScale()
{
    HDC screen = GetDC(nullptr);
    if (!screen) return 1.0f;
    int dpiX = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    return (dpiX > 0) ? static_cast<float>(dpiX) / 96.0f : 1.0f;
}

void InstallFreetypeFont(ImGuiIO &io, float dpiScale)
{
    io.Fonts->FontBuilderIO    = ImGuiFreeType::GetBuilderForFreeType();
    io.Fonts->FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH  = false;

    // Base size 16 looks crisp at 100% DPI; scale up linearly for hi-DPI.
    const float baseSize = 16.0f;
    const float pixSize  = baseSize * dpiScale;

    std::string segoe = ResolveWindowsFontPath(L"segoeui.ttf");
    if (!segoe.empty() &&
        io.Fonts->AddFontFromFileTTF(segoe.c_str(), pixSize, &cfg) != nullptr)
    {
        return;
    }
    // Fallback chain: Consolas (universal Windows install) → ImGui default.
    std::string consolas = ResolveWindowsFontPath(L"consola.ttf");
    if (!consolas.empty() &&
        io.Fonts->AddFontFromFileTTF(consolas.c_str(), pixSize, &cfg) != nullptr)
    {
        return;
    }
    io.Fonts->AddFontDefault();
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
    // System DPI awareness — the OS tells us the scale once at startup and
    // doesn't auto-blur our window. We size the font for that scale below.
    // Safe to call even on Windows versions without the API; the fallback
    // SetProcessDPIAware is on every Win10+ host.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);

    if (!RegisterWindowClass(inst))
    {
        MessageBoxW(nullptr, L"RegisterClassExW failed", kTitle, MB_OK | MB_ICONERROR);
        return 1;
    }

    const float dpiScale = QuerySystemDpiScale();
    const int   winW = static_cast<int>(1480 * dpiScale);
    const int   winH = static_cast<int>(920  * dpiScale);

    WGLBoot boot{};
    boot.hwnd = CreateWindowExW(
        0, kClassName, kTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, winW, winH,
        nullptr, nullptr, inst, nullptr);
    if (!boot.hwnd)
    {
        MessageBoxW(nullptr, L"CreateWindowExW failed", kTitle, MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!BootWGL(boot))
    {
        MessageBoxW(nullptr, L"WGL bootstrap failed", kTitle, MB_OK | MB_ICONERROR);
        DestroyWindow(boot.hwnd);
        return 1;
    }

    ShowWindow(boot.hwnd, SW_SHOWDEFAULT);
    UpdateWindow(boot.hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    static std::string iniPath = ResolveIniPath();
    io.IniFilename = iniPath.c_str();

    InstallFreetypeFont(io, dpiScale);

    ImGui_ImplWin32_Init(boot.hwnd);
    ImGui_ImplOpenGL3_Init("#version 130");

    nxtdbg::log::LogInfo(L"NXTDebugger ready");
    nxtdbg::app::App app;
    app.Init();

    bool running = true;
    while (running)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
            {
                running = false;
            }
        }
        if (!running) break;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        app.Update();

        ImGui::Render();
        ImVec4 clear = ImGui::ColorConvertU32ToFloat4(nxtdbg::theme::kBg);
        glClearColor(clear.x, clear.y, clear.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SwapBuffers(boot.dc);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ShutdownWGL(boot);
    DestroyWindow(boot.hwnd);
    UnregisterClassW(kClassName, inst);
    return 0;
}
