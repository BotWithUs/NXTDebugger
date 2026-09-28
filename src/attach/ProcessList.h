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

// The subset of rs2client*/rs3client* processes that currently publish an agent
// snapshot mapping (Local\nxt_snapshot_<pid>) -- i.e. the ones Attach would
// succeed on. Used by --attach auto, which attaches only when exactly one
// qualifies. The probe opens and immediately closes the mapping.
std::vector<ProcessEntry> ListAgentProcesses();

}
