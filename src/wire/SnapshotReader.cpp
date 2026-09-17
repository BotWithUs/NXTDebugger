#include "SnapshotReader.h"

#include <atomic>

// Holds wire/PROTOCOL.md's published offsets to the real layout. Header-only
// assertions, included here because this is the module that decodes them.
#include "wire/ProtocolDocPins.h"

namespace nxtdbg::wire
{

const nxt::ipc::Snapshot *CurrentSnapshot(const attach::Session &s)
{
    const auto *h = s.Header();
    if (!h)
    {
        return nullptr;
    }
    // Acquire-load frontIdx via atomic_ref so we synchronise with the
    // producer's InterlockedExchange release on publish.
    std::atomic_ref<const int32_t> front(h->frontIdx);
    int32_t idx = front.load(std::memory_order_acquire);
    if (idx != 0 && idx != 1)
    {
        return nullptr;
    }
    uint32_t off = (idx == 0) ? h->snapshotOff0 : h->snapshotOff1;
    return reinterpret_cast<const nxt::ipc::Snapshot *>(s.Base() + off);
}

}
