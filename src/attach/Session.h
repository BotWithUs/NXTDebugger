#pragma once
#include <Windows.h>
#include <cstdint>

#include "ipc/SharedLayout.h"

namespace nxtdbg::attach
{

// Compile-time pin: a consumer-side build that drifted from the producer
// fails to build instead of misreading bytes at runtime. Bumped together
// with kProtocolVersion in NXTLibrary/src/ipc/SharedLayout.h.
static_assert(nxt::ipc::kProtocolVersion == 14,
              "Rebuild NXTDebugger against current NXTLibrary wire");

// RAII over Local\nxt_snapshot_<pid>. Move-only.
class Session
{
public:
    Session() = default;
    Session(const Session &)            = delete;
    Session &operator=(const Session &) = delete;
    Session(Session &&other) noexcept;
    Session &operator=(Session &&other) noexcept;
    ~Session();

    // Returns true on success. On failure call LastError() for a wide message.
    bool Open(DWORD pid);
    void Close();

    bool IsOpen() const { return view_ != nullptr; }
    DWORD Pid()   const { return pid_; }

    const nxt::ipc::SharedHeader *Header() const;
    const nxt::ipc::EventRing    *Ring()   const;
    const wchar_t                *LastError() const { return lastErr_; }

    // Returns the byte view (for SnapshotReader / EventReader).
    const uint8_t *Base() const { return static_cast<const uint8_t *>(view_); }

private:
    HANDLE  mapping_ = nullptr;
    void   *view_    = nullptr;
    DWORD   pid_     = 0;
    wchar_t lastErr_[256]{};
};

}
