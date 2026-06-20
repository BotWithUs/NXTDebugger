#include "InterfacePanel.h"
#include "Panels.h"

#include "app/App.h"
#include "app/Theme.h"
#include "rpc/MsgPack.h"
#include "rpc/RpcClient.h"
#include "wire/SnapshotReader.h"

#include "ipc/SharedLayout.h"
#include "game/Interfaces.h"

#include "imgui.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace nxtdbg::panels
{

namespace
{

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr DWORD    kRpcTimeoutMs       = 1500;
constexpr DWORD    kTreeRefreshMs      = 600;  // matches server-tick cadence
constexpr DWORD    kPickPollMs         = 200;  // 5 Hz cursor sample
constexpr float    kDiffBadgeSeconds   = 1.0f;
constexpr uint32_t kEncBufCap          = 256;
constexpr uint32_t kReplyBufCap        = 1u << 20;   // 1 MiB; tree reply ceiling

// Well-known interface ids — duplicated from
// JBotWithUsV2/api/.../util/Interfaces.java. Twenty entries max, content-stable
// per build, so a static copy is cheaper than a wire round-trip.
struct IfaceName
{
    int32_t     id;
    const char *name;
};

constexpr IfaceName kIfaceNames[] = {
    {  105, "GRAND_EXCHANGE" },
    {  137, "CHAT_BOX"       },
    {  182, "LOGOUT"         },
    {  517, "BANK"           },
    { 1184, "DIALOGUE"       },
    { 1433, "SETTINGS"       },
    { 1458, "PRAYER"         },
    { 1460, "COMBAT"         },
    { 1461, "MAGIC"          },
    { 1464, "EQUIPMENT"      },
    { 1465, "MINIMAP"        },
    { 1466, "SKILLS"         },
    { 1473, "BACKPACK"       },
    { 1587, "WORLD_MAP"      },
};

const char *LookupIfaceName(int32_t id)
{
    for (const auto &e : kIfaceNames)
    {
        if (e.id == id) return e.name;
    }
    return nullptr;
}

const char *CategoryName(int32_t cat)
{
    using nxt::game::interfaces::WireCategory;
    switch (static_cast<WireCategory>(cat))
    {
    case WireCategory::Unknown:  return "Unknown";
    case WireCategory::Layer:    return "Layer";
    case WireCategory::Box:      return "Box";
    case WireCategory::Text:     return "Text";
    case WireCategory::Sprite:   return "Sprite";
    case WireCategory::Model:    return "Model";
    case WireCategory::Button:   return "Button";
    case WireCategory::Divider:  return "Divider";
    case WireCategory::List:     return "List";
    case WireCategory::Input:    return "Input";
    case WireCategory::Combo:    return "Combo";
    case WireCategory::Media:    return "Media";
    case WireCategory::Tooltip:  return "Tooltip";
    case WireCategory::CrmView:  return "CrmView";
    case WireCategory::Table:    return "Table";
    case WireCategory::Cutscene: return "Cutscene";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Decoded component — mirrors the agent's WriteComponentMap shape.
// ---------------------------------------------------------------------------

struct Comp
{
    int32_t     iface         = -1;
    int32_t     comp          = -1;
    int32_t     sub           = 0;
    int32_t     type          = -1;
    int32_t     category      = 0;
    int32_t     x             = 0;
    int32_t     y             = 0;
    int32_t     w             = 0;
    int32_t     h             = 0;
    int32_t     rawX          = 0;
    int32_t     rawY          = 0;
    int32_t     rawW          = 0;
    int32_t     rawH          = 0;
    int32_t     xPosMode      = 0;
    int32_t     yPosMode      = 0;
    int32_t     xSizeMode     = 0;
    int32_t     ySizeMode     = 0;
    int32_t     absScreenPos  = 0;
    std::string text;
    int32_t     hidden        = -1;
    int32_t     spriteId      = -1;
    int32_t     itemId        = -1;
    int32_t     itemAmount    = -1;
    int32_t     parentIndex   = -1;   // valid only inside a tree reply
};

struct TreeRow
{
    Comp     comp;
    int32_t  depth      = 0;
    bool     expanded   = false;
};

// ---------------------------------------------------------------------------
// msgpack decoding helpers (one component map → Comp)
// ---------------------------------------------------------------------------

void AssignKey(Comp &c, const char *k, uint32_t kn, nxt::rpc::msgpack::Reader &r)
{
    using nxt::rpc::msgpack::StrEq;
    int64_t iv = 0;
    if (StrEq(k, kn, "iface"))          { r.ReadInt(iv); c.iface = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "comp"))           { r.ReadInt(iv); c.comp = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "sub"))            { r.ReadInt(iv); c.sub = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "type"))           { r.ReadInt(iv); c.type = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "category"))       { r.ReadInt(iv); c.category = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "x"))              { r.ReadInt(iv); c.x = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "y"))              { r.ReadInt(iv); c.y = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "w"))              { r.ReadInt(iv); c.w = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "h"))              { r.ReadInt(iv); c.h = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "raw_x"))          { r.ReadInt(iv); c.rawX = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "raw_y"))          { r.ReadInt(iv); c.rawY = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "raw_w"))          { r.ReadInt(iv); c.rawW = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "raw_h"))          { r.ReadInt(iv); c.rawH = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "x_pos_mode"))     { r.ReadInt(iv); c.xPosMode = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "y_pos_mode"))     { r.ReadInt(iv); c.yPosMode = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "x_size_mode"))    { r.ReadInt(iv); c.xSizeMode = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "y_size_mode"))    { r.ReadInt(iv); c.ySizeMode = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "abs_screen_pos")) { r.ReadInt(iv); c.absScreenPos = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "hidden"))         { r.ReadInt(iv); c.hidden = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "sprite_id"))      { r.ReadInt(iv); c.spriteId = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "item_id"))        { r.ReadInt(iv); c.itemId = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "item_amount"))    { r.ReadInt(iv); c.itemAmount = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "parent"))         { r.ReadInt(iv); c.parentIndex = static_cast<int32_t>(iv); return; }
    if (StrEq(k, kn, "text"))
    {
        const char *s = nullptr;
        uint32_t sLen = 0;
        if (r.ReadString(s, sLen)) c.text.assign(s, sLen);
        return;
    }
    r.SkipValue();
}

bool DecodeOneComp(nxt::rpc::msgpack::Reader &r, Comp &out)
{
    uint32_t n = 0;
    if (!r.ReadMapHeader(n)) return false;
    for (uint32_t i = 0; i < n; ++i)
    {
        const char *k = nullptr;
        uint32_t    kn = 0;
        if (!r.ReadString(k, kn)) return false;
        AssignKey(out, k, kn, r);
    }
    return true;
}

// Decode a get_interface_tree reply: { count: N, nodes: [ <comp-map>... ] }.
bool DecodeTreeReply(const std::vector<uint8_t> &bytes, std::vector<Comp> &out)
{
    out.clear();
    if (bytes.empty()) return false;
    nxt::rpc::msgpack::Reader r(bytes.data(), bytes.size());
    uint32_t mc = 0;
    if (!r.ReadMapHeader(mc)) return false;
    for (uint32_t i = 0; i < mc; ++i)
    {
        const char *k = nullptr;
        uint32_t    kn = 0;
        if (!r.ReadString(k, kn)) return false;
        if (nxt::rpc::msgpack::StrEq(k, kn, "nodes"))
        {
            uint32_t arr = 0;
            if (!r.ReadArrayHeader(arr)) return false;
            out.reserve(arr);
            for (uint32_t j = 0; j < arr; ++j)
            {
                Comp c;
                if (!DecodeOneComp(r, c)) return false;
                out.push_back(std::move(c));
            }
        }
        else
        {
            r.SkipValue();
        }
    }
    return true;
}

bool DecodeSingleComp(const std::vector<uint8_t> &bytes, Comp &out)
{
    if (bytes.empty()) return false;
    nxt::rpc::msgpack::Reader r(bytes.data(), bytes.size());
    return DecodeOneComp(r, out);
}

// ---------------------------------------------------------------------------
// msgpack encoding helpers — small param maps
// ---------------------------------------------------------------------------

void EncodeIfaceComp(std::vector<uint8_t> &out, int32_t iface, int32_t comp)
{
    out.assign(kEncBufCap, 0);
    nxt::rpc::msgpack::Writer w(out.data(), out.size());
    w.WriteMapHeader(2);
    w.WriteCStr("iface"); w.WriteInt(iface);
    w.WriteCStr("comp");  w.WriteInt(comp);
    out.resize(w.BytesWritten());
}

void EncodeScreenXY(std::vector<uint8_t> &out, int32_t sx, int32_t sy)
{
    out.assign(kEncBufCap, 0);
    nxt::rpc::msgpack::Writer w(out.data(), out.size());
    w.WriteMapHeader(2);
    w.WriteCStr("screen_x"); w.WriteInt(sx);
    w.WriteCStr("screen_y"); w.WriteInt(sy);
    out.resize(w.BytesWritten());
}

// ---------------------------------------------------------------------------
// Panel state
// ---------------------------------------------------------------------------

struct OpenDiff
{
    int32_t id;
    float   added;     // seconds-since-event for the +badge; <= 0 when not "added"
    float   removed;   // seconds-since-event for the -badge
};

struct PanelState
{
    rpc::RpcClient       rpc;
    DWORD                lastAttachedPid = 0;

    // Pane 1
    std::vector<int32_t> prevOpen;
    std::vector<OpenDiff> diffs;
    char                 ifaceFilter[64] = {};

    // Pane 2
    int32_t              selectedIface   = -1;
    std::vector<Comp>    tree;
    std::vector<int32_t> treeDepth;
    char                 treeError[160]  = {};
    DWORD                lastTreeFetch   = 0;
    bool                 autoRefresh     = true;
    int                  refreshMs       = static_cast<int>(kTreeRefreshMs);
    char                 nodeFilter[64]  = {};
    std::unordered_set<int32_t> collapsed;   // comp ids the user has collapsed

    // Pane 3
    int32_t              selectedComp    = -1;
    Comp                 selected;
    bool                 selectedValid   = false;
    bool                 showRawGeom     = false;
    std::string          spriteName;
    std::string          itemName;
    int32_t              cachedSpriteId  = -2;
    int32_t              cachedItemId    = -2;

    // Pick mode
    bool                 pickActive      = false;
    bool                 pickInFlight    = false;
    DWORD                lastPickTick    = 0;
    POINT                lastPickPt      = {};
};

PanelState &State()
{
    static PanelState s;
    return s;
}

// ---------------------------------------------------------------------------
// Per-frame connection mgmt + diff tracking
// ---------------------------------------------------------------------------

void EnsureConnection(app::App &a, PanelState &s)
{
    DWORD pid = a.session.IsOpen() ? a.session.Pid() : 0;
    if (pid == s.lastAttachedPid && pid != 0 && s.rpc.IsConnected()) return;
    if (pid != s.lastAttachedPid)
    {
        s.rpc.Disconnect();
        s.prevOpen.clear();
        s.diffs.clear();
        s.tree.clear();
        s.treeDepth.clear();
        s.selectedIface = -1;
        s.selectedComp  = -1;
        s.selectedValid = false;
        s.pickActive    = false;
        s.pickInFlight  = false;
        s.lastAttachedPid = pid;
    }
    if (pid != 0 && !s.rpc.IsConnected())
    {
        s.rpc.Connect(pid);
    }
}

void UpdateDiffs(PanelState &s, const int32_t *open, uint32_t openCount,
                 float dtSec)
{
    // Age existing badges.
    for (auto &d : s.diffs)
    {
        if (d.added   > 0) d.added   -= dtSec;
        if (d.removed > 0) d.removed -= dtSec;
    }
    // GC expired entries.
    s.diffs.erase(std::remove_if(s.diffs.begin(), s.diffs.end(),
        [](const OpenDiff &d) { return d.added <= 0 && d.removed <= 0; }),
        s.diffs.end());

    auto contains = [](const int32_t *v, uint32_t n, int32_t id) {
        for (uint32_t i = 0; i < n; ++i)
        {
            if (v[i] == id) return true;
        }
        return false;
    };

    // Added since last frame.
    for (uint32_t i = 0; i < openCount; ++i)
    {
        if (!contains(s.prevOpen.data(), static_cast<uint32_t>(s.prevOpen.size()),
                      open[i]))
        {
            s.diffs.push_back({ open[i], kDiffBadgeSeconds, 0.0f });
        }
    }
    // Removed since last frame.
    for (int32_t id : s.prevOpen)
    {
        if (!contains(open, openCount, id))
        {
            s.diffs.push_back({ id, 0.0f, kDiffBadgeSeconds });
        }
    }

    s.prevOpen.assign(open, open + openCount);
}

const OpenDiff *FindDiff(const PanelState &s, int32_t id)
{
    for (const auto &d : s.diffs)
    {
        if (d.id == id) return &d;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// RPC plumbing — synchronous Call wrappers
// ---------------------------------------------------------------------------

bool CallTree(PanelState &s, int32_t iface)
{
    if (!s.rpc.IsConnected()) return false;
    std::vector<uint8_t> params;
    EncodeIfaceComp(params, iface, 0);
    std::vector<uint8_t> reply;
    reply.reserve(kReplyBufCap);
    auto st = s.rpc.Call("get_interface_tree", params.data(),
                         static_cast<uint32_t>(params.size()),
                         reply, kRpcTimeoutMs);
    if (st != rpc::CallStatus::Ok)
    {
        std::snprintf(s.treeError, sizeof(s.treeError),
                      "get_interface_tree status=%d", static_cast<int>(st));
        s.tree.clear();
        s.treeDepth.clear();
        return false;
    }
    if (!DecodeTreeReply(reply, s.tree))
    {
        std::snprintf(s.treeError, sizeof(s.treeError),
                      "tree reply did not decode");
        s.tree.clear();
        s.treeDepth.clear();
        return false;
    }
    s.treeError[0] = 0;
    s.treeDepth.assign(s.tree.size(), 0);
    for (size_t i = 0; i < s.tree.size(); ++i)
    {
        int32_t pi = s.tree[i].parentIndex;
        s.treeDepth[i] = (pi < 0) ? 0 : s.treeDepth[pi] + 1;
    }
    return true;
}

bool CallGetComponent(PanelState &s, int32_t iface, int32_t comp, Comp &out)
{
    if (!s.rpc.IsConnected()) return false;
    std::vector<uint8_t> params;
    EncodeIfaceComp(params, iface, comp);
    std::vector<uint8_t> reply;
    auto st = s.rpc.Call("get_component", params.data(),
                         static_cast<uint32_t>(params.size()),
                         reply, kRpcTimeoutMs);
    if (st != rpc::CallStatus::Ok) return false;
    Comp tmp;
    if (!DecodeSingleComp(reply, tmp)) return false;
    if (tmp.iface < 0) return false;
    out = std::move(tmp);
    return true;
}

bool CallFindAt(PanelState &s, int32_t sx, int32_t sy, Comp &out)
{
    if (!s.rpc.IsConnected()) return false;
    std::vector<uint8_t> params;
    EncodeScreenXY(params, sx, sy);
    std::vector<uint8_t> reply;
    auto st = s.rpc.Call("find_component_at", params.data(),
                         static_cast<uint32_t>(params.size()),
                         reply, kRpcTimeoutMs);
    if (st != rpc::CallStatus::Ok) return false;
    Comp tmp;
    if (!DecodeSingleComp(reply, tmp)) return false;
    if (tmp.iface < 0) return false;
    out = std::move(tmp);
    return true;
}

// ---------------------------------------------------------------------------
// Selection management
// ---------------------------------------------------------------------------

void ResolveCacheNames(app::App &a, PanelState &s)
{
    if (!s.selectedValid)
    {
        s.spriteName.clear();
        s.itemName.clear();
        s.cachedSpriteId = -2;
        s.cachedItemId   = -2;
        return;
    }
    if (s.selected.spriteId != s.cachedSpriteId)
    {
        s.cachedSpriteId = s.selected.spriteId;
        s.spriteName.clear();
        // No "sprite" decode in NXTCache yet — defer.
    }
    if (s.selected.itemId != s.cachedItemId)
    {
        s.cachedItemId = s.selected.itemId;
        s.itemName.clear();
        if (s.selected.itemId >= 0 && a.cache.IsOpen())
        {
            std::string json = a.cache.GetJson("item", s.selected.itemId);
            const char *p = std::strstr(json.c_str(), "\"name\"");
            if (p)
            {
                p = std::strchr(p, ':');
                if (p)
                {
                    ++p;
                    while (*p == ' ' || *p == '\t') ++p;
                    if (*p == '"')
                    {
                        ++p;
                        const char *q = std::strchr(p, '"');
                        if (q) s.itemName.assign(p, q - p);
                    }
                }
            }
        }
    }
}

void SelectComponent(app::App &a, PanelState &s, int32_t iface, int32_t comp)
{
    s.selectedIface  = iface;
    s.selectedComp   = comp;
    s.selectedValid  = false;
    if (CallGetComponent(s, iface, comp, s.selected))
    {
        s.selectedValid = true;
    }
    ResolveCacheNames(a, s);
}

// Apply an already-decoded Comp as the new selection without an extra RPC.
void AdoptComponent(app::App &a, PanelState &s, const Comp &c)
{
    s.selectedIface = c.iface;
    s.selectedComp  = c.comp;
    s.selected      = c;
    s.selectedValid = true;
    ResolveCacheNames(a, s);
}

void ClearSelection(PanelState &s)
{
    s.selectedIface = -1;
    s.selectedComp  = -1;
    s.selectedValid = false;
    s.tree.clear();
    s.treeDepth.clear();
    s.treeError[0]  = 0;
}

// ---------------------------------------------------------------------------
// Stale-tree detection (Q2: auto-clear via snapshot poll)
// ---------------------------------------------------------------------------

void HandleStaleTree(PanelState &s, const int32_t *open, uint32_t openCount)
{
    if (s.selectedIface < 0) return;
    for (uint32_t i = 0; i < openCount; ++i)
    {
        if (open[i] == s.selectedIface) return;
    }
    ClearSelection(s);
}

// ---------------------------------------------------------------------------
// Pick mode update (Q3: in-flight flag bounds queue to 1)
// ---------------------------------------------------------------------------

void UpdatePickMode(app::App &a, PanelState &s)
{
    if (!s.pickActive) { s.pickInFlight = false; return; }
    if (!s.rpc.IsConnected()) return;
    if (s.pickInFlight) return;

    DWORD now = GetTickCount();
    if (now - s.lastPickTick < kPickPollMs) return;

    POINT pt{};
    if (!GetCursorPos(&pt)) return;
    if (pt.x == s.lastPickPt.x && pt.y == s.lastPickPt.y)
    {
        s.lastPickTick = now;
        return;
    }
    s.lastPickPt = pt;
    s.lastPickTick = now;

    s.pickInFlight = true;
    Comp hit;
    bool ok = CallFindAt(s, pt.x, pt.y, hit);
    s.pickInFlight = false;
    if (!ok)
    {
        // Miss leaves the previous selection in place; users get to keep
        // staring at the last component they hovered.
        return;
    }
    AdoptComponent(a, s, hit);
    s.selectedIface = hit.iface;
}

// ---------------------------------------------------------------------------
// Pane 1 — Open Interfaces
// ---------------------------------------------------------------------------

bool RowMatchesFilter(int32_t id, const char *filter)
{
    if (!filter || !filter[0]) return true;
    char idBuf[16];
    std::snprintf(idBuf, sizeof(idBuf), "%d", id);
    if (std::strstr(idBuf, filter)) return true;
    const char *name = LookupIfaceName(id);
    if (name)
    {
        // Case-insensitive substring.
        for (size_t i = 0; name[i]; ++i)
        {
            bool ok = true;
            for (size_t j = 0; filter[j]; ++j)
            {
                if (!name[i + j]) { ok = false; break; }
                char a = name[i + j];   if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 32);
                char b = filter[j];     if (b >= 'a' && b <= 'z') b = static_cast<char>(b - 32);
                if (a != b) { ok = false; break; }
            }
            if (ok) return true;
        }
    }
    return false;
}

void DrawIfaceRow(app::App &a, PanelState &s, int32_t id)
{
    if (!RowMatchesFilter(id, s.ifaceFilter)) return;

    char label[96];
    const char *name = LookupIfaceName(id);
    if (name) std::snprintf(label, sizeof(label), "%d  %s", id, name);
    else      std::snprintf(label, sizeof(label), "iface %d", id);

    bool selected = (id == s.selectedIface);
    if (ImGui::Selectable(label, selected))
    {
        if (id != s.selectedIface)
        {
            s.selectedIface = id;
            s.selectedComp  = -1;
            s.selectedValid = false;
            s.lastTreeFetch = 0;
            CallTree(s, id);
            // Auto-select the root of the freshly-loaded tree so Pane 3
            // shows something instead of an empty card.
            if (!s.tree.empty()) AdoptComponent(a, s, s.tree.front());
        }
    }
    const OpenDiff *d = FindDiff(s, id);
    if (d)
    {
        ImGui::SameLine();
        if (d->added > 0)
        {
            theme::Pill("+", theme::kGoodSoft, theme::kGood);
        }
        else if (d->removed > 0)
        {
            theme::Pill("-", (theme::kBad & 0x00FFFFFFu) | (60u << 24), theme::kBad);
        }
    }
}

void DrawPaneOpenIfaces(app::App &a, PanelState &s,
                        const int32_t *open, uint32_t openCount)
{
    if (!theme::BeginCard("iface.open", "OPEN INTERFACES", theme::kAccent, true)) { theme::EndCard(); return; }

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##ifacefilter", "filter (id or name)",
                             s.ifaceFilter, sizeof(s.ifaceFilter));

    ImGui::Spacing();
    char hero[24];
    std::snprintf(hero, sizeof(hero), "%u", openCount);
    theme::HeroStat("open", hero, theme::kAccent);

    ImGui::Separator();
    ImGui::BeginChild("##ifaces", ImVec2(0, 0), 0);
    for (uint32_t i = 0; i < openCount; ++i)
    {
        DrawIfaceRow(a, s, open[i]);
    }
    ImGui::EndChild();

    theme::EndCard();
}

// ---------------------------------------------------------------------------
// Pane 2 — Component Tree
// ---------------------------------------------------------------------------

bool NodeMatchesFilter(const Comp &c, const char *filter)
{
    if (!filter || !filter[0]) return true;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%d %d %d %s",
                  c.comp, c.spriteId, c.itemId, c.text.c_str());
    return std::strstr(buf, filter) != nullptr;
}

void NodeSummary(const Comp &c, char *out, size_t cap)
{
    using nxt::game::interfaces::WireCategory;
    const auto cat = static_cast<WireCategory>(c.category);
    if (cat == WireCategory::Text && !c.text.empty())
    {
        std::snprintf(out, cap, "\"%.*s\"", 40, c.text.c_str());
        return;
    }
    if (cat == WireCategory::Model && (c.itemId >= 0 || c.itemAmount > 0))
    {
        std::snprintf(out, cap, "item %d x%d", c.itemId, c.itemAmount);
        return;
    }
    if (c.spriteId >= 0)
    {
        std::snprintf(out, cap, "sprite %d", c.spriteId);
        return;
    }
    std::snprintf(out, cap, "%dx%d @ (%d,%d)", c.w, c.h, c.x, c.y);
}

void DrawTreeControls(PanelState &s)
{
    if (ImGui::Button("Refresh"))
    {
        if (s.selectedIface >= 0) CallTree(s, s.selectedIface);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto", &s.autoRefresh);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
    ImGui::DragInt("ms", &s.refreshMs, 10, 100, 5000);
    ImGui::SameLine(0, ImGui::GetFontSize());
    bool pickWas = s.pickActive;
    ImGui::Checkbox("Pick mode", &s.pickActive);
    if (pickWas != s.pickActive)
    {
        s.pickInFlight = false;
        s.lastPickPt   = {};
    }
}

void DrawTreeNodeRow(app::App &a, PanelState &s, size_t i)
{
    const Comp &c = s.tree[i];
    if (!NodeMatchesFilter(c, s.nodeFilter)) return;

    ImGui::PushID(static_cast<int>(i));
    char header[160];
    char summary[120];
    NodeSummary(c, summary, sizeof(summary));
    std::snprintf(header, sizeof(header), "%*scomp %d  [%s]  %s",
                  s.treeDepth[i] * 2, "",
                  c.comp, CategoryName(c.category), summary);

    bool isSelected = s.selectedValid
                   && s.selected.iface == c.iface
                   && s.selected.comp  == c.comp;

    ImU32 color = (c.hidden == 1) ? theme::kTextDim : theme::kText;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
    if (ImGui::Selectable(header, isSelected))
    {
        AdoptComponent(a, s, c);
    }
    ImGui::PopStyleColor();
    ImGui::PopID();
}

void DrawPaneTree(app::App &a, PanelState &s)
{
    if (!theme::BeginCard("iface.tree", "COMPONENT TREE", theme::kAccent, true)) { theme::EndCard(); return; }

    DrawTreeControls(s);

    if (s.selectedIface < 0)
    {
        ImGui::TextDisabled("(select an interface)");
        theme::EndCard();
        return;
    }

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##nodefilter", "filter (id / text / sprite / item)",
                             s.nodeFilter, sizeof(s.nodeFilter));

    if (s.treeError[0])
    {
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::ColorConvertU32ToFloat4(theme::kBad));
        ImGui::TextWrapped("%s", s.treeError);
        ImGui::PopStyleColor();
    }
    ImGui::Separator();

    ImGui::BeginChild("##nodes", ImVec2(0, 0), 0);
    for (size_t i = 0; i < s.tree.size(); ++i)
    {
        DrawTreeNodeRow(a, s, i);
    }
    ImGui::EndChild();

    theme::EndCard();
}

// ---------------------------------------------------------------------------
// Pane 3 — Selected Component
// ---------------------------------------------------------------------------

void KeyLineInt(const char *label, int32_t v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", v);
    theme::KeyLine(label, buf);
}

void DrawSelectedIdentity(const Comp &c)
{
    KeyLineInt("iface", c.iface);
    KeyLineInt("comp",  c.comp);
    KeyLineInt("sub",   c.sub);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d (%s)", c.type, CategoryName(c.category));
    theme::KeyLine("type", buf);
}

void DrawSelectedGeometry(PanelState &s, const Comp &c)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d x %d", c.w, c.h);
    theme::KeyLine("size",   buf);
    std::snprintf(buf, sizeof(buf), "(%d, %d)", c.x, c.y);
    theme::KeyLine("at",     buf);
    KeyLineInt("hidden",         c.hidden);
    KeyLineInt("abs_screen_pos", c.absScreenPos);

    ImGui::Checkbox("show raw geometry", &s.showRawGeom);
    if (s.showRawGeom)
    {
        std::snprintf(buf, sizeof(buf), "%d x %d", c.rawW, c.rawH);
        theme::KeyLine("raw size", buf);
        std::snprintf(buf, sizeof(buf), "(%d, %d)", c.rawX, c.rawY);
        theme::KeyLine("raw at",   buf);
        std::snprintf(buf, sizeof(buf), "%d / %d  (pos)", c.xPosMode, c.yPosMode);
        theme::KeyLine("pos mode", buf);
        std::snprintf(buf, sizeof(buf), "%d / %d  (size)", c.xSizeMode, c.ySizeMode);
        theme::KeyLine("size mode", buf);
    }
}

void DrawSelectedContent(const PanelState &s, const Comp &c)
{
    char buf[80];
    if (!c.text.empty())
    {
        std::snprintf(buf, sizeof(buf), "\"%.*s\"%s",
                      60, c.text.c_str(),
                      c.text.size() > 60 ? "…" : "");
        theme::KeyLine("text", buf);
    }
    if (c.spriteId >= 0)
    {
        std::snprintf(buf, sizeof(buf), "%d%s%s",
                      c.spriteId,
                      s.spriteName.empty() ? "" : "  ",
                      s.spriteName.c_str());
        theme::KeyLine("sprite_id", buf);
    }
    if (c.itemId >= 0)
    {
        std::snprintf(buf, sizeof(buf), "%d x%d%s%s",
                      c.itemId, c.itemAmount,
                      s.itemName.empty() ? "" : "  ",
                      s.itemName.c_str());
        theme::KeyLine("item", buf);
    }
}

void DrawSelectedActions(app::App &a, PanelState &s)
{
    if (ImGui::Button("Re-fetch"))
    {
        SelectComponent(a, s, s.selected.iface, s.selected.comp);
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy ref"))
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d:%d",
                      s.selected.iface, s.selected.comp);
        ImGui::SetClipboardText(buf);
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy hash"))
    {
        // Mirrors JBotWithUsV2 Interfaces.componentHash:
        // (iface << 16) | comp
        char buf[32];
        const int64_t hash =
            (static_cast<int64_t>(s.selected.iface) << 16)
            | (static_cast<int64_t>(s.selected.comp) & 0xFFFF);
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(hash));
        ImGui::SetClipboardText(buf);
    }
}

void DrawPaneSelected(app::App &a, PanelState &s)
{
    if (!theme::BeginCard("iface.sel", "SELECTED COMPONENT")) { theme::EndCard(); return; }
    if (!s.selectedValid)
    {
        ImGui::TextDisabled("(no selection)");
        theme::EndCard();
        return;
    }
    const Comp &c = s.selected;
    DrawSelectedIdentity(c);
    theme::AccentRule();
    DrawSelectedGeometry(s, c);
    theme::AccentRule();
    DrawSelectedContent(s, c);
    theme::AccentRule();
    DrawSelectedActions(a, s);
    theme::EndCard();
}

// ---------------------------------------------------------------------------
// Per-frame top-level update + layout
// ---------------------------------------------------------------------------

// Mirror the box-relevant slice of the freshly-fetched tree into the shared
// App view-model so the external overlay can paint it. Sole writer of
// app::App::interfaceView. Cheap: a shallow copy of a few hundred small PODs,
// piggybacking on data the panel already fetched — no extra RPC.
void PublishOverlayView(app::App &a, PanelState &s)
{
    app::InterfaceView &v = a.interfaceView;
    v.iface      = s.selectedIface;
    v.pickActive = s.pickActive;
    v.hoverComp  = s.selectedValid ? s.selected.comp : -1;
    v.boxes.clear();
    v.boxes.reserve(s.tree.size());
    for (const Comp &c : s.tree)
    {
        if (c.hidden == 1) continue;   // overlay paints visible nodes only
        app::OverlayBox b;
        b.x        = c.x;
        b.y        = c.y;
        b.w        = c.w;
        b.h        = c.h;
        b.category = c.category;
        b.comp     = c.comp;
        b.hidden   = c.hidden;
        std::snprintf(b.label, sizeof(b.label), "%d %s",
                      c.comp, CategoryName(c.category));
        v.boxes.push_back(b);
    }
    ++v.rev;
}

void MaybeAutoRefresh(app::App &a, PanelState &s)
{
    if (!s.autoRefresh) return;
    if (s.selectedIface < 0) return;
    DWORD now = GetTickCount();
    if (now - s.lastTreeFetch < static_cast<DWORD>(s.refreshMs)) return;
    CallTree(s, s.selectedIface);
    s.lastTreeFetch = now;
    // Refresh the selection from the new tree if it's still there.
    if (s.selectedValid)
    {
        for (const auto &n : s.tree)
        {
            if (n.iface == s.selected.iface && n.comp == s.selected.comp)
            {
                AdoptComponent(a, s, n);
                break;
            }
        }
    }
}

}

void DrawInterfacePanel(app::App &a)
{
    ImGui::SetNextWindowSize(
        ImVec2(ImGui::GetFontSize() * 100, ImGui::GetFontSize() * 40),
        ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Interfaces"))
    {
        ImGui::End();
        return;
    }

    PanelState &s = State();
    EnsureConnection(a, s);

    const auto *snap = wire::CurrentSnapshot(a.session);
    const int32_t *open = snap ? snap->openIfaces : nullptr;
    const uint32_t openCount = snap ? snap->openIfaceCount : 0;
    const float dt = ImGui::GetIO().DeltaTime;

    if (snap)
    {
        UpdateDiffs(s, open, openCount, dt);
        HandleStaleTree(s, open, openCount);
    }
    MaybeAutoRefresh(a, s);
    UpdatePickMode(a, s);
    PublishOverlayView(a, s);

    // Pane widths: cap at comfortable em widths on a wide window, but shrink to
    // a fraction of the available width when the window is narrow — docked, or
    // hi-DPI where GetFontSize() is already ~2x and fs*22 + fs*40 alone can
    // exceed the whole window. Without this the tree + selected panes render
    // off the right edge and only Open Interfaces is visible. paneC takes the
    // remainder, floored so it never collapses to nothing.
    const float fs    = ImGui::GetFontSize();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float colA  = std::min(fs * 22.0f, avail * 0.28f);
    const float colB  = std::min(fs * 40.0f, avail * 0.44f);

    ImGui::BeginChild("##paneA", ImVec2(colA, 0), 0);
    DrawPaneOpenIfaces(a, s, open, openCount);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##paneB", ImVec2(colB, 0), 0);
    DrawPaneTree(a, s);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##paneC", ImVec2(0, 0), 0);
    DrawPaneSelected(a, s);
    ImGui::EndChild();

    ImGui::End();
}

}
