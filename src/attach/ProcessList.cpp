#include "ProcessList.h"

#include <TlHelp32.h>
#include <algorithm>
#include <cwctype>

namespace nxtdbg::attach
{

namespace
{

std::wstring ToLower(std::wstring s)
{
    for (auto &c : s)
    {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return s;
}

bool IsPrimary(const std::wstring &lower)
{
    return lower.rfind(L"rs2client", 0) == 0 || lower.rfind(L"rs3client", 0) == 0;
}

bool IsFallback(const std::wstring &lower)
{
    return lower == L"runescape.exe" || lower == L"jagexlauncher.exe";
}

}

std::vector<ProcessEntry> ListGameProcesses()
{
    std::vector<ProcessEntry> primary;
    std::vector<ProcessEntry> fallback;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
    {
        return primary;
    }
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    if (Process32FirstW(snap, &e))
    {
        do
        {
            std::wstring name  = e.szExeFile;
            std::wstring lower = ToLower(name);
            if (IsPrimary(lower))
            {
                primary.push_back({ e.th32ProcessID, std::move(name) });
            }
            else if (IsFallback(lower))
            {
                fallback.push_back({ e.th32ProcessID, std::move(name) });
            }
        }
        while (Process32NextW(snap, &e));
    }
    CloseHandle(snap);

    if (!primary.empty())
    {
        return primary;
    }
    return fallback;
}

}
