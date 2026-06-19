#include "Panels.h"

#include "app/App.h"
#include "app/Theme.h"
#include "ipc/Events.h"
#include "wire/EventReader.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>

namespace nxtdbg::panels
{

namespace
{

ImU32 EventColor(nxt::ipc::EventType t)
{
    using namespace nxt::ipc;
    switch (t)
    {
        case kEventTick:             return theme::kTextDim;
        case kEventLoginStateChange: return theme::kInfo;
        case kEventVarChange:
        case kEventVarbitChange:
        case kEventVarcChange:       return theme::kInfo;
        case kEventChatMessage:      return theme::kTextHi;
        case kEventKeyInput:         return theme::kAccent;
        case kEventActionExecuted:   return theme::kAccent;
        case kEventBreakStarted:
        case kEventBreakEnded:       return theme::kWarn;
        case kEventWalkArrived:
        case kEventWalkCancelled:
        case kEventWalkFailed:       return theme::kGood;
        case kEventHitmark:
        case kEventHeadbar:          return theme::kBad;
        case kEventSpotAnim:         return theme::kAccent;
        case kEventRadioGroupSelect: return theme::kInfo;
        default:                     return theme::kTextDim;
    }
}

void FormatBody(const wire::EventRecord &r, char *out, size_t n)
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
        case kEventVarChange:
        case kEventVarbitChange:
        case kEventVarcChange:
        {
            if (r.bodyLen < sizeof(VarChangeBody)) break;
            auto *b = reinterpret_cast<const VarChangeBody *>(r.body);
            std::snprintf(out, n, "id %d  %d -> %d", b->varpId, b->oldValue, b->newValue);
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

void DrawControlBar(app::App &a, size_t shown, size_t total)
{
    if (!theme::BeginCard("evt.bar", "STREAM"))
    {
        theme::EndCard();
        return;
    }

    theme::StatusDot(a.paused ? theme::kWarn : theme::kGood,
                     !a.paused, a.dotPhase);
    ImGui::SameLine();

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%zu of %zu", shown, total);
    theme::HeroStat("events", buf, theme::kTextHi);

    ImGui::SameLine(0, ImGui::GetFontSize() * 2);
    ImGui::BeginGroup();
    theme::Subheading("auto-scroll");
    theme::Toggle("##as", &a.autoScrollEvts);
    ImGui::EndGroup();

    ImGui::SameLine(0, ImGui::GetFontSize() * 1.5f);
    ImGui::BeginGroup();
    theme::Subheading("paused");
    theme::Toggle("##pp", &a.paused);
    ImGui::EndGroup();

    ImGui::SameLine(0, ImGui::GetFontSize() * 1.5f);
    if (ImGui::Button("Clear"))
    {
        a.eventBacklog.clear();
    }

    uint64_t drops = a.events.DropCount();
    if (drops)
    {
        ImGui::SameLine();
        char drop[32];
        std::snprintf(drop, sizeof(drop), "%llu drops",
                      static_cast<unsigned long long>(drops));
        theme::Pill(drop, theme::kBad);
    }

    theme::EndCard();
}

}

void DrawEventTail(app::App &a)
{
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 60, ImGui::GetFontSize() * 32),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Events"))
    {
        ImGui::End();
        return;
    }

    DrawControlBar(a, a.eventBacklog.size(), a.eventBacklog.size());
    ImGui::Spacing();

    const float fs = ImGui::GetFontSize();
    if (!ImGui::BeginTable("evts", 4,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH))
    {
        ImGui::End();
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Seq",  ImGuiTableColumnFlags_WidthFixed, fs * 5.0f);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, fs * 9.0f);
    ImGui::TableSetupColumn("",     ImGuiTableColumnFlags_WidthFixed, fs * 2.5f);
    ImGui::TableSetupColumn("Body", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (const auto &r : a.eventBacklog)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextDim));
        ImGui::Text("%llu", static_cast<unsigned long long>(r.seq));
        ImGui::PopStyleColor();

        ImGui::TableSetColumnIndex(1);
        theme::Pill(wire::EventTypeName(r.type),
                    (EventColor(r.type) & 0x00FFFFFFu) | (50u << 24),
                    EventColor(r.type));

        ImGui::TableSetColumnIndex(2);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextDim));
        ImGui::Text("%u", r.bodyLen);
        ImGui::PopStyleColor();

        ImGui::TableSetColumnIndex(3);
        char body[160];
        FormatBody(r, body, sizeof(body));
        ImGui::TextUnformatted(body);
    }

    if (a.autoScrollEvts && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - fs)
    {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndTable();
    ImGui::End();
}

}
