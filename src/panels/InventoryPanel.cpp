#include "Panels.h"
#include "ItemIcon.h"

#include "app/App.h"
#include "app/Theme.h"
#include "cache/Json.h"
#include "ipc/Events.h"
#include "render/Texture.h"
#include "rpc/MsgPack.h"
#include "rpc/RpcClient.h"
#include "wire/EventReader.h"
#include "wire/SnapshotReader.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nxtdbg::panels
{

namespace
{

// Containers wider than this default to the dense list view even when "icons"
// is on — a fresh bank of ~1000 slots would otherwise queue a thousand icon
// decodes. The backpack (28) / equipment (~15) / specialty inventories all sit
// comfortably under it, so the grid is the everyday view.
constexpr int kGridSlotMax = 120;

// Item-icon decodes admitted per frame. Each decode is a software model render,
// so a filled container fills in over a few frames instead of hitching once.
constexpr int kIconsPerFrame = 24;

namespace mp = nxt::rpc::msgpack;

constexpr DWORD kObjRpcTimeoutMs = 1000;

// One per-item obj var (augment XP, charges, …) read via get_obj_vars.
struct ObjVar
{
    int id    = 0;
    int value = 0;
};

// All persistent panel state, held as a single function-local static in
// DrawInventoryPanel (the panels are free functions — there is no object to
// hang state on, matching the existing EntityBrowser / CacheBrowser idiom).
struct PanelState
{
    char                                 filter[64] = {};
    bool                                 showEmpty  = true;
    bool                                 iconView   = true;
    render::TextureCache                 itemIcons;
    std::unordered_set<int>              iconFailed;
    std::unordered_map<int, std::string> nameCache;   // itemId -> cache name ("" = none)
    int                                  iconBudget = 0;

    // Per-item obj vars (opt-in via the toolbar toggle). Fetched per container
    // over the shared RPC client, refreshed on a timer and on kEventObjVarChange.
    bool                                 showObjVars = false;
    float                                objPollAccum = 0.0f;
    uint64_t                             objLastSeenSeq = 0;
    bool                                 objPrimedSeq = false;
    bool                                 objDirty     = false;
    // invId -> slot -> [(varId, value)]; only slots that carry vars are present.
    std::unordered_map<int, std::unordered_map<int, std::vector<ObjVar>>> objVars;
};

void TextColored(ImU32 col, const char *text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// Case-insensitive substring test (gameval names are UPPER, cache names Title).
bool ContainsNoCase(const char *hay, const char *needle)
{
    if (!needle || !needle[0]) return true;
    if (!hay) return false;
    for (const char *h = hay; *h; ++h)
    {
        const char *a = h;
        const char *b = needle;
        while (*a && *b
               && std::tolower((unsigned char)*a) == std::tolower((unsigned char)*b))
        {
            ++a;
            ++b;
        }
        if (!*b) return true;
    }
    return false;
}

// Cache display name for an item id, parsed once and memoised. Empty when the
// cache has no name or is not open yet (callers fall back to the gameval name).
const char *ItemCacheName(app::App &a, PanelState &st, int id)
{
    static const std::string kEmpty;
    if (id < 0 || !a.cache.IsOpen()) return kEmpty.c_str();

    auto it = st.nameCache.find(id);
    if (it != st.nameCache.end()) return it->second.c_str();

    std::string name;
    std::string json = a.cache.GetJson("item", id);
    cache::JsonValue v;
    if (!json.empty() && cache::ParseJson(json, v))
    {
        const cache::JsonValue *n = v.Find("name");
        if (n && n->IsString()) name = n->strVal;
    }
    return st.nameCache.emplace(id, std::move(name)).first->second.c_str();
}

bool MatchesFilter(const PanelState &st, int itemId, const char *gv, const char *name)
{
    if (!st.filter[0]) return true;
    char idbuf[16];
    std::snprintf(idbuf, sizeof(idbuf), "%d", itemId);
    return std::strstr(idbuf, st.filter) != nullptr
        || ContainsNoCase(gv, st.filter)
        || ContainsNoCase(name, st.filter);
}

int CountUsed(const nxt::ipc::Snapshot &s, const nxt::ipc::InventoryHeader &h)
{
    int used = 0;
    for (uint16_t j = 0; j < h.slotCount; ++j)
    {
        uint32_t idx = uint32_t(h.firstItemIdx) + j;
        if (idx >= s.invItemCount) break;
        if (s.invItems[idx].itemId >= 0) ++used;
    }
    return used;
}

// --- Obj vars (get_obj_vars RPC + kEventObjVarChange) ----------------------

const std::vector<ObjVar> *ObjVarsFor(const PanelState &st, int invId, int slot)
{
    auto inv = st.objVars.find(invId);
    if (inv == st.objVars.end())
    {
        return nullptr;
    }
    auto sl = inv->second.find(slot);
    if (sl == inv->second.end() || sl->second.empty())
    {
        return nullptr;
    }
    return &sl->second;
}

bool DecodeOneVar(mp::Reader &r, ObjVar &v)
{
    int64_t id = 0;
    int64_t val = 0;
    bool ok = mp::ForEachParam(r,
        [&](const char *k, uint32_t kl, mp::Reader &rr) -> bool
    {
        if (mp::StrEq(k, kl, "id"))
        {
            return rr.ReadInt(id);
        }
        if (mp::StrEq(k, kl, "value"))
        {
            return rr.ReadInt(val);
        }
        return false;
    });
    v.id    = static_cast<int>(id);
    v.value = static_cast<int>(val);
    return ok;
}

bool DecodeVarArray(mp::Reader &r, std::vector<ObjVar> &out)
{
    uint32_t n = 0;
    if (!r.ReadArrayHeader(n))
    {
        return false;
    }
    for (uint32_t i = 0; i < n; ++i)
    {
        ObjVar v{};
        if (DecodeOneVar(r, v))
        {
            out.push_back(v);
        }
    }
    return true;
}

void DecodeSlot(mp::Reader &r, std::unordered_map<int, std::vector<ObjVar>> &out)
{
    int64_t             slot = -1;
    std::vector<ObjVar> vars;
    mp::ForEachParam(r,
        [&](const char *k, uint32_t kl, mp::Reader &rr) -> bool
    {
        if (mp::StrEq(k, kl, "slot"))
        {
            return rr.ReadInt(slot);
        }
        if (mp::StrEq(k, kl, "vars"))
        {
            return DecodeVarArray(rr, vars);
        }
        return false;
    });
    if (slot >= 0 && !vars.empty())
    {
        out[static_cast<int>(slot)] = std::move(vars);
    }
}

void DecodeObjVars(const std::vector<uint8_t> &reply,
                   std::unordered_map<int, std::vector<ObjVar>> &out)
{
    out.clear();
    mp::Reader r(reply.data(), reply.size());
    mp::ForEachParam(r,
        [&](const char *k, uint32_t kl, mp::Reader &rr) -> bool
    {
        if (!mp::StrEq(k, kl, "slots"))
        {
            return false;
        }
        uint32_t n = 0;
        if (!rr.ReadArrayHeader(n))
        {
            return false;
        }
        for (uint32_t i = 0; i < n; ++i)
        {
            DecodeSlot(rr, out);
        }
        return true;
    });
}

void FetchObjVars(app::App &a, PanelState &st, const nxt::ipc::Snapshot &s)
{
    st.objVars.clear();
    for (uint32_t i = 0; i < s.inventoryCount; ++i)
    {
        int invId = s.inventories[i].invId;
        std::vector<uint8_t> params;
        params.assign(16, 0);
        mp::Writer w(params.data(), params.size());
        w.WriteMapHeader(1);
        w.WriteCStr("inv_id");
        w.WriteInt(invId);
        params.resize(w.BytesWritten());

        std::vector<uint8_t> reply;
        if (a.rpc.Call("get_obj_vars", params.data(), static_cast<uint32_t>(params.size()),
                       reply, kObjRpcTimeoutMs) != rpc::CallStatus::Ok)
        {
            continue;
        }
        DecodeObjVars(reply, st.objVars[invId]);
    }
}

void ScanObjVarEvents(app::App &a, PanelState &st)
{
    if (st.objPrimedSeq)
    {
        for (const auto &r : a.eventBacklog)
        {
            if (r.seq > st.objLastSeenSeq && r.type == nxt::ipc::kEventObjVarChange)
            {
                st.objDirty = true;
            }
        }
    }
    if (!a.eventBacklog.empty())
    {
        st.objLastSeenSeq = a.eventBacklog.back().seq;
    }
    st.objPrimedSeq = true;
}

void UpdateObjVars(app::App &a, PanelState &st, const nxt::ipc::Snapshot &s)
{
    if (!st.showObjVars)
    {
        st.objVars.clear();
        return;
    }
    ScanObjVarEvents(a, st);
    st.objPollAccum += ImGui::GetIO().DeltaTime;
    bool due = st.objDirty || st.objPollAccum >= 1.0f;
    if (!due)
    {
        return;
    }
    st.objPollAccum = 0.0f;
    st.objDirty     = false;
    if (a.rpc.IsConnected())
    {
        FetchObjVars(a, st, s);
    }
}

// --- Icon-grid cell --------------------------------------------------------

void CellTooltip(const PanelState &st, int invId, int slot, int itemId,
                 const char *gv, const char *name, int qty)
{
    ImGui::BeginTooltip();
    if (name && name[0]) TextColored(theme::kTextHi, name);
    else                 ImGui::TextDisabled("(no cache name)");
    ImGui::Separator();
    ImGui::Text("slot     %d", slot);
    ImGui::Text("id       %d", itemId);
    if (gv) ImGui::Text("gameval  %s", gv);
    else    ImGui::TextDisabled("gameval  —");
    ImGui::Text("qty      %d", qty);
    if (st.showObjVars)
    {
        const std::vector<ObjVar> *vars = ObjVarsFor(st, invId, slot);
        if (vars)
        {
            ImGui::Separator();
            theme::Subheading("obj vars");
            for (const auto &v : *vars)
            {
                ImGui::Text("  %d = %d", v.id, v.value);
            }
        }
    }
    ImGui::EndTooltip();
}

void DrawIconCell(app::App &a, PanelState &st, int invId, int slot, int itemId, int qty, float box)
{
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + box, p0.y + box);
    ImDrawList  *dl = ImGui::GetWindowDrawList();
    const bool   empty = itemId < 0;

    const char *gv   = empty ? nullptr : a.gameval.NameForCacheType("item", itemId);
    const char *name = empty ? nullptr : ItemCacheName(a, st, itemId);
    const bool  dim  = !empty && !MatchesFilter(st, itemId, gv, name);

    dl->AddRectFilled(p0, p1, theme::kPanelDeep, 4.0f);
    dl->AddRect(p0, p1, (empty || dim) ? theme::kBorder : theme::kBorderHi, 4.0f);
    if (!empty)
    {
        DrawItemIcon(a, st.itemIcons, st.iconFailed, &st.iconBudget, itemId, p0, box, dim);
        DrawQtyBadge(dl, p1, qty);
        if (st.showObjVars && ObjVarsFor(st, invId, slot))
        {
            dl->AddCircleFilled(ImVec2(p0.x + box * 0.16f, p0.y + box * 0.16f),
                                box * 0.09f, theme::kAccent, 12);
        }
    }

    ImGui::InvisibleButton("cell", ImVec2(box, box));
    if (!empty && ImGui::IsItemHovered())
    {
        CellTooltip(st, invId, slot, itemId, gv, name, qty);
    }
}

void DrawIconGrid(app::App &a, PanelState &st, const nxt::ipc::Snapshot &s,
                  const nxt::ipc::InventoryHeader &h)
{
    const float box     = ImGui::GetFontSize() * 2.6f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float availW  = ImGui::GetContentRegionAvail().x;
    const int   perRow  = std::max(1, int((availW + spacing) / (box + spacing)));

    int drawn = 0;
    for (uint16_t j = 0; j < h.slotCount; ++j)
    {
        uint32_t idx = uint32_t(h.firstItemIdx) + j;
        if (idx >= s.invItemCount) break;
        const auto &it = s.invItems[idx];
        if (it.itemId < 0 && !st.showEmpty) continue;

        if (drawn % perRow != 0) ImGui::SameLine();
        ImGui::PushID(int(j));
        DrawIconCell(a, st, h.invId, int(j), it.itemId, it.quantity, box);
        ImGui::PopID();
        ++drawn;
    }
}

// --- List view -------------------------------------------------------------

void DrawListRow(int slot, int itemId, const char *gv, const char *name, int qty, bool empty)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::Text("%d", slot);
    ImGui::TableSetColumnIndex(1);
    if (empty) ImGui::TextDisabled("—");
    else
    {
        char idb[16];
        std::snprintf(idb, sizeof(idb), "%d", itemId);
        TextColored(theme::kAccent, idb);
    }
    ImGui::TableSetColumnIndex(2);
    if (gv) TextColored(theme::kInfo, gv);
    else    ImGui::TextDisabled(empty ? "(empty)" : "—");
    ImGui::TableSetColumnIndex(3);
    if (name && name[0]) TextColored(theme::kTextDim, name);
    else                 ImGui::TextDisabled("—");
    ImGui::TableSetColumnIndex(4);
    if (empty) ImGui::TextDisabled("—");
    else       ImGui::Text("%d", qty);
}

void DrawListView(app::App &a, PanelState &st, const nxt::ipc::Snapshot &s,
                  const nxt::ipc::InventoryHeader &h)
{
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                | ImGuiTableFlags_BordersInnerH;
    if (!ImGui::BeginTable("##invtbl", 5, flags, ImVec2(0, ImGui::GetFontSize() * 16.0f)))
    {
        return;
    }
    ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Id",   ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Gameval");
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Qty",  ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();

    for (uint16_t j = 0; j < h.slotCount; ++j)
    {
        uint32_t idx = uint32_t(h.firstItemIdx) + j;
        if (idx >= s.invItemCount) break;
        const auto &it = s.invItems[idx];
        const bool  empty = it.itemId < 0;
        if (empty && !st.showEmpty) continue;

        const char *gv   = empty ? nullptr : a.gameval.NameForCacheType("item", it.itemId);
        const char *name = empty ? nullptr : ItemCacheName(a, st, it.itemId);
        if (!empty && !MatchesFilter(st, it.itemId, gv, name)) continue;
        DrawListRow(int(j), it.itemId, gv, name, it.quantity, empty);
    }
    ImGui::EndTable();
}

// --- Container card + panel -------------------------------------------------

void DrawContainer(app::App &a, PanelState &st, const nxt::ipc::Snapshot &s,
                   const nxt::ipc::InventoryHeader &h)
{
    const char *gv     = a.gameval.NameForCacheType("inv", h.invId);
    const ImU32 stripe = gv ? theme::kAccent : theme::kTextDim;
    char title[96];
    std::snprintf(title, sizeof(title), "%s   #%d", gv ? gv : "(inv)", h.invId);

    if (theme::BeginCard("card", title, stripe))
    {
        char sub[64];
        std::snprintf(sub, sizeof(sub), "%d / %u slots filled", CountUsed(s, h), h.slotCount);
        theme::Subheading(sub);
        ImGui::Spacing();

        if (st.iconView && h.slotCount <= kGridSlotMax) DrawIconGrid(a, st, s, h);
        else                                            DrawListView(a, st, s, h);
    }
    theme::EndCard();
}

void DrawToolbar(PanelState &st, const nxt::ipc::Snapshot &s)
{
    ImGui::PushItemWidth(ImGui::GetFontSize() * 15.0f);
    ImGui::InputTextWithHint("##invfilter", "filter  (id / name / gameval)",
                             st.filter, sizeof(st.filter));
    ImGui::PopItemWidth();

    const float gap = ImGui::GetFontSize();
    ImGui::SameLine(0.0f, gap);
    theme::Toggle("##showempty", &st.showEmpty);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::AlignTextToFramePadding();
    theme::Subheading("empty slots");

    ImGui::SameLine(0.0f, gap);
    theme::Toggle("##iconview", &st.iconView);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::AlignTextToFramePadding();
    theme::Subheading("icons");

    ImGui::SameLine(0.0f, gap);
    if (theme::Toggle("##objvars", &st.showObjVars))
    {
        st.objDirty = true;   // fetch immediately when enabled
    }
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::AlignTextToFramePadding();
    theme::Subheading("obj vars");

    char sum[48];
    std::snprintf(sum, sizeof(sum), "%u containers", s.inventoryCount);
    float w = ImGui::CalcTextSize(sum).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - w));
    ImGui::AlignTextToFramePadding();
    theme::Subheading(sum);
}

}   // namespace

void DrawInventoryPanel(app::App &a)
{
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 40, ImGui::GetFontSize() * 34),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inventory"))
    {
        ImGui::End();
        return;
    }

    static PanelState st;
    const auto *snap = a.session.IsOpen() ? wire::CurrentSnapshot(a.session) : nullptr;
    if (!snap)
    {
        theme::Subheading("Attach an agent to inspect containers.");
        ImGui::End();
        return;
    }

    st.iconBudget = kIconsPerFrame;
    DrawToolbar(st, *snap);
    UpdateObjVars(a, st, *snap);
    ImGui::Spacing();

    if (snap->inventoryCount == 0)
    {
        theme::Subheading("No open containers — open your backpack or a bank to see them here.");
        ImGui::End();
        return;
    }

    ImGui::BeginChild("##invscroll");
    for (uint32_t i = 0; i < snap->inventoryCount; ++i)
    {
        ImGui::PushID(int(i));
        DrawContainer(a, st, *snap, snap->inventories[i]);
        ImGui::PopID();
        ImGui::Spacing();
    }
    ImGui::EndChild();
    ImGui::End();
}

}   // namespace nxtdbg::panels
