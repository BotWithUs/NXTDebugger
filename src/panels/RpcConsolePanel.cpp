#include "Panels.h"

#include "app/App.h"
#include "app/Theme.h"
#include "rpc/Methods.h"
#include "rpc/MsgPack.h"
#include "rpc/RpcClient.h"

#include "imgui.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace nxtdbg::panels
{

namespace
{

constexpr uint32_t kHistoryCap = 64;
constexpr DWORD    kDefaultTimeoutMs = 2000;

struct ParamValue
{
    // One per ParamSpec::name. Curated methods populate this up front; the
    // panel mutates the right field based on ParamSpec::kind.
    int64_t   asInt;
    bool      asBool;
    char      asStr[256];
};

struct HistoryEntry
{
    char          method[64];
    rpc::CallStatus status;
    uint32_t      latencyMs;
    std::string   resultPretty;   // pre-rendered for fast scrolling
};

// ------------------------------------------------------------------
// Pretty-print decoded msgpack.
//
// Recursive walker that produces "indented text" — kept inside the panel
// because nothing else needs it. Walks via the project's msgpack Reader.
// ------------------------------------------------------------------

void AppendIndent(std::string &out, int depth)
{
    for (int i = 0; i < depth; ++i) out.append("  ");
}

void AppendValue(std::string &out, nxt::rpc::msgpack::Reader &r, int depth);

void AppendMap(std::string &out, nxt::rpc::msgpack::Reader &r, int depth)
{
    uint32_t n;
    if (!r.ReadMapHeader(n))
    {
        out.append("<bad map>");
        return;
    }
    if (n == 0)
    {
        out.append("{}");
        return;
    }
    out.append("{\n");
    for (uint32_t i = 0; i < n; ++i)
    {
        AppendIndent(out, depth + 1);
        const char *k = nullptr; uint32_t kLen = 0;
        if (!r.ReadString(k, kLen))
        {
            out.append("<bad key>\n");
            return;
        }
        out.append(k, kLen);
        out.append(": ");
        AppendValue(out, r, depth + 1);
        out.append("\n");
    }
    AppendIndent(out, depth);
    out.append("}");
}

void AppendArray(std::string &out, nxt::rpc::msgpack::Reader &r, int depth)
{
    uint32_t n;
    if (!r.ReadArrayHeader(n))
    {
        out.append("<bad array>");
        return;
    }
    if (n == 0)
    {
        out.append("[]");
        return;
    }
    out.append("[\n");
    for (uint32_t i = 0; i < n; ++i)
    {
        AppendIndent(out, depth + 1);
        AppendValue(out, r, depth + 1);
        out.append("\n");
    }
    AppendIndent(out, depth);
    out.append("]");
}

void AppendValue(std::string &out, nxt::rpc::msgpack::Reader &r, int depth)
{
    using nxt::rpc::msgpack::Type;
    Type t = r.Peek();
    char buf[64];
    switch (t)
    {
    case Type::Nil:
        out.append("nil");
        r.ReadNil();
        break;
    case Type::Bool:
    {
        bool v = false;
        r.ReadBool(v);
        out.append(v ? "true" : "false");
        break;
    }
    case Type::Int:
    {
        int64_t v = 0;
        r.ReadInt(v);
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        out.append(buf);
        break;
    }
    case Type::Float:
    {
        double v = 0.0;
        r.ReadDouble(v);
        std::snprintf(buf, sizeof(buf), "%g", v);
        out.append(buf);
        break;
    }
    case Type::Str:
    {
        const char *s = nullptr; uint32_t n = 0;
        r.ReadString(s, n);
        out.push_back('"');
        out.append(s, n);
        out.push_back('"');
        break;
    }
    case Type::Map:
        AppendMap(out, r, depth);
        break;
    case Type::Array:
        AppendArray(out, r, depth);
        break;
    case Type::Invalid:
    default:
        out.append("<invalid>");
        break;
    }
}

std::string PrettyFromBytes(const std::vector<uint8_t> &bytes)
{
    if (bytes.empty()) return "(empty)";
    nxt::rpc::msgpack::Reader r(bytes.data(), bytes.size());
    std::string out;
    AppendValue(out, r, 0);
    return out;
}

// ------------------------------------------------------------------
// Method-list cache + rpc.list_methods refresh
// ------------------------------------------------------------------

struct MethodList
{
    std::vector<std::string> names;
    bool                     dirty = true;
};

const char *StatusText(rpc::CallStatus s)
{
    switch (s)
    {
    case rpc::CallStatus::Ok:            return "OK";
    case rpc::CallStatus::Busy:          return "BUSY";
    case rpc::CallStatus::Disconnected:  return "DISC";
    case rpc::CallStatus::Timeout:       return "TIMEOUT";
    case rpc::CallStatus::ProtocolError: return "PROTO";
    case rpc::CallStatus::HandlerError:  return "HERR";
    }
    return "?";
}

ImU32 StatusColor(rpc::CallStatus s)
{
    switch (s)
    {
    case rpc::CallStatus::Ok:            return theme::kGood;
    case rpc::CallStatus::Busy:          return theme::kWarn;
    case rpc::CallStatus::HandlerError:  return theme::kWarn;
    case rpc::CallStatus::Timeout:       return theme::kBad;
    case rpc::CallStatus::ProtocolError: return theme::kBad;
    case rpc::CallStatus::Disconnected:  return theme::kTextDim;
    }
    return theme::kTextDim;
}

void RefreshMethodList(rpc::RpcClient &client, MethodList &list)
{
    list.names.clear();
    if (!client.IsConnected()) return;

    std::vector<uint8_t> result;
    auto status = client.Call("rpc.list_methods", nullptr, 0, result,
                              kDefaultTimeoutMs);
    if (status != rpc::CallStatus::Ok)
    {
        list.dirty = true;   // try again later
        return;
    }
    nxt::rpc::msgpack::Reader r(result.data(), result.size());
    uint32_t n = 0;
    if (!r.ReadArrayHeader(n))
    {
        list.dirty = true;
        return;
    }
    list.names.reserve(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        const char *s = nullptr; uint32_t sLen = 0;
        if (!r.ReadString(s, sLen)) break;
        list.names.emplace_back(s, sLen);
    }
    list.dirty = false;
}

// ------------------------------------------------------------------
// Encode params from the typed form into msgpack
// ------------------------------------------------------------------

void EncodeParams(const rpc::MethodSpec  &spec,
                  const ParamValue       *values,
                  std::vector<uint8_t>   &out)
{
    out.clear();
    if (spec.paramCount == 0) return;

    out.resize(4096);
    nxt::rpc::msgpack::Writer w(out.data(), out.size());
    w.WriteMapHeader(spec.paramCount);
    for (uint32_t i = 0; i < spec.paramCount; ++i)
    {
        const auto &p = spec.params[i];
        w.WriteCStr(p.name);
        switch (p.kind)
        {
        case rpc::ParamKind::Int:
            w.WriteInt(values[i].asInt);
            break;
        case rpc::ParamKind::Bool:
            w.WriteBool(values[i].asBool);
            break;
        case rpc::ParamKind::Str:
            w.WriteCStr(values[i].asStr);
            break;
        }
    }
    out.resize(w.BytesWritten());
}

// ------------------------------------------------------------------
// Panel state — function-static so the panel survives frame to frame.
// ------------------------------------------------------------------

struct PanelState
{
    rpc::RpcClient client;
    MethodList     methods;
    char           filter[64]   = {};
    int            selectedIdx  = -1;       // index into methods.names
    char           selectedName[96] = {};
    ParamValue     values[8]    = {};       // sized for largest curated form
    int            timeoutMs    = static_cast<int>(kDefaultTimeoutMs);
    std::vector<HistoryEntry> history;
    DWORD          lastAttachedPid = 0;
};

PanelState &State()
{
    static PanelState s;
    return s;
}

void EnsureConnection(app::App &a, PanelState &s)
{
    DWORD pid = a.session.IsOpen() ? a.session.Pid() : 0;
    if (pid == s.lastAttachedPid && pid != 0 && s.client.IsConnected()) return;
    if (pid != s.lastAttachedPid)
    {
        s.client.Disconnect();
        s.methods.names.clear();
        s.methods.dirty = true;
        s.history.clear();
        s.lastAttachedPid = pid;
    }
    if (pid != 0 && !s.client.IsConnected())
    {
        s.client.Connect(pid);
        if (s.client.IsConnected()) s.methods.dirty = true;
    }
}

void DrawStatusCard(PanelState &s, DWORD attachedPid)
{
    if (!theme::BeginCard("rpc.status", "RPC PIPE"))
    {
        theme::EndCard();
        return;
    }

    rpc::CallStatus st = s.client.IsConnected()
                       ? rpc::CallStatus::Ok
                       : s.client.LastStatus();
    const char *label = attachedPid == 0   ? "NOT ATTACHED"
                      : s.client.IsConnected() ? "LIVE"
                      : StatusText(st);

    theme::StatusDot(StatusColor(st),
                     s.client.IsConnected(),
                     0.5f);
    ImGui::SameLine();
    theme::HeroStat("pipe",
                    label,
                    StatusColor(st));

    if (attachedPid != 0)
    {
        char pidBuf[32];
        std::snprintf(pidBuf, sizeof(pidBuf), "pid %lu", attachedPid);
        theme::KeyLine("attached", pidBuf);
    }

    char countBuf[32] = "—";
    if (s.client.IsConnected())
    {
        std::snprintf(countBuf, sizeof(countBuf), "%u methods",
                      static_cast<uint32_t>(s.methods.names.size()));
    }
    theme::KeyLine("registry", countBuf);

    if (!s.client.IsConnected())
    {
        wchar_t const *err = s.client.LastError();
        if (err && err[0] != 0)
        {
            char e[512];
            WideCharToMultiByte(CP_UTF8, 0, err, -1, e, sizeof(e), nullptr, nullptr);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertU32ToFloat4(theme::kBad));
            ImGui::TextWrapped("%s", e);
            ImGui::PopStyleColor();
        }
        if (attachedPid != 0)
        {
            if (ImGui::Button("Retry connect"))
            {
                s.client.Connect(attachedPid);
                if (s.client.IsConnected()) s.methods.dirty = true;
            }
        }
    }
    else
    {
        if (ImGui::Button("Refresh methods")) s.methods.dirty = true;
        ImGui::SameLine();
        if (ImGui::Button("Disconnect")) s.client.Disconnect();
    }

    theme::EndCard();
}

bool PassesFilter(const std::string &name, const char *filter)
{
    if (!filter || !filter[0]) return true;
    return name.find(filter) != std::string::npos;
}

void DrawMethodList(PanelState &s)
{
    if (!theme::BeginCard("rpc.methods", "METHODS"))
    {
        theme::EndCard();
        return;
    }

    ImGui::PushItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "filter…", s.filter, sizeof(s.filter));
    ImGui::PopItemWidth();

    ImGui::BeginChild("##methodlist",
                      ImVec2(0, ImGui::GetFontSize() * 22),
                      ImGuiChildFlags_Border);
    for (int i = 0; i < static_cast<int>(s.methods.names.size()); ++i)
    {
        const auto &name = s.methods.names[i];
        if (!PassesFilter(name, s.filter)) continue;
        bool selected = (i == s.selectedIdx);
        if (ImGui::Selectable(name.c_str(), selected))
        {
            s.selectedIdx = i;
            std::snprintf(s.selectedName, sizeof(s.selectedName), "%s",
                          name.c_str());
            // Pre-populate the typed form's defaults for curated methods.
            const auto *spec = rpc::FindMethodSpec(name.c_str(),
                                                   static_cast<uint32_t>(name.size()));
            if (spec)
            {
                for (uint32_t p = 0; p < spec->paramCount && p < 8; ++p)
                {
                    s.values[p].asInt  = spec->params[p].defaultInt;
                    s.values[p].asBool = spec->params[p].defaultBool;
                    if (spec->params[p].defaultStr)
                    {
                        std::snprintf(s.values[p].asStr,
                                      sizeof(s.values[p].asStr),
                                      "%s", spec->params[p].defaultStr);
                    }
                    else
                    {
                        s.values[p].asStr[0] = 0;
                    }
                }
            }
        }
    }
    ImGui::EndChild();
    theme::EndCard();
}

bool DrawTypedForm(const rpc::MethodSpec &spec, ParamValue *values)
{
    for (uint32_t i = 0; i < spec.paramCount && i < 8; ++i)
    {
        const auto &p = spec.params[i];
        ImGui::PushID(static_cast<int>(i));
        switch (p.kind)
        {
        case rpc::ParamKind::Int:
        {
            int v = static_cast<int>(values[i].asInt);
            if (ImGui::InputInt(p.name, &v))
            {
                values[i].asInt = v;
            }
            break;
        }
        case rpc::ParamKind::Bool:
            ImGui::Checkbox(p.name, &values[i].asBool);
            break;
        case rpc::ParamKind::Str:
            ImGui::InputText(p.name, values[i].asStr,
                             sizeof(values[i].asStr));
            break;
        }
        ImGui::PopID();
    }
    return true;
}

void PushHistory(PanelState &s, const char *method,
                 rpc::CallStatus status, uint32_t latencyMs,
                 std::string pretty)
{
    HistoryEntry e{};
    std::snprintf(e.method, sizeof(e.method), "%s", method);
    e.status       = status;
    e.latencyMs    = latencyMs;
    e.resultPretty = std::move(pretty);
    s.history.insert(s.history.begin(), std::move(e));
    if (s.history.size() > kHistoryCap)
    {
        s.history.resize(kHistoryCap);
    }
}

void DrawCallCard(PanelState &s)
{
    if (!theme::BeginCard("rpc.call", "CALL"))
    {
        theme::EndCard();
        return;
    }
    if (s.selectedIdx < 0 || s.selectedIdx >= static_cast<int>(s.methods.names.size()))
    {
        theme::Subheading("Select a method on the left.");
        theme::EndCard();
        return;
    }

    const auto &name = s.methods.names[s.selectedIdx];
    const auto *spec = rpc::FindMethodSpec(name.c_str(),
                                           static_cast<uint32_t>(name.size()));

    ImGui::PushFont(nullptr);   // no font scaling, but keeps push/pop pairs explicit
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::kAccent),
                       "%s", name.c_str());
    ImGui::PopFont();

    if (spec)
    {
        theme::Subheading(spec->summary);
        ImGui::Spacing();
        DrawTypedForm(*spec, s.values);
    }
    else
    {
        theme::Subheading("No schema — calling with empty params.");
    }

    ImGui::Spacing();
    ImGui::PushItemWidth(ImGui::GetFontSize() * 6);
    ImGui::SliderInt("timeout (ms)", &s.timeoutMs, 100, 10000);
    ImGui::PopItemWidth();
    ImGui::SameLine();

    bool disabled = !s.client.IsConnected();
    if (disabled) ImGui::BeginDisabled();
    if (ImGui::Button("Call"))
    {
        std::vector<uint8_t> params;
        if (spec) EncodeParams(*spec, s.values, params);

        std::vector<uint8_t> result;
        LARGE_INTEGER t0, t1, freq;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        auto status = s.client.Call(name.c_str(),
                                    params.empty() ? nullptr : params.data(),
                                    static_cast<uint32_t>(params.size()),
                                    result,
                                    static_cast<DWORD>(s.timeoutMs));
        QueryPerformanceCounter(&t1);
        uint32_t latencyMs = static_cast<uint32_t>(
            (t1.QuadPart - t0.QuadPart) * 1000 / freq.QuadPart);

        std::string pretty;
        if (status == rpc::CallStatus::Ok)
        {
            pretty = PrettyFromBytes(result);
        }
        else if (status == rpc::CallStatus::HandlerError)
        {
            pretty = "error: " + PrettyFromBytes(result);
        }
        else
        {
            pretty = std::string("[") + StatusText(status) + "]";
            const wchar_t *err = s.client.LastError();
            if (err && err[0] != 0)
            {
                char e[512];
                WideCharToMultiByte(CP_UTF8, 0, err, -1, e, sizeof(e),
                                    nullptr, nullptr);
                pretty.append(" ");
                pretty.append(e);
            }
        }
        PushHistory(s, name.c_str(), status, latencyMs, std::move(pretty));
    }
    if (disabled) ImGui::EndDisabled();

    theme::EndCard();
}

void DrawHistoryCard(PanelState &s)
{
    if (!theme::BeginCard("rpc.history", "HISTORY"))
    {
        theme::EndCard();
        return;
    }
    if (s.history.empty())
    {
        theme::Subheading("Calls will land here.");
        theme::EndCard();
        return;
    }
    if (ImGui::BeginTable("##history", 4,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_Borders |
                          ImGuiTableFlags_ScrollY,
                          ImVec2(0, ImGui::GetFontSize() * 14)))
    {
        ImGui::TableSetupColumn("status",
                                ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFontSize() * 4);
        ImGui::TableSetupColumn("method",
                                ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFontSize() * 12);
        ImGui::TableSetupColumn("ms",
                                ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetFontSize() * 3);
        ImGui::TableSetupColumn("result",
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < s.history.size(); ++i)
        {
            const auto &h = s.history[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertU32ToFloat4(StatusColor(h.status)));
            ImGui::TextUnformatted(StatusText(h.status));
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(h.method);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u", h.latencyMs);
            ImGui::TableSetColumnIndex(3);
            // Single-line preview; multi-line bodies get truncated visually
            // but full text remains in resultPretty for the detail pane.
            const char *p = h.resultPretty.c_str();
            const char *nl = std::strchr(p, '\n');
            if (nl)
            {
                ImGui::TextUnformatted(p, nl);
            }
            else
            {
                ImGui::TextUnformatted(p);
            }
        }
        ImGui::EndTable();
    }

    // Detail view of the most-recent call.
    if (!s.history.empty())
    {
        ImGui::Spacing();
        theme::Subheading("Last result");
        const auto &h = s.history.front();
        ImGui::PushStyleColor(ImGuiCol_FrameBg,
            ImGui::ColorConvertU32ToFloat4(theme::kPanelDeep));
        ImGui::InputTextMultiline("##lastresult",
            const_cast<char *>(h.resultPretty.c_str()),
            h.resultPretty.size() + 1,
            ImVec2(-FLT_MIN, ImGui::GetFontSize() * 10),
            ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleColor();
    }

    theme::EndCard();
}

}

void DrawRpcConsole(app::App &a)
{
    auto &s = State();

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 56,
                                    ImGui::GetFontSize() * 40),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("RPC console"))
    {
        ImGui::End();
        return;
    }

    EnsureConnection(a, s);
    DWORD pid = a.session.IsOpen() ? a.session.Pid() : 0;

    if (s.client.IsConnected() && s.methods.dirty)
    {
        RefreshMethodList(s.client, s.methods);
    }

    DrawStatusCard(s, pid);
    ImGui::Spacing();

    // Two-column body: method list | call + history.
    float colWidth = ImGui::GetContentRegionAvail().x * 0.32f;
    ImGui::BeginChild("##rpcleft", ImVec2(colWidth, 0));
    DrawMethodList(s);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##rpcright", ImVec2(0, 0));
    DrawCallCard(s);
    ImGui::Spacing();
    DrawHistoryCard(s);
    ImGui::EndChild();

    ImGui::End();
}

}
