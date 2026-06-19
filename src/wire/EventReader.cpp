#include "EventReader.h"

#include <atomic>
#include <cstring>

namespace nxtdbg::wire
{

using nxt::ipc::EventRing;
using nxt::ipc::EventSlot;
using nxt::ipc::EventType;

void EventReader::Drain(const attach::Session &s, std::vector<EventRecord> &out)
{
    out.clear();
    const EventRing *ring = s.Ring();
    if (!ring || ring->slotCount == 0)
    {
        return;
    }

    std::atomic_ref<const uint64_t> headRef(ring->head);
    uint64_t head = headRef.load(std::memory_order_acquire);

    if (!primed_)
    {
        lastSeq_ = head;
        primed_  = true;
        return;
    }

    uint32_t mask = ring->slotMask;
    for (uint64_t seq = lastSeq_; seq < head; ++seq)
    {
        const EventSlot &slot = ring->slots[seq & mask];
        std::atomic_ref<const uint64_t> slotSeq(slot.seq);
        uint64_t s2 = slotSeq.load(std::memory_order_acquire);
        if (s2 == seq)
        {
            EventRecord rec{};
            rec.seq     = seq;
            rec.type    = static_cast<EventType>(slot.type);
            rec.bodyLen = (slot.bodyLen <= nxt::ipc::kEventBodyMax)
                              ? slot.bodyLen
                              : nxt::ipc::kEventBodyMax;
            std::memcpy(rec.body, slot.body, rec.bodyLen);
            out.push_back(rec);
        }
        else if (s2 > seq)
        {
            dropped_ += (s2 - seq);
            seq      = s2 - 1;  // resume from the slot's actual seq next iter
        }
        else
        {
            break;  // writer hasn't committed yet; retry next tick
        }
    }
    lastSeq_ = head;
}

const char *EventTypeName(EventType t)
{
    using namespace nxt::ipc;
    switch (t)
    {
        case kEventNone:             return "none";
        case kEventLoginStateChange: return "login_state";
        case kEventTick:             return "tick";
        case kEventVarChange:        return "var";
        case kEventVarbitChange:     return "varbit";
        case kEventVarcChange:       return "varc";
        case kEventChatMessage:      return "chat";
        case kEventKeyInput:         return "key";
        case kEventActionExecuted:   return "action";
        case kEventBreakStarted:     return "break_start";
        case kEventBreakEnded:       return "break_end";
        case kEventWalkArrived:      return "walk_arrived";
        case kEventWalkCancelled:    return "walk_cancelled";
        case kEventWalkFailed:       return "walk_failed";
        case kEventHitmark:          return "hitmark";
        case kEventHeadbar:          return "headbar";
        case kEventSpotAnim:         return "spotanim";
        case kEventRadioGroupSelect: return "radio";
        default:                     return "?";
    }
}

}
