#pragma once
#include <cstdint>

namespace nxtdbg::log
{

enum class Level : uint8_t
{
    Info,
    Warn,
    Error,
};

struct Entry
{
    Level     level;
    uint64_t  seq;
    wchar_t   text[256];
};

void LogInfo(const wchar_t *msg);
void LogWarn(const wchar_t *msg);
void LogError(const wchar_t *msg);

// Snapshot up to `cap` most-recent entries into `out`. Returns the number
// written. Stable order: oldest first. Lock-free single-reader-friendly.
uint32_t Snapshot(Entry *out, uint32_t cap);

}
