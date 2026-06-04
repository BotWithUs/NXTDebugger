#pragma once
#include "attach/Session.h"
#include "ipc/Events.h"
#include "ipc/SharedLayout.h"

#include <cstdint>
#include <vector>

namespace nxtdbg::wire
{

struct EventRecord
{
    uint64_t                seq;
    nxt::ipc::EventType     type;
    uint32_t                bodyLen;
    uint8_t                 body[nxt::ipc::kEventBodyMax];
};

// Tracks the last sequence drained per session. Construct fresh per session.
class EventReader
{
public:
    void Reset() { lastSeq_ = 0; dropped_ = 0; primed_ = false; }

    // Drains all newly-published events from the ring. Returns through out
    // (cleared on entry). dropCount() is the running total of detected drops.
    void Drain(const attach::Session &s, std::vector<EventRecord> &out);

    uint64_t LastSeq()   const { return lastSeq_;  }
    uint64_t DropCount() const { return dropped_;  }

private:
    uint64_t lastSeq_ = 0;
    uint64_t dropped_ = 0;
    bool     primed_  = false;  // first drain skips backlog
};

const char *EventTypeName(nxt::ipc::EventType t);

}
