#include "Panels.h"

#include "app/App.h"
#include "app/Theme.h"
#include "attach/ProcessList.h"
#include "ipc/SharedLayout.h"

#include "imgui.h"

#include <cstdio>

namespace nxtdbg::panels
{

namespace
{

void DrawAttachedHero(app::App &a)
{
    if (!theme::BeginCard("attach.hero", "SESSION"))
    {
        theme::EndCard();
        return;
    }

    const auto *h = a.session.Header();
    bool live = h != nullptr;

    theme::StatusDot(live ? theme::kGood : theme::kTextDim, live, a.dotPhase);
    ImGui::SameLine();

    char buf[128];
    if (live)
    {
        std::snprintf(buf, sizeof(buf), "ATTACHED  pid %lu", a.session.Pid());
        theme::HeroStat("agent process", buf, theme::kGood);
    }
    else
    {
        theme::HeroStat("agent process", "NOT ATTACHED", theme::kTextDim);
    }

    if (live)
    {
        ImGui::SameLine(0, ImGui::GetFontSize() * 1.5f);
        ImGui::BeginGroup();
        char vb[32];
        std::snprintf(vb, sizeof(vb), "v%u", h->version);
        theme::Pill(vb, theme::kAccentSoft, theme::kAccent);
        ImGui::Spacing();
        if (ImGui::Button("Detach"))
        {
            a.session.Close();
            a.events.Reset();
        }
        ImGui::EndGroup();
    }
    theme::EndCard();
}

void DrawCandidateList(app::App &a, std::vector<attach::ProcessEntry> &list)
{
    if (!theme::BeginCard("attach.list", "CANDIDATES"))
    {
        theme::EndCard();
        return;
    }

    if (ImGui::Button("Refresh"))
    {
        list = attach::ListGameProcesses();
    }
    ImGui::SameLine();
    theme::Subheading(list.empty() ? "No rs2client*.exe found." : "Click a row to attach.");

    if (list.empty())
    {
        theme::EndCard();
        return;
    }

    const float fs = ImGui::GetFontSize();
    if (ImGui::BeginTable("procs", 3,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("PID",  ImGuiTableColumnFlags_WidthFixed, fs * 5.5f);
        ImGui::TableSetupColumn("EXE",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("",     ImGuiTableColumnFlags_WidthFixed, fs * 7.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < list.size(); ++i)
        {
            const auto &p = list[i];
            ImGui::TableNextRow();
            ImGui::PushID(int(p.pid));

            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertU32ToFloat4(theme::kAccent));
            ImGui::Text("%lu", p.pid);
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(1);
            char utf8[256];
            WideCharToMultiByte(CP_UTF8, 0, p.exeName.c_str(), -1,
                                utf8, sizeof(utf8), nullptr, nullptr);
            ImGui::TextUnformatted(utf8);

            ImGui::TableSetColumnIndex(2);
            if (ImGui::Button("Attach", ImVec2(-FLT_MIN, 0)))
            {
                a.session.Open(p.pid);
                a.events.Reset();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    theme::EndCard();
}

}

void DrawAttach(app::App &a)
{
    static std::vector<attach::ProcessEntry> sList = attach::ListGameProcesses();

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 38, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Attach"))
    {
        ImGui::End();
        return;
    }

    DrawAttachedHero(a);
    ImGui::Spacing();

    if (a.session.LastError()[0] != 0)
    {
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(theme::kBad));
        char utf8[512];
        WideCharToMultiByte(CP_UTF8, 0, a.session.LastError(), -1,
                            utf8, sizeof(utf8), nullptr, nullptr);
        ImGui::TextWrapped("%s", utf8);
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    DrawCandidateList(a, sList);
    ImGui::End();
}

}
