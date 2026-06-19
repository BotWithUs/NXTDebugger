#include "Panels.h"

#include "app/App.h"
#include "app/Theme.h"

#include "imgui.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace nxtdbg::panels
{

namespace
{

struct ScriptState
{
    int                      scriptId = -1;
    int                      source   = 0;     // 0 = asm, 1 = decompiled
    std::vector<std::string> lines;
    char                     highlight[64] = {};
    char                     loadError[256] = {};
    int                      idInput  = 0;
};

ScriptState &State()
{
    static ScriptState s;
    return s;
}

ImVec4 Vec4(ImU32 c)
{
    return ImGui::ColorConvertU32ToFloat4(c);
}

bool ReadWholeFile(const std::wstring &path, std::string &out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        return false;
    }
    f.seekg(0, std::ios::end);
    std::streamoff len = f.tellg();
    if (len <= 0)
    {
        out.clear();
        return true;
    }
    f.seekg(0);
    out.resize(static_cast<size_t>(len));
    f.read(out.data(), len);
    return true;
}

void SplitLines(const std::string &text, std::vector<std::string> &out)
{
    out.clear();
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i)
    {
        if (i == text.size() || text[i] == '\n')
        {
            size_t len = i - start;
            if (len > 0 && text[start + len - 1] == '\r')
            {
                --len;
            }
            out.emplace_back(text.substr(start, len));
            start = i + 1;
        }
    }
}

// Build "<corpus>\<id:06d>.asm", or the sibling decompiled "<...>\cs2_decompiled\
// <id:06d>.cs2" when source==1 (the decompiled tree sits next to cs2_asm).
std::wstring ScriptPath(const std::wstring &asmDir, int id, int source)
{
    wchar_t name[32];
    if (source == 1)
    {
        std::swprintf(name, 32, L"%06d.cs2", id);
        size_t slash = asmDir.find_last_of(L"\\/");
        std::wstring parent = (slash == std::wstring::npos) ? asmDir : asmDir.substr(0, slash);
        return parent + L"\\cs2_decompiled\\" + name;
    }
    std::swprintf(name, 32, L"%06d.asm", id);
    return asmDir + L"\\" + name;
}

void LoadScript(app::App &a, ScriptState &s, int id)
{
    s.scriptId = id;
    s.idInput  = id;
    s.loadError[0] = 0;
    std::wstring path = ScriptPath(a.cs2.Dir(), id, s.source);
    std::string text;
    if (ReadWholeFile(path, text))
    {
        SplitLines(text, s.lines);
    }
    else
    {
        s.lines.clear();
        char p[600];
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, p, sizeof(p), nullptr, nullptr);
        std::snprintf(s.loadError, sizeof(s.loadError), "Could not open %s", p);
    }
}

void DrawToolbar(app::App &a, ScriptState &s)
{
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    ImGui::InputInt("##cs2id", &s.idInput);
    ImGui::SameLine();
    if (ImGui::Button("Load") && s.idInput >= 0)
    {
        LoadScript(a, s, s.idInput);
    }
    ImGui::SameLine();
    int wasSource = s.source;
    ImGui::RadioButton("asm", &s.source, 0);
    ImGui::SameLine();
    ImGui::RadioButton("decompiled", &s.source, 1);
    if (wasSource != s.source && s.scriptId >= 0)
    {
        LoadScript(a, s, s.scriptId);
    }
    ImGui::SameLine(0, ImGui::GetFontSize());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
    ImGui::InputTextWithHint("##cs2hl", "highlight", s.highlight, sizeof(s.highlight));
}

void DrawListing(ScriptState &s)
{
    if (!theme::BeginCard("cs2.listing", "DISASSEMBLY"))
    {
        theme::EndCard();
        return;
    }
    if (s.loadError[0])
    {
        ImGui::PushStyleColor(ImGuiCol_Text, Vec4(theme::kBad));
        ImGui::TextWrapped("%s", s.loadError);
        ImGui::PopStyleColor();
        theme::EndCard();
        return;
    }
    if (s.lines.empty())
    {
        theme::Subheading("Pick a script from a CS2 xref list, or load one by id.");
        theme::EndCard();
        return;
    }
    char hdr[48];
    std::snprintf(hdr, sizeof(hdr), "script %06d · %zu lines", s.scriptId, s.lines.size());
    theme::Subheading(hdr);
    ImGui::BeginChild("##cs2text", ImVec2(0, 0), 0);
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(s.lines.size()));
    while (clip.Step())
    {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
        {
            const std::string &ln = s.lines[static_cast<size_t>(i)];
            bool hl = s.highlight[0] && std::strstr(ln.c_str(), s.highlight);
            if (hl)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, Vec4(theme::kAccent));
            }
            ImGui::TextUnformatted(ln.empty() ? " " : ln.c_str());
            if (hl)
            {
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::EndChild();
    theme::EndCard();
}

}

void DrawCs2Script(app::App &a)
{
    ScriptState &s = State();

    bool pending = a.cs2RequestedScript >= 0;
    if (pending)
    {
        ImGui::SetNextWindowFocus();
    }

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 52, ImGui::GetFontSize() * 38),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("CS2 script"))
    {
        ImGui::End();
        return;
    }

    if (pending)
    {
        if (a.cs2RequestedHighlight >= 0)
        {
            std::snprintf(s.highlight, sizeof(s.highlight), "%d", a.cs2RequestedHighlight);
        }
        LoadScript(a, s, a.cs2RequestedScript);
        a.cs2RequestedScript    = -1;
        a.cs2RequestedHighlight = -1;
    }

    DrawToolbar(a, s);
    ImGui::Spacing();
    DrawListing(s);

    ImGui::End();
}

}
