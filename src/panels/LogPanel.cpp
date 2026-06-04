#include "Panels.h"

#include "app/App.h"
#include "app/Theme.h"
#include "log/Log.h"

#include "imgui.h"

namespace nxtdbg::panels
{

namespace
{

ImU32 LevelColor(log::Level lv)
{
    switch (lv)
    {
        case log::Level::Warn:  return theme::kWarn;
        case log::Level::Error: return theme::kBad;
        default:                return theme::kInfo;
    }
}

const char *LevelName(log::Level lv)
{
    switch (lv)
    {
        case log::Level::Warn:  return "WARN";
        case log::Level::Error: return "ERR ";
        default:                return "INFO";
    }
}

}

void DrawLogPanel(app::App &a)
{
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 56, ImGui::GetFontSize() * 22),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Log"))
    {
        ImGui::End();
        return;
    }

    if (theme::BeginCard("log.bar", "DIAGNOSTICS"))
    {
        ImGui::BeginGroup();
        theme::Subheading("auto-scroll");
        theme::Toggle("##aslog", &a.autoScrollLog);
        ImGui::EndGroup();
    }
    theme::EndCard();
    ImGui::Spacing();

    log::Entry entries[256];
    uint32_t n = log::Snapshot(entries, 256);

    const float fs = ImGui::GetFontSize();
    ImGui::BeginChild("log.scroll", ImVec2(0, 0), ImGuiChildFlags_Border);

    char utf8[512];
    for (uint32_t i = 0; i < n; ++i)
    {
        const auto &e   = entries[i];
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImU32 col       = LevelColor(e.level);
        ImGui::GetWindowDrawList()->AddRectFilled(
            p0, ImVec2(p0.x + 3.0f, p0.y + fs * 1.05f), col);

        ImGui::SetCursorScreenPos(ImVec2(p0.x + fs * 0.6f, p0.y));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
        ImGui::TextUnformatted(LevelName(e.level));
        ImGui::PopStyleColor();

        ImGui::SameLine();
        WideCharToMultiByte(CP_UTF8, 0, e.text, -1, utf8, sizeof(utf8), nullptr, nullptr);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kText));
        ImGui::TextUnformatted(utf8);
        ImGui::PopStyleColor();
    }
    if (a.autoScrollLog && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - fs)
    {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

}
