#pragma once
#include <Windows.h>
#include <string>
#include <vector>

namespace nxtdbg::attach
{

struct ProcessEntry
{
    DWORD        pid;
    std::wstring exeName;
};

// Enumerates candidate game processes. Returns rs2client*.exe / rs3client*.exe
// entries first; if none found, returns RuneScape.exe / jagexlauncher.exe as
// a fallback so the user sees something useful pre-game-start.
std::vector<ProcessEntry> ListGameProcesses();

}
