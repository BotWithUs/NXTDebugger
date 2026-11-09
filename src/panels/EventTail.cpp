#include "Panels.h"

#include "app/App.h"
#include "app/Theme.h"
#include "ipc/Events.h"
#include "wire/EventFormat.h"
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
        case kEventTokenRefreshFired:  return theme::kWarn;   // kickoff posted, awaiting reply
        case kEventTokenRefreshed:     return theme::kGood;   // fresh access_token landed
        case kEventTokenRefreshFailed: return theme::kBad;    // stuck past deadline
        case kEventVarChange:
        case kEventVarbitChange:
        case kEventVarcChange:       return theme::kInfo;
        case kEventObjVarChange:     return theme::kInfo;
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
        wire::FormatEventBody(r, body, sizeof(body));
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
