#include "Panels.h"

#include "app/App.h"
#include "app/Theme.h"
#include "cache/CacheClient.h"
#include "cache/Json.h"
#include "render/Texture.h"

#include "imgui.h"

#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nxtdbg::panels
{

namespace
{

// ---------------------------------------------------------------------------
// Browsable config types. "varp" is synthetic — the cache has no varp def, so
// its detail view is a reverse index (which varbits sit on the varp) plus the
// CS2 xref (Phase 6b). hasIcon flags the types we can render a thumbnail for.
// ---------------------------------------------------------------------------

struct BrowseType
{
    const char *name;
    bool        synthetic;   // no GetJson decode (varp)
    bool        itemIcon;    // nxt_render_item_icon
    bool        spriteIcon;  // nxt_get_sprite_frame_rgba
};

constexpr BrowseType kBrowseTypes[] = {
    { "item",     false, true,  false },
    { "npc",      false, false, false },
    { "loc",      false, false, false },
    { "varbit",   false, false, false },
    { "varp",     true,  false, false },
    { "struct",   false, false, false },
    { "enum",     false, false, false },
    { "param",    false, false, false },
    { "inv",      false, false, false },
    { "seq",      false, false, false },
    { "sprite",   false, false, true  },
    { "model",    false, false, false },
    { "dbrow",    false, false, false },
    { "quest",    false, false, false },
    { "worldmap", false, false, false },
    { "underlay", false, false, false },
    { "overlay",  false, false, false },
    { "if",       false, false, false },
};

constexpr int kBrowseTypeCount = static_cast<int>(sizeof(kBrowseTypes) / sizeof(kBrowseTypes[0]));

// Known scalar id-fields that should render as a clickable cross-link, and the
// browse type they point at.
struct IdField
{
    const char *field;
    const char *type;
};

constexpr IdField kIdFields[] = {
    { "varbitId",        "varbit" },
    { "varpId",          "varp"   },
    { "modelID",         "model"  },
    { "modelId",         "model"  },
    { "notedID",         "item"   },
    { "templateID",      "item"   },
    { "lentItemId",      "item"   },
    { "lendTemplate",    "item"   },
    { "bindId",          "item"   },
    { "boundTemplate",   "item"   },
    { "shardItemId",     "item"   },
    { "shardTemplateId", "item"   },
    { "mapSpriteId",     "sprite" },
    { "mapAreaId",       "worldmap" },
};

// ---------------------------------------------------------------------------
// Panel state
// ---------------------------------------------------------------------------

struct Target
{
    int typeIdx;
    int id;
};

struct BrowserState
{
    int  typeIdx    = 0;
    int  listedType = -1;             // which type ids[] currently holds
    std::vector<int> ids;             // ListTypeIds for the current type
    char filter[64] = {};
    std::string      filterApplied;   // last filter RecomputeFiltered ran on
    bool             idsDirty = true;
    std::vector<int> filtered;

    std::unordered_map<int, std::string> nameCache;   // current-type id -> name
    bool   nameIndexing  = false;
    bool   nameIndexBuilt = false;
    size_t nameIndexPos  = 0;

    int  selectedId  = -1;
    std::string      detailJson;
    cache::JsonValue detail;
    bool detailValid = false;

    render::TextureCache itemIcons;
    render::TextureCache spriteIcons;
    std::unordered_set<int> iconFailed;
    std::unordered_set<int> spriteFailed;

    std::vector<Target> history;
    int  histPos = -1;

    std::unordered_map<int, std::string> paramTypeCache;   // paramId -> type name

    bool varpMapBuilt = false;
    std::unordered_map<int, std::vector<int>> varpToVarbits;

    bool showRawJson = false;
};

BrowserState &State()
{
    static BrowserState s;
    return s;
}

ImVec4 Vec4(ImU32 c)
{
    return ImGui::ColorConvertU32ToFloat4(c);
}

// ImTextureID is an integer type in this ImGui build, so a plain GLuint maps
// through intptr_t with a C-style cast (covers both the integer and void*
// flavours across ImGui versions).
ImTextureID ToTexId(unsigned int tex)
{
    return (ImTextureID)(intptr_t)tex;
}

const char *TypeName(const BrowserState &s)
{
    return kBrowseTypes[s.typeIdx].name;
}

int FindTypeIdx(const char *name)
{
    for (int i = 0; i < kBrowseTypeCount; ++i)
    {
        if (std::strcmp(kBrowseTypes[i].name, name) == 0)
        {
            return i;
        }
    }
    return -1;
}

bool ContainsNoCase(const char *hay, const char *needle)
{
    if (!needle || !needle[0])
    {
        return true;
    }
    for (size_t i = 0; hay[i]; ++i)
    {
        size_t j = 0;
        for (; needle[j]; ++j)
        {
            char a = hay[i + j];
            if (!a)
            {
                return false;
            }
            if (a >= 'A' && a <= 'Z')
            {
                a = static_cast<char>(a + 32);
            }
            char b = needle[j];
            if (b >= 'A' && b <= 'Z')
            {
                b = static_cast<char>(b + 32);
            }
            if (a != b)
            {
                break;
            }
        }
        if (!needle[j])
        {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Cache open card (compact; shares the App-level handle with Cache lookup)
// ---------------------------------------------------------------------------

void DefaultCachePath(char *out, size_t n)
{
    wchar_t local[MAX_PATH];
    DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    wchar_t w[MAX_PATH];
    if (got == 0 || got >= MAX_PATH)
    {
        std::swprintf(w, MAX_PATH, L"%s", L"C:\\");
    }
    else
    {
        std::swprintf(w, MAX_PATH, L"%s\\Jagex\\RuneScape\\Cache", local);
    }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, static_cast<int>(n), nullptr, nullptr);
}

void DrawCacheOpen(app::App &a)
{
    if (!theme::BeginCard("cbrowse.open", "CACHE"))
    {
        theme::EndCard();
        return;
    }
    static char path[MAX_PATH] = {};
    if (path[0] == 0)
    {
        DefaultCachePath(path, sizeof(path));
    }
    theme::StatusDot(a.cache.IsOpen() ? theme::kGood : theme::kTextDim,
                     a.cache.IsOpen(), a.dotPhase);
    ImGui::SameLine();
    theme::HeroStat("local cache", a.cache.IsOpen() ? "OPEN" : "CLOSED",
                    a.cache.IsOpen() ? theme::kGood : theme::kTextDim);

    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.72f);
    ImGui::InputText("##cbpath", path, sizeof(path));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button(a.cache.IsOpen() ? "Reopen" : "Open"))
    {
        wchar_t w[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, path, -1, w, MAX_PATH);
        a.cache.Open(w);
        State().listedType = -1;   // force id reload against the new handle
    }
    if (a.cache.LastError()[0] != 0)
    {
        char e[512];
        WideCharToMultiByte(CP_UTF8, 0, a.cache.LastError(), -1, e, sizeof(e), nullptr, nullptr);
        ImGui::PushStyleColor(ImGuiCol_Text, Vec4(theme::kBad));
        ImGui::TextWrapped("%s", e);
        ImGui::PopStyleColor();
    }
    theme::EndCard();
}

// ---------------------------------------------------------------------------
// Id list + name resolution
// ---------------------------------------------------------------------------

void ReloadIds(app::App &a, BrowserState &s)
{
    s.listedType = s.typeIdx;
    s.nameCache.clear();
    s.nameIndexing  = false;
    s.nameIndexBuilt = false;
    s.nameIndexPos  = 0;
    s.idsDirty      = true;
    if (kBrowseTypes[s.typeIdx].synthetic)
    {
        s.ids.clear();   // varp has no enumerable def list
        return;
    }
    s.ids = a.cache.ListTypeIds(TypeName(s));
}

const char *ResolveName(app::App &a, BrowserState &s, int id, int *budget)
{
    auto it = s.nameCache.find(id);
    if (it != s.nameCache.end())
    {
        return it->second.empty() ? nullptr : it->second.c_str();
    }
    if (budget && *budget <= 0)
    {
        return nullptr;   // defer to a later frame
    }
    if (budget)
    {
        --*budget;
    }
    std::string nm;
    std::string json = a.cache.GetJson(TypeName(s), id);
    if (!json.empty())
    {
        cache::JsonValue v;
        if (cache::ParseJson(json, v))
        {
            const cache::JsonValue *n = v.Find("name");
            if (n && n->IsString() && n->strVal != "null")
            {
                nm = n->strVal;
            }
        }
    }
    auto res = s.nameCache.emplace(id, std::move(nm));
    return res.first->second.empty() ? nullptr : res.first->second.c_str();
}

bool RowMatches(BrowserState &s, int id)
{
    if (!s.filter[0])
    {
        return true;
    }
    char idBuf[16];
    std::snprintf(idBuf, sizeof(idBuf), "%d", id);
    if (std::strstr(idBuf, s.filter))
    {
        return true;
    }
    auto it = s.nameCache.find(id);
    if (it != s.nameCache.end() && !it->second.empty())
    {
        return ContainsNoCase(it->second.c_str(), s.filter);
    }
    return false;
}

void RecomputeFiltered(BrowserState &s)
{
    bool needRecompute = s.idsDirty
                      || s.filterApplied != s.filter
                      || s.nameIndexing;
    if (!needRecompute)
    {
        return;
    }
    s.filtered.clear();
    if (!s.filter[0])
    {
        s.filtered = s.ids;
    }
    else
    {
        for (int id : s.ids)
        {
            if (RowMatches(s, id))
            {
                s.filtered.push_back(id);
            }
        }
    }
    s.filterApplied = s.filter;
    s.idsDirty      = false;
}

void PumpNameIndex(app::App &a, BrowserState &s)
{
    if (!s.nameIndexing)
    {
        return;
    }
    int budget = 512;
    while (budget > 0 && s.nameIndexPos < s.ids.size())
    {
        ResolveName(a, s, s.ids[s.nameIndexPos], &budget);
        ++s.nameIndexPos;
    }
    if (s.nameIndexPos >= s.ids.size())
    {
        s.nameIndexing   = false;
        s.nameIndexBuilt = true;
        s.idsDirty       = true;   // refresh name-filtered view
    }
}

// ---------------------------------------------------------------------------
// Selection + navigation history
// ---------------------------------------------------------------------------

void Load(app::App &a, BrowserState &s, int typeIdx, int id)
{
    if (typeIdx < 0 || typeIdx >= kBrowseTypeCount)
    {
        return;
    }
    if (typeIdx != s.typeIdx || typeIdx != s.listedType)
    {
        s.typeIdx = typeIdx;
        ReloadIds(a, s);
    }
    s.selectedId  = id;
    s.detailValid = false;
    s.detailJson.clear();
    s.detail = cache::JsonValue{};
    if (!kBrowseTypes[typeIdx].synthetic)
    {
        s.detailJson = a.cache.GetJson(TypeName(s), id);
        if (!s.detailJson.empty())
        {
            s.detailValid = cache::ParseJson(s.detailJson, s.detail);
        }
    }
}

void Navigate(app::App &a, BrowserState &s, int typeIdx, int id)
{
    Load(a, s, typeIdx, id);
    // Truncate any forward history, then push.
    if (s.histPos + 1 < static_cast<int>(s.history.size()))
    {
        s.history.resize(static_cast<size_t>(s.histPos + 1));
    }
    s.history.push_back({ typeIdx, id });
    s.histPos = static_cast<int>(s.history.size()) - 1;
}

void GoHistory(app::App &a, BrowserState &s, int delta)
{
    int pos = s.histPos + delta;
    if (pos < 0 || pos >= static_cast<int>(s.history.size()))
    {
        return;
    }
    s.histPos = pos;
    Load(a, s, s.history[static_cast<size_t>(pos)].typeIdx,
         s.history[static_cast<size_t>(pos)].id);
}

// ---------------------------------------------------------------------------
// Left pane — type selector + id list
// ---------------------------------------------------------------------------

void DrawTypeCombo(app::App &a, BrowserState &s)
{
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9);
    if (ImGui::BeginCombo("##cbtype", TypeName(s)))
    {
        for (int i = 0; i < kBrowseTypeCount; ++i)
        {
            bool sel = (i == s.typeIdx);
            if (ImGui::Selectable(kBrowseTypes[i].name, sel) && i != s.typeIdx)
            {
                s.typeIdx    = i;
                ReloadIds(a, s);
                s.selectedId = -1;
                s.detailValid = false;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    char count[24];
    std::snprintf(count, sizeof(count), "%zu", s.ids.size());
    theme::Pill(count, theme::kAccentSoft, theme::kAccent);
}

void DrawNameIndexControl(BrowserState &s)
{
    if (kBrowseTypes[s.typeIdx].synthetic)
    {
        return;
    }
    if (s.nameIndexing)
    {
        float frac = s.ids.empty() ? 1.0f
                   : static_cast<float>(s.nameIndexPos) / static_cast<float>(s.ids.size());
        theme::ProgressStrip(frac, theme::kAccent, "indexing names…");
    }
    else if (!s.nameIndexBuilt && !s.ids.empty())
    {
        if (ImGui::SmallButton("Index names for search"))
        {
            s.nameIndexing  = true;
            s.nameIndexPos  = 0;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Decode every entry once so the filter can match names,\n"
                              "not just ids. Cached until you switch type.");
        }
    }
}

void DrawIdRow(app::App &a, BrowserState &s, int id, int *budget)
{
    const char *name = kBrowseTypes[s.typeIdx].synthetic
                     ? nullptr : ResolveName(a, s, id, budget);
    char label[128];
    if (name)
    {
        std::snprintf(label, sizeof(label), "%-7d %s", id, name);
    }
    else
    {
        std::snprintf(label, sizeof(label), "%-7d", id);
    }
    ImGui::PushID(id);
    bool selected = (id == s.selectedId);
    if (ImGui::Selectable(label, selected))
    {
        Navigate(a, s, s.typeIdx, id);
    }
    ImGui::PopID();
}

void DrawSyntheticVarpEntry(app::App &a, BrowserState &s)
{
    theme::Subheading("varp has no cache definition. Enter a varp id to inspect\n"
                      "the varbits riding on it (and CS2 xref in Phase 6b).");
    ImGui::Spacing();
    static int varpId = 0;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    ImGui::InputInt("##varpid", &varpId);
    ImGui::SameLine();
    if (ImGui::Button("Inspect") && varpId >= 0)
    {
        Navigate(a, s, s.typeIdx, varpId);
    }
}

void DrawLeftPane(app::App &a, BrowserState &s)
{
    if (!theme::BeginCard("cbrowse.list", "TYPES"))
    {
        theme::EndCard();
        return;
    }
    DrawTypeCombo(a, s);

    if (kBrowseTypes[s.typeIdx].synthetic)
    {
        ImGui::Separator();
        DrawSyntheticVarpEntry(a, s);
        theme::EndCard();
        return;
    }

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##cbfilter", "filter (id or name)", s.filter, sizeof(s.filter));
    DrawNameIndexControl(s);
    RecomputeFiltered(s);

    ImGui::Separator();
    ImGui::BeginChild("##cbids", ImVec2(0, 0), 0);
    int budget = 96;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(s.filtered.size()));
    while (clipper.Step())
    {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            DrawIdRow(a, s, s.filtered[static_cast<size_t>(i)], &budget);
        }
    }
    ImGui::EndChild();
    theme::EndCard();
}

// ---------------------------------------------------------------------------
// Cross-link primitives
// ---------------------------------------------------------------------------

bool TextLink(const char *label, ImU32 col = theme::kAccent)
{
    ImGui::TextColored(Vec4(col), "%s", label);
    bool clicked = ImGui::IsItemClicked();
    if (ImGui::IsItemHovered())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImVec2 mn = ImGui::GetItemRectMin();
        ImVec2 mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mx.y), ImVec2(mx.x, mx.y), col, 1.0f);
    }
    return clicked;
}

// Right-aligned clickable value, dim label on the left (KeyLine that links).
bool KeyLink(const char *label, const char *value)
{
    ImGui::PushStyleColor(ImGuiCol_Text, Vec4(theme::kTextDim));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    float avail = ImGui::GetContentRegionAvail().x;
    float vw    = ImGui::CalcTextSize(value).x;
    float pad   = avail - vw;
    if (pad > 0.0f)
    {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    }
    return TextLink(value);
}

// ScriptVarType getName() substring -> browse type, for param/enum value links.
struct VarTypeLink
{
    const char *needle;
    const char *type;
};

constexpr VarTypeLink kVarTypeLinks[] = {
    { "ITEM",   "item"   },
    { "NPC",    "npc"    },
    { "LOC",    "loc"    },
    { "ENUM",   "enum"   },
    { "STRUCT", "struct" },
    { "MODEL",  "model"  },
    { "INV",    "inv"    },
    { "ANIM",   "seq"    },
};

const char *ParamLinkType(const std::string &typeName)
{
    if (typeName.empty())
    {
        return nullptr;
    }
    for (const auto &l : kVarTypeLinks)
    {
        if (std::strstr(typeName.c_str(), l.needle))
        {
            return l.type;
        }
    }
    return nullptr;
}

const std::string &ParamTypeName(app::App &a, BrowserState &s, int paramId)
{
    auto it = s.paramTypeCache.find(paramId);
    if (it != s.paramTypeCache.end())
    {
        return it->second;
    }
    std::string tn;
    std::string json = a.cache.GetJson("param", paramId);
    if (!json.empty())
    {
        cache::JsonValue v;
        if (cache::ParseJson(json, v))
        {
            const cache::JsonValue *t = v.Find("type");
            if (t && t->IsString())
            {
                tn = t->strVal;
            }
        }
    }
    return s.paramTypeCache.emplace(paramId, std::move(tn)).first->second;
}

// ---------------------------------------------------------------------------
// Detail — icon thumbnails
// ---------------------------------------------------------------------------

void DrawItemIcon(app::App &a, BrowserState &s, int id)
{
    if (!s.itemIcons.Contains(id) && s.iconFailed.find(id) == s.iconFailed.end())
    {
        std::vector<uint8_t> rgba;
        if (a.cache.RenderItemIcon(id, 128, 128, 4, rgba) && rgba.size() == 128u * 128u * 4u)
        {
            s.itemIcons.Put(id, rgba.data(), 128, 128);
        }
        else
        {
            s.iconFailed.insert(id);
        }
    }
    unsigned int tex = s.itemIcons.Get(id);
    const float box = ImGui::GetFontSize() * 6.0f;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + box, p0.y + box),
                                              theme::kPanelDeep, 4.0f);
    ImGui::GetWindowDrawList()->AddRect(p0, ImVec2(p0.x + box, p0.y + box),
                                        theme::kBorder, 4.0f);
    if (tex != 0)
    {
        ImGui::Image(ToTexId(tex), ImVec2(box, box));
    }
    else
    {
        ImGui::Dummy(ImVec2(box, box));
        ImVec2 c(p0.x + box * 0.5f, p0.y + box * 0.5f);
        const char *msg = "no icon";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::GetWindowDrawList()->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                                            theme::kTextDim, msg);
    }
}

void DrawSpritePreview(app::App &a, BrowserState &s, int id)
{
    int w = 0, h = 0;
    if (!s.spriteIcons.Contains(id) && s.spriteFailed.find(id) == s.spriteFailed.end())
    {
        std::vector<uint8_t> rgba;
        if (a.cache.GetSpriteFrame(id, 0, rgba, w, h) && w > 0 && h > 0
            && rgba.size() == static_cast<size_t>(w) * static_cast<size_t>(h) * 4u)
        {
            s.spriteIcons.Put(id, rgba.data(), w, h);
        }
        else
        {
            s.spriteFailed.insert(id);
        }
    }
    unsigned int tex = s.spriteIcons.Get(id);
    if (tex != 0)
    {
        const float box = ImGui::GetFontSize() * 6.0f;
        ImGui::Image(ToTexId(tex), ImVec2(box, box));
    }
    else
    {
        theme::Subheading("(no sprite frame)");
    }
}

// ---------------------------------------------------------------------------
// Detail — header + field rendering
// ---------------------------------------------------------------------------

void DrawDetailHeader(app::App &a, BrowserState &s)
{
    char title[48];
    std::snprintf(title, sizeof(title), "%s  #%d", TypeName(s), s.selectedId);
    ImGui::PushStyleColor(ImGuiCol_Text, Vec4(theme::kTextHi));
    ImGui::SetWindowFontScale(1.5f);
    ImGui::TextUnformatted(title);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();

    if (s.detailValid)
    {
        const cache::JsonValue *n = s.detail.Find("name");
        if (n && n->IsString() && !n->strVal.empty() && n->strVal != "null")
        {
            ImGui::PushStyleColor(ImGuiCol_Text, Vec4(theme::kAccent));
            ImGui::TextUnformatted(n->strVal.c_str());
            ImGui::PopStyleColor();
        }
    }
    if (kBrowseTypes[s.typeIdx].itemIcon)
    {
        ImGui::Spacing();
        DrawItemIcon(a, s, s.selectedId);
    }
    else if (kBrowseTypes[s.typeIdx].spriteIcon)
    {
        ImGui::Spacing();
        DrawSpritePreview(a, s, s.selectedId);
    }
}

// Render an array of scalars compactly: "[a, b, c]" — with element links when
// the field is a transform/quest list.
void DrawScalarArray(app::App &a, BrowserState &s, const char *field,
                     const cache::JsonValue &arr)
{
    const char *elemType = nullptr;
    if (std::strcmp(field, "transforms") == 0)
    {
        elemType = TypeName(s);
    }
    if (std::strcmp(field, "quests") == 0)
    {
        elemType = "quest";
    }

    ImGui::PushStyleColor(ImGuiCol_Text, Vec4(theme::kTextDim));
    ImGui::Text("%s [%zu]", field, arr.arrVal.size());
    ImGui::PopStyleColor();
    ImGui::Indent(ImGui::GetFontSize());
    for (const auto &e : arr.arrVal)
    {
        std::string v = e.AsString();
        int linkIdx = elemType ? FindTypeIdx(elemType) : -1;
        if (linkIdx >= 0 && e.IsNumber())
        {
            ImGui::SameLine();
            if (TextLink(v.c_str()))
            {
                Navigate(a, s, linkIdx, static_cast<int>(e.AsInt()));
            }
        }
        else
        {
            ImGui::SameLine();
            ImGui::TextUnformatted(v.c_str());
        }
    }
    ImGui::Unindent(ImGui::GetFontSize());
    ImGui::NewLine();
}

const char *IdFieldType(const char *field)
{
    for (const auto &f : kIdFields)
    {
        if (std::strcmp(f.field, field) == 0)
        {
            return f.type;
        }
    }
    return nullptr;
}

void DrawScalarField(app::App &a, BrowserState &s, const char *field,
                     const cache::JsonValue &v)
{
    std::string val = v.AsString();
    const char *linkType = (v.IsNumber() && v.AsInt() >= 0) ? IdFieldType(field) : nullptr;
    int linkIdx = linkType ? FindTypeIdx(linkType) : -1;
    if (linkIdx >= 0)
    {
        if (KeyLink(field, val.c_str()))
        {
            Navigate(a, s, linkIdx, static_cast<int>(v.AsInt()));
        }
    }
    else
    {
        theme::KeyLine(field, val.c_str());
    }
}

// ---------------------------------------------------------------------------
// Detail — params / enum entries (feature 5)
// ---------------------------------------------------------------------------

void DrawParams(app::App &a, BrowserState &s, const cache::JsonValue &params)
{
    if (!theme::BeginCard("cbrowse.params", "PARAMS"))
    {
        theme::EndCard();
        return;
    }
    if (params.objVal.empty())
    {
        theme::Subheading("(none)");
        theme::EndCard();
        return;
    }
    for (const auto &kv : params.objVal)
    {
        int paramId = std::atoi(kv.first.c_str());
        const std::string &tn = ParamTypeName(a, s, paramId);
        char left[80];
        if (tn.empty())
        {
            std::snprintf(left, sizeof(left), "%d", paramId);
        }
        else
        {
            std::snprintf(left, sizeof(left), "%d · %s", paramId, tn.c_str());
        }
        std::string val = kv.second.AsString();
        const char *lt = kv.second.IsNumber() ? ParamLinkType(tn) : nullptr;
        int linkIdx = lt ? FindTypeIdx(lt) : -1;
        if (linkIdx >= 0)
        {
            if (KeyLink(left, val.c_str()))
            {
                Navigate(a, s, linkIdx, static_cast<int>(kv.second.AsInt()));
            }
        }
        else
        {
            theme::KeyLine(left, val.c_str());
        }
    }
    theme::EndCard();
}

void DrawEnumEntries(const cache::JsonValue &detail)
{
    const cache::JsonValue *entries = detail.Find("entries");
    if (!entries || !entries->IsObject())
    {
        return;
    }
    if (!theme::BeginCard("cbrowse.enum", "ENTRIES"))
    {
        theme::EndCard();
        return;
    }
    char hdr[64];
    std::snprintf(hdr, sizeof(hdr), "%zu entries", entries->objVal.size());
    theme::Subheading(hdr);
    int shown = 0;
    for (const auto &kv : entries->objVal)
    {
        if (shown++ >= 512)
        {
            theme::Subheading("… (truncated)");
            break;
        }
        std::string val = kv.second.AsString();
        theme::KeyLine(kv.first.c_str(), val.c_str());
    }
    theme::EndCard();
}

// ---------------------------------------------------------------------------
// Detail — synthetic varp
// ---------------------------------------------------------------------------

void BuildVarpMap(app::App &a, BrowserState &s)
{
    s.varpMapBuilt = true;
    s.varpToVarbits.clear();
    std::vector<int> varbits = a.cache.ListTypeIds("varbit");
    for (int vb : varbits)
    {
        std::string json = a.cache.GetJson("varbit", vb);
        if (json.empty())
        {
            continue;
        }
        cache::JsonValue v;
        if (!cache::ParseJson(json, v))
        {
            continue;
        }
        const cache::JsonValue *vp = v.Find("varpId");
        if (!vp)
        {
            vp = v.Find("baseVar");
        }
        if (vp && vp->IsNumber())
        {
            s.varpToVarbits[static_cast<int>(vp->AsInt())].push_back(vb);
        }
    }
}

void DrawVarpDetail(app::App &a, BrowserState &s)
{
    if (!theme::BeginCard("cbrowse.varp", "VARP (SYNTHETIC)"))
    {
        theme::EndCard();
        return;
    }
    char hdr[48];
    std::snprintf(hdr, sizeof(hdr), "varp #%d", s.selectedId);
    ImGui::PushStyleColor(ImGuiCol_Text, Vec4(theme::kTextHi));
    ImGui::SetWindowFontScale(1.5f);
    ImGui::TextUnformatted(hdr);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();

    if (!s.varpMapBuilt)
    {
        if (ImGui::Button("Build varbit map"))
        {
            BuildVarpMap(a, s);
        }
        theme::Subheading("Scans every varbit once to map varp -> varbits.");
        theme::EndCard();
        return;
    }
    auto it = s.varpToVarbits.find(s.selectedId);
    if (it == s.varpToVarbits.end() || it->second.empty())
    {
        theme::Subheading("No varbits reference this varp.");
        theme::EndCard();
        return;
    }
    theme::Subheading("varbits on this varp:");
    int varbitIdx = FindTypeIdx("varbit");
    for (int vb : it->second)
    {
        char b[16];
        std::snprintf(b, sizeof(b), "%d", vb);
        if (TextLink(b) && varbitIdx >= 0)
        {
            Navigate(a, s, varbitIdx, vb);
        }
    }
    theme::EndCard();
}

// ---------------------------------------------------------------------------
// Detail — top-level field dump + raw JSON
// ---------------------------------------------------------------------------

void DrawFieldDump(app::App &a, BrowserState &s)
{
    if (!theme::BeginCard("cbrowse.fields", "FIELDS"))
    {
        theme::EndCard();
        return;
    }
    const cache::JsonValue *params = nullptr;
    for (const auto &kv : s.detail.objVal)
    {
        const std::string &field = kv.first;
        const cache::JsonValue &v = kv.second;
        if (field == "params" && v.IsObject())
        {
            params = &v;
            continue;
        }
        if (field == "entries")
        {
            continue;   // rendered in its own card
        }
        if (v.IsObject())
        {
            theme::KeyLine(field.c_str(), "{...}");
        }
        else if (v.IsArray())
        {
            DrawScalarArray(a, s, field.c_str(), v);
        }
        else
        {
            DrawScalarField(a, s, field.c_str(), v);
        }
    }
    theme::EndCard();

    if (params)
    {
        ImGui::Spacing();
        DrawParams(a, s, *params);
    }
    ImGui::Spacing();
    DrawEnumEntries(s.detail);
}

void DrawRawJson(BrowserState &s)
{
    ImGui::Spacing();
    if (!ImGui::CollapsingHeader("Raw JSON"))
    {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Vec4(theme::kPanelDeep));
    ImGui::InputTextMultiline("##cbjson",
                              const_cast<char *>(s.detailJson.c_str()),
                              s.detailJson.size() + 1,
                              ImVec2(-FLT_MIN, ImGui::GetFontSize() * 14),
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Detail — CS2 cross-reference (Phase 6b)
// ---------------------------------------------------------------------------

void Utf8FromWide(const std::wstring &w, char *out, int cap)
{
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out, cap, nullptr, nullptr);
}

void DrawXrefList(app::App &a, const std::vector<int> *scripts, int highlightId)
{
    if (!scripts || scripts->empty())
    {
        theme::Subheading("No CS2 scripts reference this id.");
        return;
    }
    char hdr[48];
    std::snprintf(hdr, sizeof(hdr), "%zu script%s", scripts->size(),
                  scripts->size() == 1 ? "" : "s");
    theme::Subheading(hdr);
    ImGui::BeginChild("##cbxref", ImVec2(0, ImGui::GetFontSize() * 10), 0);
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(scripts->size()));
    while (clip.Step())
    {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
        {
            int sid = (*scripts)[static_cast<size_t>(i)];
            char b[24];
            std::snprintf(b, sizeof(b), "script %06d", sid);
            ImGui::PushID(i);
            if (TextLink(b))
            {
                a.cs2RequestedScript    = sid;
                a.cs2RequestedHighlight = highlightId;
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void DrawCs2XrefIdle(app::App &a, bool isError)
{
    static char dir[512] = {};
    if (dir[0] == 0)
    {
        Utf8FromWide(a.cs2.Dir(), dir, sizeof(dir));
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.72f);
    ImGui::InputText("##cbxrefdir", dir, sizeof(dir));
    ImGui::SameLine();
    if (ImGui::Button("Build index"))
    {
        wchar_t w[512];
        MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, 512);
        a.cs2.SetDir(w);
        a.cs2.BuildAsync(false);   // reuse the on-disk index when the corpus is unchanged
    }
    if (isError)
    {
        char e[600];
        Utf8FromWide(a.cs2.LastError(), e, sizeof(e));
        ImGui::PushStyleColor(ImGuiCol_Text, Vec4(theme::kBad));
        ImGui::TextWrapped("%s", e);
        ImGui::PopStyleColor();
    }
    else
    {
        theme::Subheading("Index the cs2_asm corpus to find the scripts that use this id.");
    }
}

void DrawCs2Xref(app::App &a, BrowserState &s)
{
    if (!theme::BeginCard("cbrowse.xref", "CS2 XREF"))
    {
        theme::EndCard();
        return;
    }
    using St = cs2::Cs2Index::Status;
    St st = a.cs2.GetStatus();
    if (st == St::Building)
    {
        char cap[48];
        size_t done = a.cs2.FilesDone();
        size_t total = a.cs2.FilesTotal();
        std::snprintf(cap, sizeof(cap), "%zu / %zu", done, total);
        theme::ProgressStrip(total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f,
                             theme::kAccent, cap);
        theme::EndCard();
        return;
    }
    if (st != St::Ready)
    {
        DrawCs2XrefIdle(a, st == St::Error);
        theme::EndCard();
        return;
    }

    const char *typeName = TypeName(s);
    bool isVarbit = std::strcmp(typeName, "varbit") == 0;
    bool isVarp   = std::strcmp(typeName, "varp") == 0;
    bool exact    = isVarbit || isVarp;
    const std::vector<int> *scripts = isVarbit ? a.cs2.Varbit(s.selectedId)
                                    : isVarp   ? a.cs2.Varp(s.selectedId)
                                               : a.cs2.Const(s.selectedId);
    theme::Pill(exact ? "exact" : "constant union",
                exact ? theme::kGoodSoft : theme::kAccentSoft,
                exact ? theme::kGood : theme::kAccent);
    if (!exact)
    {
        ImGui::SameLine();
        theme::Subheading("may match other types sharing this id");
    }
    DrawXrefList(a, scripts, s.selectedId);
    theme::EndCard();
}

void DrawDetailToolbar(app::App &a, BrowserState &s)
{
    bool canBack = s.histPos > 0;
    bool canFwd  = s.histPos + 1 < static_cast<int>(s.history.size());
    if (!canBack)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::ArrowButton("##cbback", ImGuiDir_Left))
    {
        GoHistory(a, s, -1);
    }
    if (!canBack)
    {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (!canFwd)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::ArrowButton("##cbfwd", ImGuiDir_Right))
    {
        GoHistory(a, s, +1);
    }
    if (!canFwd)
    {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy id"))
    {
        char b[16];
        std::snprintf(b, sizeof(b), "%d", s.selectedId);
        ImGui::SetClipboardText(b);
    }
}

void DrawRightPane(app::App &a, BrowserState &s)
{
    if (!theme::BeginCard("cbrowse.detail", "DETAIL"))
    {
        theme::EndCard();
        return;
    }
    if (s.selectedId < 0)
    {
        theme::Subheading("Pick an entry on the left to decode it.");
        theme::EndCard();
        return;
    }
    DrawDetailToolbar(a, s);
    theme::AccentRule();

    if (kBrowseTypes[s.typeIdx].synthetic)
    {
        DrawVarpDetail(a, s);
        ImGui::Spacing();
        DrawCs2Xref(a, s);
        theme::EndCard();
        return;
    }
    if (!s.detailValid)
    {
        theme::Subheading("(not found in this cache)");
        ImGui::Spacing();
        DrawCs2Xref(a, s);
        theme::EndCard();
        return;
    }
    DrawDetailHeader(a, s);
    theme::AccentRule();
    DrawFieldDump(a, s);
    ImGui::Spacing();
    DrawCs2Xref(a, s);
    DrawRawJson(s);
    theme::EndCard();
}

}

void DrawCacheBrowser(app::App &a)
{
    BrowserState &s = State();

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 78, ImGui::GetFontSize() * 42),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Cache Browser"))
    {
        ImGui::End();
        return;
    }

    DrawCacheOpen(a);

    if (!a.cache.IsOpen())
    {
        ImGui::Spacing();
        theme::Subheading("Open the local cache above to browse config types,\n"
                          "render item icons, and cross-reference ids.");
        ImGui::End();
        return;
    }

    if (s.listedType != s.typeIdx)
    {
        ReloadIds(a, s);
    }
    PumpNameIndex(a, s);

    ImGui::Spacing();
    const float fs   = ImGui::GetFontSize();
    const float colL = fs * 22.0f;

    ImGui::BeginChild("##cbleft", ImVec2(colL, 0), 0);
    DrawLeftPane(a, s);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##cbright", ImVec2(0, 0), 0);
    DrawRightPane(a, s);
    ImGui::EndChild();

    ImGui::End();
}

}
