#include "Panels.h"
#include "ItemIcon.h"

#include "app/App.h"
#include "app/Theme.h"
#include "render/Texture.h"
#include "wire/SnapshotReader.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

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
    if (id < 0) return {};
    std::string name;
    if (a.cache.IsOpen())
    {
        std::string json = a.cache.GetJson(type, id);
        const char *p = json.empty() ? nullptr : std::strstr(json.c_str(), "\"name\"");
        if (p && (p = std::strchr(p, ':')) != nullptr)
        {
            ++p;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p == '"')
            {
                ++p;
                if (const char *q = std::strchr(p, '"'))
                {
                    name.assign(p, size_t(q - p));
                }
            }
        }
    }
    if (name.empty())
    {
        // Fallback: the cache has no display name (locs especially, and any
        // entity when the cache isn't open) — use the bundled gameval name.
        if (const char *gv = a.gameval.NameForCacheType(type, id))
        {
            name = gv;
        }
    }
    return name;
}

void TableHeader(const char *cols[], int n)
{
    for (int i = 0; i < n; ++i)
    {
        ImGui::TableSetupColumn(cols[i]);
    }
    ImGui::TableHeadersRow();
}

// Numeric cell that renders a "—" dash for the -1 "none" sentinel the wire uses
// for optional ids (spotAnimId, interactId, animationId).
void IntOrDash(int v)
{
    if (v >= 0)
    {
        ImGui::Text("%d", v);
    }
    else
    {
        ImGui::TextDisabled("—");
    }
}

// Raw v21 facing (PROTOCOL.md §2.8, "Entity orientation"). Shown in client
// units, not degrees: the compass convention is not pinned yet, and a raw
// number is what a reader compares against the wire anyway. The 0xFFFF
// sentinel renders as a dash; anything else above 0x3FFF breaks the producer's
// value-domain invariant and is flagged rather than printed as an angle.
void OrientationCell(uint16_t v)
{
    if (v == nxt::ipc::kOrientationUnknown)
    {
        ImGui::TextDisabled("—");
    }
    else if (v > nxt::ipc::kOrientationMask)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kBad));
        ImGui::Text("BAD %u", unsigned(v));
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::Text("%u", unsigned(v));
    }
}

void NpcsTab(app::App &a, const nxt::ipc::Snapshot &s)
{
    static FilterBuf fb;
    DrawFilter(fb);
    char buf[64];

    if (!ImGui::BeginTable("npcs", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                   | ImGuiTableFlags_BordersInnerH))
    {
        return;
    }
    const char *cols[] = { "Idx", "Type", "Name", "Tile", "HP", "Anim", "Gfx", "Stance", "Facing" };
    TableHeader(cols, 9);

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
        ImGui::TableSetColumnIndex(6); IntOrDash(n.spotAnimId);
        ImGui::TableSetColumnIndex(7); ImGui::Text("%d", n.stanceId);
        ImGui::TableSetColumnIndex(8); OrientationCell(n.orientation);
    }
    ImGui::EndTable();
}

void PlayersTab(const nxt::ipc::Snapshot &s)
{
    static FilterBuf fb;
    DrawFilter(fb);
    char buf[64];

    if (!ImGui::BeginTable("plyrs", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                     | ImGuiTableFlags_BordersInnerH))
    {
        return;
    }
    const char *cols[] = { "Idx", "Combat", "Tile", "Anim", "Gfx", "Stance", "Flags", "Facing" };
    TableHeader(cols, 8);

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
        ImGui::TableSetColumnIndex(4); IntOrDash(p.spotAnimId);
        ImGui::TableSetColumnIndex(5); ImGui::Text("%d", p.stanceId);
        ImGui::TableSetColumnIndex(6);
        if (p.flags & nxt::ipc::kFlagMoving)
        {
            theme::Pill("moving", theme::kInfo);
        }
        ImGui::TableSetColumnIndex(7); OrientationCell(p.orientation);
    }
    ImGui::EndTable();
}

// The base id a loc row publishes: a combined section carries it in typeId, a
// direct LOCATION in interactId. Every consumer of this wire makes the same
// pick, and it is what the Morph column is judged against.
int32_t LocBaseId(const nxt::ipc::LocationEntry &l)
{
    const bool section = (l.flags & nxt::ipc::kLocFlagCombinedSection) != 0;
    return section ? l.typeId : l.interactId;
}

// Only the rows the producer actually transformed earn ink in the Morph column.
// On everything else resolvedId equals the base id by contract, so printing it
// would be a column of repeats hiding the few rows that matter.
void DrawMorphCell(const nxt::ipc::LocationEntry &l)
{
    if (LocBaseId(l) > 0 && l.resolvedId != LocBaseId(l))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kInfo));
        ImGui::Text("%d", l.resolvedId);
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::TextDisabled("—");
    }
}

void DrawLocRow(app::App &a, const nxt::ipc::LocationEntry &l)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kAccent));
    ImGui::Text("%d", l.typeId);
    ImGui::PopStyleColor();

    ImGui::TableSetColumnIndex(1);
    DrawMorphCell(l);

    // Named off resolvedId, never the base: a morph loc's base definition has an
    // empty name, which is the whole reason v20 publishes the resolved id.
    ImGui::TableSetColumnIndex(2);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextDim));
    ImGui::TextUnformatted(CacheName(a, "loc", l.resolvedId).c_str());
    ImGui::PopStyleColor();

    ImGui::TableSetColumnIndex(3); ImGui::Text("%d, %d  p%d", l.tileX, l.tileY, int(l.plane));
    ImGui::TableSetColumnIndex(4); ImGui::Text("%u", unsigned(l.shape));
    ImGui::TableSetColumnIndex(5); ImGui::Text("%u", unsigned(l.rotation));
    ImGui::TableSetColumnIndex(6); IntOrDash(l.animationId);
    ImGui::TableSetColumnIndex(7); IntOrDash(l.interactId);
    ImGui::TableSetColumnIndex(8);
    if (l.flags & nxt::ipc::kLocFlagHidden)          theme::Pill("hidden",  theme::kBad);
    if (l.flags & nxt::ipc::kLocFlagCombinedSection) theme::Pill("section", theme::kInfo);
    if (l.flags & nxt::ipc::kLocFlagDeleted)         theme::Pill("deleted", theme::kBad);
}

void LocsTab(app::App &a, const nxt::ipc::Snapshot &s)
{
    static FilterBuf fb;
    DrawFilter(fb);
    char buf[64];

    if (!ImGui::BeginTable("locs", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                    | ImGuiTableFlags_BordersInnerH))
    {
        return;
    }
    const char *cols[] = { "Type", "Morph", "Name", "Tile", "Shape", "Rot",
                           "Anim", "Interact", "Flags" };
    TableHeader(cols, 9);

    for (uint32_t i = 0; i < s.locationCount; ++i)
    {
        const auto &l = s.locations[i];
        // Filter on both ids, so a search for the resolved id finds the row the
        // producer published under its base id.
        std::snprintf(buf, sizeof(buf), "%d %d %d %d",
                      l.typeId, l.resolvedId, l.tileX, l.tileY);
        if (!fb.Match(fb.text, buf)) continue;

        DrawLocRow(a, l);
    }
    ImGui::EndTable();
}

// --- Ground items tab ------------------------------------------------------

// Item-icon decodes admitted per frame (a software model render apiece), so a
// drop-heavy scene fills in over a few frames instead of hitching once. Mirrors
// InventoryPanel::kIconsPerFrame.
constexpr int kGroundIconsPerFrame = 24;

struct GroundRow
{
    int itemId;
    int qty;
    int tileX;
    int tileY;
    int plane;
    int dist;   // chebyshev tiles from the local player; -1 when not in-world
};

struct GroundState
{
    FilterBuf               fb;
    render::TextureCache    icons;
    std::unordered_set<int> iconFailed;
    int                     iconBudget = 0;
    bool                    showIcons  = true;
};

int Chebyshev(int ax, int ay, int bx, int by)
{
    int dx = ax > bx ? ax - bx : bx - ax;
    int dy = ay > by ? ay - by : by - ay;
    return dx > dy ? dx : dy;
}

void CollectGroundRows(GroundState &gs, const nxt::ipc::Snapshot &s,
                       std::vector<GroundRow> &out)
{
    const bool haveSelf = s.self.serverIndex >= 0;
    char buf[64];
    for (uint32_t i = 0; i < s.groundItemCount; ++i)
    {
        const auto &g = s.groundItems[i];
        std::snprintf(buf, sizeof(buf), "%d %d %d", g.itemId, g.tileX, g.tileY);
        if (!gs.fb.Match(gs.fb.text, buf))
        {
            continue;
        }
        GroundRow row;
        row.itemId = g.itemId;
        row.qty    = g.quantity;
        row.tileX  = g.tileX;
        row.tileY  = g.tileY;
        row.plane  = g.plane;
        row.dist   = haveSelf ? Chebyshev(g.tileX, g.tileY, s.self.tileX, s.self.tileY) : -1;
        out.push_back(row);
    }
}

void SortGroundRows(std::vector<GroundRow> &rows)
{
    const ImGuiTableSortSpecs *ss = ImGui::TableGetSortSpecs();
    if (!ss || ss->SpecsCount == 0)
    {
        return;
    }
    const ImGuiTableColumnSortSpecs &c = ss->Specs[0];
    const bool asc = c.SortDirection == ImGuiSortDirection_Ascending;
    std::sort(rows.begin(), rows.end(), [&](const GroundRow &x, const GroundRow &y)
    {
        int xv = x.dist;
        int yv = y.dist;
        if (c.ColumnIndex == 1)
        {
            xv = x.itemId;
            yv = y.itemId;
        }
        else if (c.ColumnIndex == 2)
        {
            xv = x.qty;
            yv = y.qty;
        }
        return asc ? xv < yv : xv > yv;
    });
}

void DrawGroundIconCell(app::App &a, GroundState &gs, int itemId)
{
    if (!gs.showIcons)
    {
        ImGui::TextDisabled("—");
        return;
    }
    const float box = ImGui::GetTextLineHeight() * 1.3f;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    DrawItemIcon(a, gs.icons, gs.iconFailed, &gs.iconBudget, itemId, p0, box, false);
    ImGui::Dummy(ImVec2(box, box));
}

void DrawGroundRow(app::App &a, GroundState &gs, const GroundRow &g)
{
    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    DrawGroundIconCell(a, gs, g.itemId);

    ImGui::TableSetColumnIndex(1);
    char idb[16];
    std::snprintf(idb, sizeof(idb), "%d", g.itemId);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kAccent));
    ImGui::TextUnformatted(idb);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextDim));
    ImGui::TextUnformatted(CacheName(a, "item", g.itemId).c_str());
    ImGui::PopStyleColor();

    ImGui::TableSetColumnIndex(2);
    char qb[16];
    FormatQty(g.qty, qb, sizeof(qb));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(QtyColor(g.qty)));
    ImGui::TextUnformatted(qb);
    ImGui::PopStyleColor();

    ImGui::TableSetColumnIndex(3);
    ImGui::Text("%d, %d  p%d", g.tileX, g.tileY, g.plane);

    ImGui::TableSetColumnIndex(4);
    IntOrDash(g.dist);
}

void GroundToolbar(GroundState &gs)
{
    DrawFilter(gs.fb);
    ImGui::SameLine(0.0f, ImGui::GetFontSize());
    theme::Toggle("##gicons", &gs.showIcons);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::AlignTextToFramePadding();
    theme::Subheading("icons");
}

void GroundTab(app::App &a, const nxt::ipc::Snapshot &s)
{
    static GroundState gs;
    gs.iconBudget = kGroundIconsPerFrame;

    GroundToolbar(gs);

    if (s.groundItemCount == 0)
    {
        ImGui::Spacing();
        theme::Subheading("No ground items in the loaded scene.");
        return;
    }

    const float fs = ImGui::GetFontSize();
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Sortable;
    if (!ImGui::BeginTable("ground", 5, flags))
    {
        return;
    }
    ImGui::TableSetupColumn("Icon", ImGuiTableColumnFlags_NoSort
                                  | ImGuiTableColumnFlags_WidthFixed, fs * 2.0f);
    ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Qty",  ImGuiTableColumnFlags_WidthFixed, fs * 5.0f);
    ImGui::TableSetupColumn("Tile", ImGuiTableColumnFlags_WidthFixed, fs * 8.0f);
    ImGui::TableSetupColumn("Dist", ImGuiTableColumnFlags_DefaultSort
                                  | ImGuiTableColumnFlags_WidthFixed, fs * 4.0f);
    ImGui::TableHeadersRow();

    std::vector<GroundRow> rows;
    CollectGroundRows(gs, s, rows);
    SortGroundRows(rows);
    for (const auto &g : rows)
    {
        DrawGroundRow(a, gs, g);
    }
    ImGui::EndTable();
}

// --- Projectiles tab -------------------------------------------------------

// Endpoint cell for a projectile's source / target: the entity server index
// (a "—" dash for the -1 "fixed tile" sentinel) followed by the raw
// entity-type tag in dim text. The type is passed through verbatim from the
// engine, so it's shown as an opaque "t<n>" tag.
void EndpointCell(int index, int type)
{
    IntOrDash(index);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextDim));
    ImGui::Text("t%d", type);
    ImGui::PopStyleColor();
}

void ProjectilesTab(const nxt::ipc::Snapshot &s)
{
    static FilterBuf fb;
    DrawFilter(fb);
    char buf[64];

    if (s.projectileCount == 0)
    {
        ImGui::Spacing();
        theme::Subheading("No in-flight projectiles this tick.");
        return;
    }

    if (!ImGui::BeginTable("projs", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                     | ImGuiTableFlags_BordersInnerH))
    {
        return;
    }
    // Gfx = projectile graphic (spot-anim) id; Start/End = launch/land game
    // cycles bracketing the flight; Source/Target = entity server index + raw
    // type tag (— = fixed-tile endpoint); From/To = absolute launch/target tiles.
    const char *cols[] = { "Idx", "Gfx", "Start", "End",
                           "Source", "Target", "From", "To", "Plane" };
    TableHeader(cols, 9);

    for (uint32_t i = 0; i < s.projectileCount; ++i)
    {
        const auto &p = s.projectiles[i];
        std::snprintf(buf, sizeof(buf), "%d %d %d", p.projectileId, p.sourceIndex, p.targetIndex);
        if (!fb.Match(fb.text, buf)) continue;

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("%u", i);
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kAccent));
        ImGui::Text("%d", p.projectileId);
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(2); ImGui::Text("%d", p.startCycle);
        ImGui::TableSetColumnIndex(3); ImGui::Text("%d", p.endCycle);
        ImGui::TableSetColumnIndex(4); EndpointCell(p.sourceIndex, p.sourceType);
        ImGui::TableSetColumnIndex(5); EndpointCell(p.targetIndex, p.targetType);
        ImGui::TableSetColumnIndex(6); ImGui::Text("%d, %d", p.startTileX, p.startTileY);
        ImGui::TableSetColumnIndex(7); ImGui::Text("%d, %d", p.endTileX, p.endTileY);
        ImGui::TableSetColumnIndex(8); ImGui::Text("%d", int(p.plane));
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
        if (ImGui::BeginTabItem("Ground"))      { GroundTab(a, *snap);  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Projectiles")) { ProjectilesTab(*snap); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

}
