#include "Panels.h"

#include "app/App.h"
#include "app/Theme.h"
#include "overlay/LayerSync.h"
#include "overlay/OverlayWindow.h"
#include "rpc/DrawParams.h"

#include "imgui.h"
#include "imgui_internal.h"   // ImGuiSettingsHandler

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ===========================================================================
// Layers panel -- one toggle per thing to draw IN the game.
//
// World layers (npcs, players, self, scene objects, ground items) are pushed
// to the agent's own overlay by overlay/LayerSync, which owns the timer, the
// diff and the RPCs; this panel only edits LayerConfig and shows what the sync
// reports. The Interfaces layer is the existing transparent OverlayWindow
// (App::overlayEnabled), surfaced here so every layer has one home.
//
// Settings persist in imgui.ini under [NXTDebugger][Layers] -- the one
// settings file the app already has (and which --hidden neither reads nor
// writes, so a headless instance starts from defaults).
// ===========================================================================

namespace nxtdbg::panels
{

namespace
{

using overlay::LayerConfig;
using overlay::LayerId;
using overlay::LayerRuntime;
using overlay::LayerSettings;

constexpr const char *kIniType  = "NXTDebugger";
constexpr const char *kIniEntry = "Layers";

// Paint order of the cards: what a debugging user reaches for first.
constexpr LayerId kCardOrder[overlay::kLayerCount] = {
    LayerId::Npc, LayerId::Player, LayerId::Self, LayerId::Loc, LayerId::Ground,
    LayerId::Projectile, LayerId::SpotAnim,
};

void Changed(app::App &a)
{
    overlay::NotifyLayerConfigChanged(a.layers);
    ImGui::MarkIniSettingsDirty();
}

ImU32 ImColFromArgb(uint32_t argb)
{
    return IM_COL32((argb >> 16) & 0xFFu, (argb >> 8) & 0xFFu,
                    argb & 0xFFu, (argb >> 24) & 0xFFu);
}

ImU32 Soft(ImU32 c)
{
    return (c & 0x00FFFFFFu) | (50u << 24);
}

// --- ini persistence -----------------------------------------------------------

void *IniReadOpen(ImGuiContext *, ImGuiSettingsHandler *h, const char *name)
{
    return std::strcmp(name, kIniEntry) == 0 ? h->UserData : nullptr;
}

bool ReadLayerLine(LayerSettings &s, const char *field, const char *value)
{
    if (std::strcmp(field, "enabled") == 0)      { s.isEnabled = std::atoi(value) != 0; }
    else if (std::strcmp(field, "color") == 0)   { s.colorArgb = std::strtoul(value, nullptr, 16); }
    else if (std::strcmp(field, "labels") == 0)  { overlay::ParseLabelFields(value, s.labels); }
    else if (std::strcmp(field, "coords") == 0)   // pre-fields ini: its own line
    {
        overlay::SetLabelField(s.labels, overlay::LabelField::Coords, std::atoi(value) != 0);
    }
    else if (std::strcmp(field, "animating_only") == 0) { s.isAnimatingOnly = std::atoi(value) != 0; }
    else if (std::strcmp(field, "distance") == 0) { s.maxDistance = std::atoi(value); }
    else if (std::strcmp(field, "count") == 0)   { s.maxCount = std::atoi(value); }
    else if (std::strcmp(field, "filter") == 0)
    {
        std::snprintf(s.filter, sizeof(s.filter), "%s", value);
    }
    else
    {
        return false;
    }
    return true;
}

// Lines are `refresh_ms=600`, `interfaces=1`, `<layer>.<field>=<value>`.
void IniReadLine(ImGuiContext *, ImGuiSettingsHandler *, void *entry, const char *line)
{
    app::App   &a  = *static_cast<app::App *>(entry);
    const char *eq = std::strchr(line, '=');
    if (eq == nullptr)
    {
        return;
    }
    const std::string key(line, static_cast<size_t>(eq - line));
    const char       *value = eq + 1;
    if (key == "refresh_ms")             { a.layers.config.refreshMs = std::atoi(value); return; }
    if (key == "fast_refresh_ms")        { a.layers.config.fastRefreshMs = std::atoi(value); return; }
    if (key == "label_budget")           { a.layers.config.labelBudget = std::atoi(value); return; }
    if (key == "label_chars")            { a.layers.config.tileLabelChars = std::atoi(value); return; }
    if (key == "label_rows")             { a.layers.config.isMultiRowLabels = std::atoi(value) != 0; return; }
    if (key == "interfaces")             { a.overlayEnabled = std::atoi(value) != 0; return; }
    if (key == "interfaces_all_labels")  { a.overlayShowAllLabels = std::atoi(value) != 0; return; }
    const size_t dot = key.find('.');
    LayerId      id  = LayerId::Npc;
    if (dot != std::string::npos && overlay::ParseLayerToken(key.substr(0, dot), id))
    {
        ReadLayerLine(a.layers.config.layers[static_cast<int>(id)], key.c_str() + dot + 1, value);
    }
}

void IniApplyAll(ImGuiContext *, ImGuiSettingsHandler *h)
{
    overlay::NotifyLayerConfigChanged(static_cast<app::App *>(h->UserData)->layers);
}

void IniWriteAll(ImGuiContext *, ImGuiSettingsHandler *h, ImGuiTextBuffer *out)
{
    const app::App &a = *static_cast<const app::App *>(h->UserData);
    out->appendf("[%s][%s]\n", kIniType, kIniEntry);
    out->appendf("refresh_ms=%d\n", a.layers.config.refreshMs);
    out->appendf("fast_refresh_ms=%d\n", a.layers.config.fastRefreshMs);
    out->appendf("label_budget=%d\n", a.layers.config.labelBudget);
    out->appendf("label_chars=%d\n", a.layers.config.tileLabelChars);
    out->appendf("label_rows=%d\n", a.layers.config.isMultiRowLabels ? 1 : 0);
    out->appendf("interfaces=%d\n", a.overlayEnabled ? 1 : 0);
    out->appendf("interfaces_all_labels=%d\n", a.overlayShowAllLabels ? 1 : 0);
    for (int i = 0; i < overlay::kLayerCount; ++i)
    {
        const LayerSettings &s   = a.layers.config.layers[i];
        const char          *tok = overlay::LayerToken(static_cast<LayerId>(i));
        out->appendf("%s.enabled=%d\n", tok, s.isEnabled ? 1 : 0);
        out->appendf("%s.color=%08X\n", tok, s.colorArgb);
        out->appendf("%s.labels=%s\n", tok, overlay::LabelFieldsToken(s.labels).c_str());
        out->appendf("%s.animating_only=%d\n", tok, s.isAnimatingOnly ? 1 : 0);
        out->appendf("%s.distance=%d\n", tok, s.maxDistance);
        out->appendf("%s.count=%d\n", tok, s.maxCount);
        out->appendf("%s.filter=%s\n", tok, s.filter);
    }
    out->append("\n");
}

// --- cards -------------------------------------------------------------------

void DrawStatusPill(const app::App &a, const LayerSettings &s, const LayerRuntime &rt)
{
    char buf[64];
    if (!s.isEnabled)
    {
        theme::Pill("off", theme::kPanelDeep, theme::kTextDim);
        return;
    }
    if (rt.isRejected)
    {
        theme::Pill("refused by agent", Soft(theme::kBad), theme::kBad);
        return;
    }
    if (!a.rpc.IsConnected())
    {
        theme::Pill("waiting for agent", Soft(theme::kInfo), theme::kInfo);
        return;
    }
    if (rt.matched > rt.shown)
    {
        std::snprintf(buf, sizeof(buf), "showing %u of %u", rt.shown, rt.matched);
        theme::Pill(buf, Soft(theme::kWarn), theme::kWarn);
        return;
    }
    std::snprintf(buf, sizeof(buf), "%u drawn", rt.shown);
    theme::Pill(buf, Soft(theme::kGood), theme::kGood);
}

void DrawLayerHeader(app::App &a, LayerId id, LayerSettings &s, const LayerRuntime &rt)
{
    ImGui::PushID(static_cast<int>(id));
    const float pillX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x * 0.55f;
    if (theme::Toggle("##on", &s.isEnabled))
    {
        Changed(a);
    }
    ImGui::SameLine();
    // Colour swatch doubles as the colour editor: the box on screen is this hue.
    float rgba[4];
    const ImVec4 col = ImGui::ColorConvertU32ToFloat4(ImColFromArgb(s.colorArgb));
    rgba[0] = col.x; rgba[1] = col.y; rgba[2] = col.z; rgba[3] = col.w;
    if (ImGui::ColorEdit4("##color", rgba, ImGuiColorEditFlags_NoInputs
                                         | ImGuiColorEditFlags_AlphaBar))
    {
        s.colorArgb = rpc::ArgbFromFloats(rgba);
        Changed(a);
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, s.isEnabled ? theme::kTextHi : theme::kText);
    ImGui::TextUnformatted(overlay::LayerTitle(id));
    ImGui::PopStyleColor();
    ImGui::SameLine(pillX);
    DrawStatusPill(a, s, rt);
    ImGui::PopID();
}

void DrawLayerMessage(app::App &a, LayerId id, const LayerRuntime &rt)
{
    if (rt.message.empty())
    {
        return;
    }
    const ImU32 c = rt.isWarning ? theme::kWarn : theme::kBad;
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    ImGui::TextWrapped("%s %s", rt.isWarning ? "(!)" : "(x)", rt.message.c_str());
    ImGui::PopStyleColor();
    if (rt.isRejected)
    {
        ImGui::TextDisabled("This agent build cannot draw this layer. Not retrying.");
        ImGui::SameLine();
        ImGui::PushID(static_cast<int>(id));
        if (ImGui::SmallButton("Retry"))
        {
            overlay::RetryLayer(a.layers, id);
        }
        ImGui::PopID();
    }
}

// One checkbox per label field that means something on this layer, flowing
// onto a new line when the next one would not fit.
void DrawLabelFields(app::App &a, LayerId id, LayerSettings &s)
{
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextDim);
    ImGui::TextUnformatted("label");
    ImGui::PopStyleColor();
    const ImGuiStyle &style = ImGui::GetStyle();
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    bool isFirst = true;
    for (int i = 0; i < overlay::kLabelFieldCount; ++i)
    {
        const auto field = static_cast<overlay::LabelField>(i);
        if (!overlay::LabelFieldApplies(id, field))
        {
            continue;
        }
        const char *title = overlay::LabelFieldTitle(field);
        const float width = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x
                          + ImGui::CalcTextSize(title).x;
        ImGui::SameLine(0.0f, isFirst ? style.ItemSpacing.x : style.ItemSpacing.x * 2.0f);
        if (!isFirst && ImGui::GetCursorScreenPos().x + width > right)
        {
            ImGui::NewLine();
        }
        isFirst = false;
        bool isOn = overlay::GetLabelField(s.labels, field);
        if (ImGui::Checkbox(title, &isOn))
        {
            overlay::SetLabelField(s.labels, field, isOn);
            Changed(a);
        }
    }
}

// Layer-specific options under the label fields.
void DrawLayerExtras(app::App &a, LayerId id, LayerSettings &s)
{
    if (id == LayerId::Npc || id == LayerId::Player)
    {
        if (ImGui::Checkbox("only while animating", &s.isAnimatingOnly))
        {
            Changed(a);
        }
    }
    if (id == LayerId::Projectile)
    {
        ImGui::TextDisabled("Box where it lands, a line from where it was thrown. Refreshes "
                            "every %d ms.", a.layers.config.fastRefreshMs);
    }
    if (id == LayerId::SpotAnim)
    {
        ImGui::TextDisabled("Graphics on a tile, from the event feed: each shows for %d s "
                            "after it starts. Graphics on an npc or player are the "
                            "'spot anim' label field.",
                            static_cast<int>(overlay::kSpotAnimHoldMs / 1000));
    }
}

void DrawLayerOptions(app::App &a, LayerId id, LayerSettings &s)
{
    ImGui::PushID(static_cast<int>(id) + 100);
    ImGui::Indent(ImGui::GetFontSize() * 2.6f);
    DrawLabelFields(a, id, s);
    DrawLayerExtras(a, id, s);
    if (id != LayerId::Self)
    {
        const float w = ImGui::GetFontSize() * 11.0f;
        const int   maxCount = overlay::IsFastLayer(id) ? overlay::kMaxFastLayerCount
                                                        : overlay::kMaxLayerCount;
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderInt("max distance (tiles)", &s.maxDistance, 0,
                             overlay::kMaxLayerDistance))
        {
            Changed(a);
        }
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderInt("max count", &s.maxCount, 1, maxCount))
        {
            Changed(a);
        }
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputTextWithHint("filter", "name or exact id", s.filter, sizeof(s.filter)))
        {
            Changed(a);
        }
    }
    ImGui::Unindent(ImGui::GetFontSize() * 2.6f);
    ImGui::PopID();
}

void DrawLayerRow(app::App &a, LayerId id)
{
    LayerSettings      &s  = a.layers.config.layers[static_cast<int>(id)];
    const LayerRuntime &rt = a.layers.runtime[static_cast<int>(id)];
    DrawLayerHeader(a, id, s, rt);
    DrawLayerMessage(a, id, rt);
    char hdr[64];
    std::snprintf(hdr, sizeof(hdr), "options##%s", overlay::LayerToken(id));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextDim);
    const bool isOpen = ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_NoTreePushOnOpen
                                             | ImGuiTreeNodeFlags_SpanAvailWidth);
    ImGui::PopStyleColor();
    if (isOpen)
    {
        DrawLayerOptions(a, id, s);
    }
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.35f));
}

void DrawWorldCard(app::App &a)
{
    if (!theme::BeginCard("ly.world", "WORLD LAYERS  (drawn by the agent)"))
    {
        theme::EndCard();
        return;
    }
    for (LayerId id : kCardOrder)
    {
        DrawLayerRow(a, id);
    }
    theme::EndCard();
}

void DrawInterfacesCard(app::App &a)
{
    if (!theme::BeginCard("ly.iface", "INTERFACES  (overlay window)", theme::kInfo))
    {
        theme::EndCard();
        return;
    }
    if (theme::Toggle("##ifon", &a.overlayEnabled))
    {
        ImGui::MarkIniSettingsDirty();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Component boxes");
    ImGui::TextDisabled("Paints the Interfaces panel's selected interface over the game.");
    if (a.overlayEnabled && !overlay::HasVisibleBoxes(a.interfaceView))
    {
        // The overlay window draws no text of its own; it stays hidden instead.
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kWarn), "%s",
                           a.interfaceView.iface < 0
                               ? "Nothing to draw: select an interface in the Interfaces panel."
                               : "Nothing to draw: the interface has no visible components.");
    }
    if (ImGui::Checkbox("Label every component", &a.overlayShowAllLabels))
    {
        ImGui::MarkIniSettingsDirty();
    }
    ImGui::Checkbox("Only while the game is focused", &a.overlayFollowFg);
    theme::EndCard();
}

void DrawSummaryStats(app::App &a)
{
    const overlay::LayerSyncState &st = a.layers;
    char sent[32];
    char rpcs[32];
    std::snprintf(sent, sizeof(sent), "%u", overlay::TotalSentDraws(st));
    std::snprintf(rpcs, sizeof(rpcs), "%u / %u", st.rpcsLastRefresh, st.rpcsLastFastRefresh);
    theme::HeroStat("draws in the agent's store", sent);
    ImGui::SameLine(0, ImGui::GetFontSize() * 2.5f);
    theme::HeroStat("rpcs last refresh (main / fast)", rpcs, theme::kText);
}

void DrawTimingControls(app::App &a, LayerConfig &cfg)
{
    const float w = ImGui::GetFontSize() * 11.0f;
    ImGui::SetNextItemWidth(w);
    if (ImGui::SliderInt("refresh (ms)", &cfg.refreshMs, overlay::kMinRefreshMs, 3000))
    {
        Changed(a);
    }
    ImGui::SetNextItemWidth(w);
    if (ImGui::SliderInt("fast refresh (ms)", &cfg.fastRefreshMs, overlay::kMinFastRefreshMs,
                         1000))
    {
        Changed(a);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("projectiles, spot anims");
}

void DrawLabelControls(app::App &a, LayerConfig &cfg)
{
    const float w = ImGui::GetFontSize() * 11.0f;
    ImGui::SetNextItemWidth(w);
    if (ImGui::SliderInt("label budget", &cfg.labelBudget, 0, overlay::kMaxLabelBudget))
    {
        Changed(a);
    }
    const overlay::LabelTally &main = a.layers.labels;
    const overlay::LabelTally &fast = a.layers.fastLabels;
    const uint32_t given  = main.given + fast.given;
    const uint32_t wanted = main.wanted + fast.wanted;
    const ImU32    tone   = given < wanted ? theme::kWarn : theme::kInfo;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "labels %u of %u", given, wanted);
    ImGui::SameLine();
    theme::Pill(buf, Soft(tone), tone);
    if (ImGui::Checkbox("multi-row labels", &cfg.isMultiRowLabels))
    {
        Changed(a);
    }
    ImGui::SameLine();
    ImGui::TextDisabled(cfg.isMultiRowLabels ? "one row per thing, wrapped"
                                             : "one line (agents without row support)");
    ImGui::SetNextItemWidth(w);
    const char *lengthLabel = cfg.isMultiRowLabels ? "row width (chars)###labelChars"
                                                 : "label length (chars)###labelChars";
    if (ImGui::SliderInt(lengthLabel, &cfg.tileLabelChars,
                         overlay::kMinTileLabelChars, overlay::kMaxTileLabelChars))
    {
        Changed(a);
    }
    if (given < wanted)
    {
        ImGui::TextDisabled("Nearest get labels first; projectiles and spot anims get what "
                            "the other layers leave. The agent has 64 text slots shared "
                            "with the host and scripts; boxes need none.");
    }
}

// How this session's agent takes entity highlights: one batch per layer, or
// (an agent DLL without entity batch items) one highlight_entity per entity.
void DrawAgentMode(const app::App &a)
{
    if (!a.rpc.IsConnected())
    {
        return;
    }
    const overlay::AgentEntityMode mode = a.layers.agentMode;
    const char *title = overlay::AgentModeTitle(mode);
    switch (mode)
    {
    case overlay::AgentEntityMode::Unknown:
        theme::Pill(title, theme::kPanelDeep, theme::kTextDim);
        break;
    case overlay::AgentEntityMode::Batch:
        theme::Pill(title, Soft(theme::kGood), theme::kGood);
        break;
    case overlay::AgentEntityMode::PerEntity:
        theme::Pill(title, Soft(theme::kWarn), theme::kWarn);
        ImGui::TextDisabled("Older agent DLL: one call per entity, up to %u per refresh. "
                            "Locs and ground items need the newer DLL.",
                            overlay::kMaxEntityCallsPerRefresh);
        break;
    }
}

void DrawSummaryCard(app::App &a)
{
    if (!theme::BeginCard("ly.sum", "LAYERS"))
    {
        theme::EndCard();
        return;
    }
    LayerConfig &cfg = a.layers.config;
    const bool   isLive = a.rpc.IsConnected() && overlay::AnyWorldLayerOn(cfg);
    theme::StatusDot(isLive ? theme::kGood : theme::kTextDim, isLive, a.dotPhase);
    ImGui::SameLine();
    ImGui::TextUnformatted(!a.session.IsOpen() ? "Attach to a client to draw layers."
                         : !a.rpc.IsConnected() ? "Waiting for the agent's RPC pipe."
                         : isLive ? "Drawing in the game." : "Turn a layer on to draw it in the game.");
    DrawAgentMode(a);
    ImGui::Spacing();
    DrawSummaryStats(a);
    ImGui::Spacing();
    DrawTimingControls(a, cfg);
    DrawLabelControls(a, cfg);
    theme::EndCard();
}

}   // namespace

void RegisterLayerSettings(app::App &a)
{
    ImGuiSettingsHandler h;
    h.TypeName   = kIniType;
    h.TypeHash   = ImHashStr(kIniType);
    h.ReadOpenFn = IniReadOpen;
    h.ReadLineFn = IniReadLine;
    h.ApplyAllFn = IniApplyAll;
    h.WriteAllFn = IniWriteAll;
    h.UserData   = &a;
    ImGui::AddSettingsHandler(&h);
}

void DrawLayersPanel(app::App &a)
{
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34, ImGui::GetFontSize() * 40),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Layers"))
    {
        ImGui::End();
        return;
    }
    DrawSummaryCard(a);
    ImGui::Spacing();
    DrawWorldCard(a);
    ImGui::Spacing();
    DrawInterfacesCard(a);
    ImGui::End();
}

}
