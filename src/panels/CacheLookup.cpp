#include "Panels.h"

#include "app/App.h"
#include "app/Theme.h"
#include "cache/CacheClient.h"

#include "imgui.h"

#include <Windows.h>
#include <Shlwapi.h>
#include <cstdio>
#include <string>

namespace nxtdbg::panels
{

namespace
{

const char *kTypes[] = {
    "npc", "item", "loc", "seq", "varbit", "enum",
    "struct", "inv", "param", "quest",
};

void DefaultCachePath(wchar_t *out, size_t n)
{
    wchar_t local[MAX_PATH];
    DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (got == 0 || got >= MAX_PATH)
    {
        std::swprintf(out, n, L"%s", L"C:\\");
        return;
    }
    std::swprintf(out, n, L"%s\\Jagex\\RuneScape\\Cache", local);
}

void DrawOpenCard(app::App &a)
{
    if (!theme::BeginCard("cache.open", "CACHE"))
    {
        theme::EndCard();
        return;
    }
    static char path[MAX_PATH] = {};
    if (path[0] == 0)
    {
        wchar_t w[MAX_PATH];
        DefaultCachePath(w, MAX_PATH);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, path, MAX_PATH, nullptr, nullptr);
    }

    theme::StatusDot(a.cache.IsOpen() ? theme::kGood : theme::kTextDim,
                     a.cache.IsOpen(), a.dotPhase);
    ImGui::SameLine();
    theme::HeroStat("local cache",
                    a.cache.IsOpen() ? "OPEN" : "CLOSED",
                    a.cache.IsOpen() ? theme::kGood : theme::kTextDim);

    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.7f);
    ImGui::InputText("##cachepath", path, sizeof(path));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button(a.cache.IsOpen() ? "Reopen" : "Open"))
    {
        wchar_t w[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, path, -1, w, MAX_PATH);
        a.cache.Open(w);
    }
    if (a.cache.LastError()[0] != 0)
    {
        char e[512];
        WideCharToMultiByte(CP_UTF8, 0, a.cache.LastError(), -1, e, sizeof(e), nullptr, nullptr);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kBad));
        ImGui::TextWrapped("%s", e);
        ImGui::PopStyleColor();
    }
    theme::EndCard();
}

void DrawLookupCard(app::App &a, std::string &result)
{
    if (!theme::BeginCard("cache.look", "LOOKUP"))
    {
        theme::EndCard();
        return;
    }
    static int typeIdx = 0;
    static int id      = 0;

    ImGui::PushItemWidth(ImGui::GetFontSize() * 8);
    if (ImGui::BeginCombo("##type", kTypes[typeIdx]))
    {
        for (int i = 0; i < int(sizeof(kTypes) / sizeof(kTypes[0])); ++i)
        {
            bool sel = (i == typeIdx);
            if (ImGui::Selectable(kTypes[i], sel))
            {
                typeIdx = i;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::PushItemWidth(ImGui::GetFontSize() * 7);
    ImGui::InputInt("##id", &id);
    ImGui::PopItemWidth();

    ImGui::SameLine();
    bool disabled = !a.cache.IsOpen();
    if (disabled)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Resolve"))
    {
        result = a.cache.GetJson(kTypes[typeIdx], id);
        if (result.empty())
        {
            result = "(not found)";
        }
    }
    if (disabled)
    {
        ImGui::EndDisabled();
    }
    theme::EndCard();
}

void DrawResult(const std::string &result)
{
    if (!theme::BeginCard("cache.res", "JSON"))
    {
        theme::EndCard();
        return;
    }
    if (result.empty())
    {
        theme::Subheading("Resolve an id to see decoded JSON.");
        theme::EndCard();
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_FrameBg,
                          ImGui::ColorConvertU32ToFloat4(theme::kPanelDeep));
    ImGui::InputTextMultiline("##json",
                              const_cast<char *>(result.c_str()),
                              result.size() + 1,
                              ImVec2(-FLT_MIN, ImGui::GetFontSize() * 18),
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleColor();
    theme::EndCard();
}

}

void DrawCacheLookup(app::App &a)
{
    static std::string result;

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 48, ImGui::GetFontSize() * 36),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Cache lookup"))
    {
        ImGui::End();
        return;
    }
    DrawOpenCard(a);
    ImGui::Spacing();
    DrawLookupCard(a, result);
    ImGui::Spacing();
    DrawResult(result);
    ImGui::End();
}

}
