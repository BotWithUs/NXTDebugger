#include "Panels.h"

#include "app/App.h"
#include "app/Theme.h"
#include "wire/SnapshotReader.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace nxtdbg::panels
{

namespace
{

struct FilterBuf
{
    char text[64] = {};
    bool Match(const char *needle, const char *hay) const
    {
        if (!needle[0]) return true;
        return std::strstr(hay, needle) != nullptr;
    }
};

void DrawFilter(FilterBuf &fb)
{
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
    ImGui::InputTextWithHint("##filter", "filter (substring)", fb.text, sizeof(fb.text));
    ImGui::PopItemWidth();
}

std::string CacheName(app::App &a, const char *type, int id)
{
    if (id < 0 || !a.cache.IsOpen()) return {};
    std::string json = a.cache.GetJson(type, id);
    if (json.empty()) return {};
    const char *p = std::strstr(json.c_str(), "\"name\"");
    if (!p) return {};
    p = std::strchr(p, ':');
    if (!p) return {};
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '"') return {};
    ++p;
    const char *q = std::strchr(p, '"');
    if (!q) return {};
    return std::string(p, size_t(q - p));
}

void TableHeader(const char *cols[], int n)
{
    for (int i = 0; i < n; ++i)
    {
        ImGui::TableSetupColumn(cols[i]);
    }
    ImGui::TableHeadersRow();
}

void NpcsTab(app::App &a, const nxt::ipc::Snapshot &s)
{
    static FilterBuf fb;
    DrawFilter(fb);
    char buf[64];

    if (!ImGui::BeginTable("npcs", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                   | ImGuiTableFlags_BordersInnerH))
    {
        return;
    }
    const char *cols[] = { "Idx", "Type", "Name", "Tile", "HP", "Anim" };
    TableHeader(cols, 6);

    for (uint32_t i = 0; i < s.npcCount; ++i)
    {
        const auto &n = s.npcs[i];
        std::snprintf(buf, sizeof(buf), "%d %d %d %d", n.serverIndex, n.typeId, n.tileX, n.tileY);
        if (!fb.Match(fb.text, buf)) continue;

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("%d", n.serverIndex);
        ImGui::TableSetColumnIndex(1); ImGui::Text("%d", n.typeId);
        ImGui::TableSetColumnIndex(2);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextDim));
        ImGui::TextUnformatted(CacheName(a, "npc", n.typeId).c_str());
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%d, %d  p%d", n.tileX, n.tileY, int(n.plane));
        ImGui::TableSetColumnIndex(4);
        if (n.maxHp > 0)
        {
            float frac = float(n.hp) / float(n.maxHp);
            ImU32 col = frac > 0.5f ? theme::kGood : frac > 0.2f ? theme::kWarn : theme::kBad;
            std::snprintf(buf, sizeof(buf), "%d / %d", n.hp, n.maxHp);
            theme::ProgressStrip(frac, col, buf);
        }
        else
        {
            ImGui::TextDisabled("-");
        }
        ImGui::TableSetColumnIndex(5); ImGui::Text("%d", n.animationId);
    }
    ImGui::EndTable();
}

void PlayersTab(const nxt::ipc::Snapshot &s)
{
    static FilterBuf fb;
    DrawFilter(fb);
    char buf[64];

    if (!ImGui::BeginTable("plyrs", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                     | ImGuiTableFlags_BordersInnerH))
    {
        return;
    }
    const char *cols[] = { "Idx", "Combat", "Tile", "Anim", "Flags" };
    TableHeader(cols, 5);

    for (uint32_t i = 0; i < s.playerCount; ++i)
    {
        const auto &p = s.players[i];
        std::snprintf(buf, sizeof(buf), "%d %d %d %d", p.serverIndex, p.combatLevel, p.tileX, p.tileY);
        if (!fb.Match(fb.text, buf)) continue;

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("%d", p.serverIndex);
        ImGui::TableSetColumnIndex(1); ImGui::Text("%d", p.combatLevel);
        ImGui::TableSetColumnIndex(2); ImGui::Text("%d, %d  p%d", p.tileX, p.tileY, int(p.plane));
        ImGui::TableSetColumnIndex(3); ImGui::Text("%d", p.animationId);
        ImGui::TableSetColumnIndex(4);
        if (p.flags & nxt::ipc::kFlagMoving) theme::Pill("moving", theme::kInfo);
    }
    ImGui::EndTable();
}

void LocsTab(app::App &a, const nxt::ipc::Snapshot &s)
{
    static FilterBuf fb;
    DrawFilter(fb);
    char buf[64];

    if (!ImGui::BeginTable("locs", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                    | ImGuiTableFlags_BordersInnerH))
    {
        return;
    }
    const char *cols[] = { "Type", "Name", "Tile", "Shape", "Rot", "Flags" };
    TableHeader(cols, 6);

    for (uint32_t i = 0; i < s.locationCount; ++i)
    {
        const auto &l = s.locations[i];
        std::snprintf(buf, sizeof(buf), "%d %d %d", l.typeId, l.tileX, l.tileY);
        if (!fb.Match(fb.text, buf)) continue;

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kAccent));
        ImGui::Text("%d", l.typeId);
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextDim));
        ImGui::TextUnformatted(CacheName(a, "loc", l.typeId).c_str());
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(2); ImGui::Text("%d, %d  p%d", l.tileX, l.tileY, int(l.plane));
        ImGui::TableSetColumnIndex(3); ImGui::Text("%u", unsigned(l.shape));
        ImGui::TableSetColumnIndex(4); ImGui::Text("%u", unsigned(l.rotation));
        ImGui::TableSetColumnIndex(5);
        if (l.flags & nxt::ipc::kLocFlagHidden)          theme::Pill("hidden",  theme::kBad);
        if (l.flags & nxt::ipc::kLocFlagCombinedSection) theme::Pill("section", theme::kInfo);
        if (l.flags & nxt::ipc::kLocFlagDeleted)         theme::Pill("deleted", theme::kBad);
    }
    ImGui::EndTable();
}

void InvTab(app::App &a, const nxt::ipc::Snapshot &s)
{
    if (!ImGui::BeginTable("inv", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                   | ImGuiTableFlags_BordersInnerH))
    {
        return;
    }
    const char *cols[] = { "InvId", "Slot", "Item", "Qty" };
    TableHeader(cols, 4);

    for (uint32_t i = 0; i < s.inventoryCount; ++i)
    {
        const auto &h = s.inventories[i];
        for (uint16_t j = 0; j < h.slotCount; ++j)
        {
            uint32_t idx = uint32_t(h.firstItemIdx) + j;
            if (idx >= s.invItemCount) break;
            const auto &it = s.invItems[idx];
            if (it.itemId < 0 && it.quantity == 0) continue;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", h.invId);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%u", unsigned(j));
            ImGui::TableSetColumnIndex(2);
            if (it.itemId >= 0)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kAccent));
                ImGui::Text("%d", it.itemId);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextDim));
                ImGui::TextUnformatted(CacheName(a, "item", it.itemId).c_str());
                ImGui::PopStyleColor();
            }
            else
            {
                ImGui::TextDisabled("(empty)");
            }
            ImGui::TableSetColumnIndex(3); ImGui::Text("%d", it.quantity);
        }
    }
    ImGui::EndTable();
}

}

void DrawEntityBrowser(app::App &a)
{
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 56, ImGui::GetFontSize() * 30),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Entities"))
    {
        ImGui::End();
        return;
    }
    const auto *snap = a.session.IsOpen()
                           ? wire::CurrentSnapshot(a.session)
                           : nullptr;
    if (!snap)
    {
        theme::Subheading("Attach an agent to browse entities.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("ents"))
    {
        if (ImGui::BeginTabItem("NPCs"))        { NpcsTab(a, *snap);    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Players"))     { PlayersTab(*snap);    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Locations"))   { LocsTab(a, *snap);    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Inventories")) { InvTab(a, *snap);     ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

}
