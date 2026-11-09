#include "EventFormat.h"

#include "ipc/Events.h"

#include <cstdio>

namespace nxtdbg::wire
{

void FormatEventBody(const EventRecord &r, char *out, size_t n)
{
    using namespace nxt::ipc;
    out[0] = 0;
    switch (r.type)
    {
        case kEventLoginStateChange:
        {
            if (r.bodyLen < sizeof(LoginStateChangeBody)) break;
            auto *b = reinterpret_cast<const LoginStateChangeBody *>(r.body);
            std::snprintf(out, n, "%d -> %d", b->oldState, b->newState);
            break;
        }
        case kEventTick:
        {
            if (r.bodyLen < sizeof(TickBody)) break;
            auto *b = reinterpret_cast<const TickBody *>(r.body);
            std::snprintf(out, n, "%d", b->tick);
            break;
        }
        case kEventTokenRefreshFired:
        {
            if (r.bodyLen < sizeof(TokenRefreshFiredBody)) break;
            auto *b = reinterpret_cast<const TokenRefreshFiredBody *>(r.body);
            std::snprintf(out, n, "expiry %llu  (%lld s left at fire)",
                          static_cast<unsigned long long>(b->expirySec),
                          static_cast<long long>(b->secondsRemainingAtFire));
            break;
        }
        case kEventTokenRefreshed:
        {
            if (r.bodyLen < sizeof(TokenRefreshedBody)) break;
            auto *b = reinterpret_cast<const TokenRefreshedBody *>(r.body);
            std::snprintf(out, n, "expiry %llu  (+%lld s)",
                          static_cast<unsigned long long>(b->expirySec),
                          static_cast<long long>(b->secondsUntilExpiry));
            break;
        }
        case kEventTokenRefreshFailed:
            std::snprintf(out, n, "refresh failed");   // body-less (slot.bodyLen == 0)
            break;
        case kEventVarChange:
        case kEventVarbitChange:
        case kEventVarcChange:
        {
            if (r.bodyLen < sizeof(VarChangeBody)) break;
            auto *b = reinterpret_cast<const VarChangeBody *>(r.body);
            std::snprintf(out, n, "id %d  %d -> %d", b->varpId, b->oldValue, b->newValue);
            break;
        }
        case kEventObjVarChange:
        {
            if (r.bodyLen < sizeof(ObjVarChangeBody)) break;
            auto *b = reinterpret_cast<const ObjVarChangeBody *>(r.body);
            std::snprintf(out, n, "inv %d  slot %d  var %d  %d -> %d",
                          b->invId, b->slot, b->varId, b->oldValue, b->newValue);
            break;
        }
        case kEventChatMessage:
        {
            if (r.bodyLen < sizeof(ChatMessageBody)) break;
            auto *b = reinterpret_cast<const ChatMessageBody *>(r.body);
            uint16_t sl = b->senderLen;
            uint16_t tl = b->textLen;
            if (size_t(sl) + tl > sizeof(b->buf)) break;
            std::snprintf(out, n, "[%d] %.*s: %.*s",
                          b->msgType, int(sl), reinterpret_cast<const char *>(b->buf),
                          int(tl), reinterpret_cast<const char *>(b->buf) + sl);
            break;
        }
        case kEventKeyInput:
        {
            if (r.bodyLen < sizeof(KeyInputBody)) break;
            auto *b = reinterpret_cast<const KeyInputBody *>(r.body);
            std::snprintf(out, n, "vk=%u %s%s%s", b->key,
                          b->isCtrl  ? "ctrl "  : "",
                          b->isAlt   ? "alt "   : "",
                          b->isShift ? "shift " : "");
            break;
        }
        case kEventActionExecuted:
        {
            if (r.bodyLen < sizeof(ActionExecutedBody)) break;
            auto *b = reinterpret_cast<const ActionExecutedBody *>(r.body);
            std::snprintf(out, n, "action %d  (%d, %d, %d)",
                          b->actionId, b->param1, b->param2, b->param3);
            break;
        }
        case kEventBreakStarted:
        {
            if (r.bodyLen < sizeof(BreakStartedBody)) break;
            auto *b = reinterpret_cast<const BreakStartedBody *>(r.body);
            std::snprintf(out, n, "%ds  fatigue=%.2f  risk=%.2f",
                          b->durationSeconds, b->fatigue, b->risk);
            break;
        }
        case kEventWalkArrived:
        case kEventWalkCancelled:
        case kEventWalkFailed:
        {
            if (r.bodyLen < sizeof(WalkBody)) break;
            auto *b = reinterpret_cast<const WalkBody *>(r.body);
            std::snprintf(out, n, "(%d, %d)", b->targetX, b->targetY);
            break;
        }
        case kEventHitmark:
        {
            if (r.bodyLen < sizeof(HitmarkBody)) break;
            auto *b = reinterpret_cast<const HitmarkBody *>(r.body);
            std::snprintf(out, n, "tgt %d/%d  dmg %d  type %d",
                          b->targetServerIndex, int(b->targetType),
                          b->damage, b->hitmarkType);
            break;
        }
        case kEventHeadbar:
        {
            if (r.bodyLen < sizeof(HeadbarBody)) break;
            auto *b = reinterpret_cast<const HeadbarBody *>(r.body);
            std::snprintf(out, n, "tgt %d/%d  type %d  val %d",
                          b->targetServerIndex, int(b->targetType),
                          b->headbarType, b->value);
            break;
        }
        case kEventSpotAnim:
        {
            if (r.bodyLen < sizeof(SpotAnimBody)) break;
            auto *b = reinterpret_cast<const SpotAnimBody *>(r.body);
            if (b->targetServerIndex >= 0)
            {
                std::snprintf(out, n, "tgt %d/%d  gfx %d",
                              b->targetServerIndex, int(b->targetType), b->spotAnimId);
            }
            else
            {
                std::snprintf(out, n, "(%d, %d) p%d  gfx %d",
                              b->tileX, b->tileY, int(b->plane), b->spotAnimId);
            }
            break;
        }
        case kEventRadioGroupSelect:
        {
            if (r.bodyLen < sizeof(RadioGroupSelectBody)) break;
            auto *b = reinterpret_cast<const RadioGroupSelectBody *>(r.body);
            std::snprintf(out, n, "iface %d  comp %d  sub %d  val %d  op %d",
                          b->ifaceId, b->componentId, b->subId, b->value, b->opcode);
            break;
        }
        default:
            std::snprintf(out, n, "(%u bytes)", r.bodyLen);
    }
}

}
