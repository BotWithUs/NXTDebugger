#include "Session.h"

#include "log/Log.h"

#include <cstdio>

namespace nxtdbg::attach
{

namespace
{

uint32_t MagicLE(const uint8_t m[4])
{
    return uint32_t(m[0])
         | (uint32_t(m[1]) << 8)
         | (uint32_t(m[2]) << 16)
         | (uint32_t(m[3]) << 24);
}

}

Session::Session(Session &&o) noexcept
    : mapping_(o.mapping_), view_(o.view_), pid_(o.pid_)
{
    o.mapping_ = nullptr;
    o.view_    = nullptr;
    o.pid_     = 0;
}

Session &Session::operator=(Session &&o) noexcept
{
    if (this != &o)
    {
        Close();
        mapping_   = o.mapping_;
        view_      = o.view_;
        pid_       = o.pid_;
        o.mapping_ = nullptr;
        o.view_    = nullptr;
        o.pid_     = 0;
    }
    return *this;
}

Session::~Session()
{
    Close();
}

void Session::Close()
{
    if (view_)
    {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_)
    {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    pid_ = 0;
}

bool Session::Open(DWORD pid)
{
    Close();

    wchar_t name[64];
    swprintf_s(name, L"Local\\nxt_snapshot_%lu", pid);

    HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!m)
    {
        swprintf_s(lastErr_, L"OpenFileMappingW(%s) failed: %lu", name, GetLastError());
        log::LogError(lastErr_);
        return false;
    }
    void *v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (!v)
    {
        swprintf_s(lastErr_, L"MapViewOfFile failed: %lu", GetLastError());
        log::LogError(lastErr_);
        CloseHandle(m);
        return false;
    }

    auto *h = static_cast<const nxt::ipc::SharedHeader *>(v);
    uint32_t magic = MagicLE(h->magic);
    if (magic != nxt::ipc::kMagic)
    {
        swprintf_s(lastErr_, L"Magic mismatch: got 0x%08X expected 0x%08X",
                   magic, nxt::ipc::kMagic);
        log::LogError(lastErr_);
        UnmapViewOfFile(v);
        CloseHandle(m);
        return false;
    }
    if (h->version != nxt::ipc::kProtocolVersion)
    {
        swprintf_s(lastErr_,
                   L"Protocol version mismatch: agent=%u debugger=%u (rebuild the side that's behind)",
                   h->version, nxt::ipc::kProtocolVersion);
        log::LogError(lastErr_);
        UnmapViewOfFile(v);
        CloseHandle(m);
        return false;
    }

    mapping_ = m;
    view_    = v;
    pid_     = pid;
    swprintf_s(lastErr_, L"Attached to pid %lu (version %u)", pid, h->version);
    log::LogInfo(lastErr_);
    lastErr_[0] = 0;
    return true;
}

const nxt::ipc::SharedHeader *Session::Header() const
{
    return view_ ? static_cast<const nxt::ipc::SharedHeader *>(view_) : nullptr;
}

const nxt::ipc::EventRing *Session::Ring() const
{
    const auto *h = Header();
    if (!h || h->ringOff == 0)
    {
        return nullptr;
    }
    return reinterpret_cast<const nxt::ipc::EventRing *>(Base() + h->ringOff);
}

}
