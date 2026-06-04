#include "Log.h"

#include <Windows.h>
#include <cwchar>
#include <mutex>

namespace nxtdbg::log
{

namespace
{

constexpr uint32_t kCap = 512;

struct Ring
{
    Entry     entries[kCap];
    uint32_t  count = 0;       // grows up to kCap then stays
    uint32_t  head  = 0;       // next write slot
    uint64_t  seq   = 0;
    std::mutex mu;
};

Ring &GetRing()
{
    static Ring r;
    return r;
}

void Push(Level lvl, const wchar_t *msg)
{
    Ring &r = GetRing();
    std::lock_guard lock(r.mu);
    Entry &e = r.entries[r.head];
    e.level = lvl;
    e.seq   = ++r.seq;
    wcsncpy_s(e.text, msg, _TRUNCATE);
    r.head = (r.head + 1) % kCap;
    if (r.count < kCap)
    {
        ++r.count;
    }
    OutputDebugStringW(msg);
    OutputDebugStringW(L"\n");
}

}

void LogInfo(const wchar_t *msg)  { Push(Level::Info,  msg); }
void LogWarn(const wchar_t *msg)  { Push(Level::Warn,  msg); }
void LogError(const wchar_t *msg) { Push(Level::Error, msg); }

uint32_t Snapshot(Entry *out, uint32_t cap)
{
    Ring &r = GetRing();
    std::lock_guard lock(r.mu);
    uint32_t n     = (r.count < cap) ? r.count : cap;
    uint32_t start = (r.head + kCap - n) % kCap;
    for (uint32_t i = 0; i < n; ++i)
    {
        out[i] = r.entries[(start + i) % kCap];
    }
    return n;
}

}
