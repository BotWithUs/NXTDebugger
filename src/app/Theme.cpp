#include "Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>

namespace nxtdbg::theme
{

namespace
{

ImVec4 Vec4(ImU32 c)
{
    return ImGui::ColorConvertU32ToFloat4(c);
}

float StripeWidth()
{
    return std::max(3.0f, ImGui::GetFontSize() * 0.18f);
}

}

void Apply()
{
    ImGuiStyle &s = ImGui::GetStyle();

    s.WindowRounding      = 6.0f;
    s.ChildRounding       = 6.0f;
    s.FrameRounding       = 4.0f;
    s.GrabRounding        = 4.0f;
    s.PopupRounding       = 6.0f;
    s.ScrollbarRounding   = 4.0f;
    s.TabRounding         = 4.0f;

    s.WindowBorderSize    = 1.0f;
    s.ChildBorderSize     = 1.0f;
    s.FrameBorderSize     = 0.0f;
    s.PopupBorderSize     = 1.0f;

    s.WindowPadding       = ImVec2(12, 10);
    s.FramePadding        = ImVec2(8,  5);
    s.CellPadding         = ImVec2(8,  4);
    s.ItemSpacing         = ImVec2(8,  6);
    s.ItemInnerSpacing    = ImVec2(5,  4);
    s.IndentSpacing       = 18.0f;
    s.ScrollbarSize       = 12.0f;
    s.GrabMinSize         = 10.0f;
    s.SeparatorTextBorderSize = 1.0f;

    ImVec4 *c = s.Colors;
    c[ImGuiCol_Text]                = Vec4(kText);
    c[ImGuiCol_TextDisabled]        = Vec4(kTextDim);
    c[ImGuiCol_WindowBg]            = Vec4(kBg);
    c[ImGuiCol_ChildBg]             = Vec4(kPanel);
    c[ImGuiCol_PopupBg]             = Vec4(kPanel);
    c[ImGuiCol_Border]              = Vec4(kBorder);
    c[ImGuiCol_BorderShadow]        = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]             = Vec4(kPanelDeep);
    c[ImGuiCol_FrameBgHovered]      = Vec4(IM_COL32(0x1A, 0x1E, 0x28, 255));
    c[ImGuiCol_FrameBgActive]       = Vec4(IM_COL32(0x22, 0x28, 0x34, 255));
    c[ImGuiCol_TitleBg]             = Vec4(kPanelDeep);
    c[ImGuiCol_TitleBgActive]       = Vec4(IM_COL32(0x18, 0x1C, 0x24, 255));
    c[ImGuiCol_TitleBgCollapsed]    = Vec4(kPanelDeep);
    c[ImGuiCol_MenuBarBg]           = Vec4(IM_COL32(0x0C, 0x0E, 0x12, 255));
    c[ImGuiCol_ScrollbarBg]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]       = Vec4(kBorderHi);
    c[ImGuiCol_ScrollbarGrabHovered]= Vec4(IM_COL32(0x48, 0x52, 0x66, 255));
    c[ImGuiCol_ScrollbarGrabActive] = Vec4(kAccent);
    c[ImGuiCol_CheckMark]           = Vec4(kAccent);
    c[ImGuiCol_SliderGrab]          = Vec4(kAccent);
    c[ImGuiCol_SliderGrabActive]    = Vec4(IM_COL32(0xFF, 0xC2, 0x66, 255));
    c[ImGuiCol_Button]              = Vec4(IM_COL32(0x1C, 0x21, 0x2C, 255));
    c[ImGuiCol_ButtonHovered]       = Vec4(IM_COL32(0x2A, 0x32, 0x42, 255));
    c[ImGuiCol_ButtonActive]        = Vec4(kAccentSoft);
    c[ImGuiCol_Header]              = Vec4(IM_COL32(0x18, 0x1D, 0x26, 255));
    c[ImGuiCol_HeaderHovered]       = Vec4(IM_COL32(0x22, 0x28, 0x34, 255));
    c[ImGuiCol_HeaderActive]        = Vec4(kAccentSoft);
    c[ImGuiCol_Separator]           = Vec4(kBorder);
    c[ImGuiCol_SeparatorHovered]    = Vec4(kBorderHi);
    c[ImGuiCol_SeparatorActive]     = Vec4(kAccent);
    c[ImGuiCol_Tab]                 = Vec4(kPanelDeep);
    c[ImGuiCol_TabHovered]          = Vec4(IM_COL32(0x22, 0x28, 0x34, 255));
    c[ImGuiCol_TabActive]           = Vec4(kPanel);
    c[ImGuiCol_TabUnfocused]        = Vec4(kPanelDeep);
    c[ImGuiCol_TabUnfocusedActive]  = Vec4(kPanel);
    c[ImGuiCol_TableHeaderBg]       = Vec4(kPanelDeep);
    c[ImGuiCol_TableBorderStrong]   = Vec4(kBorder);
    c[ImGuiCol_TableBorderLight]    = Vec4(IM_COL32(0x18, 0x1B, 0x24, 255));
    c[ImGuiCol_TableRowBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]       = Vec4(kRowAlt);
    c[ImGuiCol_TextSelectedBg]      = Vec4(kAccentSoft);
    c[ImGuiCol_NavHighlight]        = Vec4(kAccent);
}

bool BeginCard(const char *id, const char *title, ImU32 stripeCol, bool fillY)
{
    const float fs       = ImGui::GetFontSize();
    const float stripeW  = StripeWidth();
    const float headerH  = fs * 1.55f;

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Vec4(kPanel));
    ImGui::PushStyleColor(ImGuiCol_Border,  Vec4(kBorder));

    // fillY: stretch the card to the parent's remaining height so embedded
    // scroll lists get real vertical space. Otherwise auto-fit to content.
    ImGuiChildFlags childFlags = ImGuiChildFlags_Border;
    if (!fillY) childFlags |= ImGuiChildFlags_AutoResizeY;
    bool visible = ImGui::BeginChild(id, ImVec2(0, 0), childFlags);

    if (visible)
    {
        ImDrawList *dl  = ImGui::GetWindowDrawList();
        ImVec2      p0  = ImGui::GetCursorScreenPos();
        ImVec2      avail = ImGui::GetContentRegionAvail();

        dl->AddRectFilled(
            ImVec2(p0.x - 4.0f, p0.y - 2.0f),
            ImVec2(p0.x + stripeW, p0.y + headerH),
            stripeCol, 1.5f);

        ImGui::SetCursorScreenPos(ImVec2(p0.x + stripeW + fs * 0.55f, p0.y + fs * 0.18f));
        ImGui::PushStyleColor(ImGuiCol_Text, Vec4(kTextHi));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();

        ImVec2 ruleStart(p0.x + stripeW + fs * 0.55f, p0.y + headerH - 1.0f);
        ImVec2 ruleEnd  (p0.x + avail.x,              p0.y + headerH - 1.0f);
        dl->AddLine(ruleStart, ruleEnd, kBorder, 1.0f);

        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + headerH + fs * 0.3f));
    }
    return visible;
}

void EndCard()
{
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

void Pill(const char *text, ImU32 fillCol, ImU32 textCol)
{
    const float fs      = ImGui::GetFontSize();
    const ImVec2 padCfg = ImVec2(fs * 0.55f, fs * 0.18f);
    const ImVec2 tsz    = ImGui::CalcTextSize(text);
    const ImVec2 p0     = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + tsz.x + padCfg.x * 2.0f,
                    p0.y + tsz.y + padCfg.y * 2.0f);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, fillCol, (p1.y - p0.y) * 0.5f);
    dl->AddText(ImVec2(p0.x + padCfg.x, p0.y + padCfg.y), textCol, text);

    ImGui::Dummy(ImVec2(p1.x - p0.x, p1.y - p0.y));
}

void StatusDot(ImU32 col, bool alive, float phase)
{
    const float fs    = ImGui::GetFontSize();
    const float r     = fs * 0.34f;
    const ImVec2 p0   = ImGui::GetCursorScreenPos();
    const ImVec2 c    = ImVec2(p0.x + r + 2.0f, p0.y + fs * 0.55f);

    float pulse = 1.0f;
    if (alive)
    {
        pulse = 0.65f + 0.35f * std::sin(phase * 6.28318f * 1.6f);
    }
    else
    {
        pulse = 0.35f;
    }
    ImU32 glow = (col & 0x00FFFFFF) | (uint32_t(80.0f * pulse) << 24);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(c, r * 2.2f, glow,  20);
    dl->AddCircleFilled(c, r,         col,  20);

    ImGui::Dummy(ImVec2((r + 2.0f) * 2.0f, fs));
}

void Subheading(const char *text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, Vec4(kTextDim));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void KeyLine(const char *label, const char *value)
{
    ImGui::PushStyleColor(ImGuiCol_Text, Vec4(kTextDim));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    float avail = ImGui::GetContentRegionAvail().x;
    float vw    = ImGui::CalcTextSize(value).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, avail - vw));
    ImGui::PushStyleColor(ImGuiCol_Text, Vec4(kTextHi));
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor();
}

void HeroStat(const char *label, const char *value, ImU32 valueCol)
{
    ImGui::BeginGroup();
    ImGui::SetWindowFontScale(2.1f);
    ImGui::PushStyleColor(ImGuiCol_Text, Vec4(valueCol));
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, Vec4(kTextDim));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::EndGroup();
}

void ProgressStrip(float frac, ImU32 fillCol, const char *caption)
{
    const float fs   = ImGui::GetFontSize();
    const float h    = std::max(6.0f, fs * 0.35f);
    const ImVec2 p0  = ImGui::GetCursorScreenPos();
    const float w    = ImGui::GetContentRegionAvail().x;
    const ImVec2 p1(p0.x + w, p0.y + h);
    const float clamped = std::clamp(frac, 0.0f, 1.0f);
    const ImVec2 fillEnd(p0.x + w * clamped, p1.y);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, kPanelDeep, h * 0.5f);
    if (clamped > 0.0f)
    {
        ImU32 fade = (fillCol & 0x00FFFFFF) | 0x60000000;
        dl->AddRectFilledMultiColor(p0, fillEnd, fade, fillCol, fillCol, fade);
        dl->AddRectFilled(p0, fillEnd, fillCol, h * 0.5f);
    }
    if (caption && caption[0])
    {
        ImVec2 ts = ImGui::CalcTextSize(caption);
        dl->AddText(ImVec2(p1.x - ts.x - 4.0f, p0.y + (h - ts.y) * 0.5f - 1.0f),
                    kTextHi, caption);
    }
    ImGui::Dummy(ImVec2(w, h));
}

bool Toggle(const char *id, bool *value)
{
    const float fs = ImGui::GetFontSize();
    const float w  = fs * 2.2f;
    const float h  = fs * 1.1f;
    ImVec2 p0      = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    bool clicked = ImGui::IsItemClicked();
    if (clicked)
    {
        *value = !*value;
    }
    float t  = *value ? 1.0f : 0.0f;
    ImU32 bg = *value ? kAccentSoft : kPanelDeep;
    ImU32 fg = *value ? kAccent     : kBorderHi;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), bg, h * 0.5f);
    dl->AddRect      (p0, ImVec2(p0.x + w, p0.y + h), kBorder, h * 0.5f, 0, 1.0f);

    float knobR = h * 0.4f;
    float knobX = p0.x + knobR + 2.0f + (w - (knobR + 2.0f) * 2.0f) * t;
    float knobY = p0.y + h * 0.5f;
    dl->AddCircleFilled(ImVec2(knobX, knobY), knobR, fg, 18);
    return clicked;
}

void AccentRule()
{
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.3f));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w   = ImGui::GetContentRegionAvail().x;
    const float bar = w * 0.18f;
    const float h   = 2.0f;
    const ImVec2 a(p0.x + (w - bar) * 0.5f, p0.y);
    const ImVec2 b(a.x + bar, p0.y + h);
    ImGui::GetWindowDrawList()->AddRectFilled(a, b, kAccent, h * 0.5f);
    ImGui::Dummy(ImVec2(w, h + ImGui::GetFontSize() * 0.3f));
}

bool RowHover()
{
    return ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup
                              | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
}

}
