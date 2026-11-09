#include "ctl/Control.h"

#include <shellapi.h>

#include <cwchar>
#include <string>

namespace nxtdbg::ctl
{

namespace
{

bool ParsePid(const wchar_t *s, DWORD &outPid)
{
    wchar_t      *end = nullptr;
    unsigned long v   = std::wcstoul(s, &end, 10);
    if (end == s || *end != 0 || v == 0)
    {
        return false;
    }
    outPid = static_cast<DWORD>(v);
    return true;
}

bool ParseAttach(const wchar_t *value, LaunchOptions &out, std::wstring &outError)
{
    if (!value)
    {
        outError = L"--attach needs a value: auto or a pid";
        return false;
    }
    if (std::wcscmp(value, L"auto") == 0)
    {
        out.attachMode = AttachMode::Auto;
        return true;
    }
    if (!ParsePid(value, out.attachPid))
    {
        outError = std::wstring(L"--attach: not a pid: ") + value;
        return false;
    }
    out.attachMode = AttachMode::Pid;
    return true;
}

}   // namespace

bool ParseLaunchOptions(const wchar_t *commandLine, LaunchOptions &outOptions,
                        std::wstring &outError)
{
    int     argc = 0;
    LPWSTR *argv = CommandLineToArgvW(commandLine, &argc);
    if (!argv)
    {
        outError = L"could not parse the command line";
        return false;
    }
    bool isOk = true;
    for (int i = 1; i < argc && isOk; ++i)
    {
        const std::wstring arg = argv[i];
        if (arg == L"--attach")
        {
            isOk = ParseAttach(i + 1 < argc ? argv[++i] : nullptr, outOptions, outError);
        }
        else if (arg.rfind(L"--attach=", 0) == 0)
        {
            isOk = ParseAttach(arg.c_str() + 9, outOptions, outError);
        }
        else if (arg == L"--ctl")
        {
            outOptions.isCtlEnabled = true;
        }
        else if (arg == L"--cs2-dir")
        {
            if (i + 1 >= argc)
            {
                outError = L"--cs2-dir needs a directory";
                isOk     = false;
            }
            else
            {
                outOptions.cs2Dir = argv[++i];
            }
        }
        else if (arg == L"--minimized")
        {
            outOptions.showCommand = SW_SHOWMINNOACTIVE;
        }
        else if (arg == L"--hidden" || arg == L"--no-window")
        {
            outOptions.showCommand      = SW_HIDE;
            outOptions.shouldSaveLayout = false;
        }
        else
        {
            outError = L"unknown option: " + arg
                     + L"\nusage: nxt_debugger [--attach auto|<pid>] [--ctl]"
                       L" [--minimized|--hidden] [--cs2-dir <path>]";
            isOk = false;
        }
    }
    LocalFree(argv);
    return isOk;
}

}
