#include "Panels.h"

#include "app/App.h"
#include "app/Theme.h"
#include "rpc/MsgPack.h"
#include "rpc/TapClient.h"

#include "imgui.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace nxtdbg::panels
{

namespace
{

constexpr size_t kRingCap = 5000;
constexpr int    kPrettyCap = 96;  // chars per cell before truncation

// --------------------------------------------------------------------------
// Pretty-print msgpack values into a single-line summary. Kept compact (no
// indentation) — the row cell is one line; the detail pane uses the same
// renderer with a higher cap so long maps still fit reasonably.
// --------------------------------------------------------------------------

void AppendValue(std::string &out, nxt::rpc::msgpack::Reader &r, int budget);

void AppendMap(std::string &out, nxt::rpc::msgpack::Reader &r, int budget)
{
    uint32_t n;
    if (!r.ReadMapHeader(n)) { out.append("<bad>"); return; }
    out.push_back('{');
    for (uint32_t i = 0; i < n; ++i)
    {
        if (i) out.append(", ");
        if (static_cast<int>(out.size()) >= budget) { out.append("…"); break; }
        const char *k = nullptr; uint32_t kLen = 0;
        if (!r.ReadString(k, kLen)) { out.append("<bad>"); return; }
        out.append(k, kLen);
        out.append(": ");
        AppendValue(out, r, budget);
    }
    out.push_back('}');
}

void AppendArray(std::string &out, nxt::rpc::msgpack::Reader &r, int budget)
{
    uint32_t n;
    if (!r.ReadArrayHeader(n)) { out.append("<bad>"); return; }
    out.push_back('[');
    for (uint32_t i = 0; i < n; ++i)
    {
        if (i) out.append(", ");
        if (static_cast<int>(out.size()) >= budget) { out.append("…"); break; }
        AppendValue(out, r, budget);
    }
    out.push_back(']');
}

void AppendValue(std::string &out, nxt::rpc::msgpack::Reader &r, int budget)
{
    using nxt::rpc::msgpack::Type;
    if (static_cast<int>(out.size()) >= budget) { out.append("…"); r.SkipValue(); return; }
    char buf[64];
    switch (r.Peek())
    {
    case Type::Nil:    out.append("nil"); r.ReadNil(); break;
    case Type::Bool:
    {
        bool v = false; r.ReadBool(v);
        out.append(v ? "true" : "false");
        break;
    }
    case Type::Int:
    {
        int64_t v = 0; r.ReadInt(v);
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        out.append(buf);
        break;
    }
    case Type::Float:
    {
        double v = 0.0; r.ReadDouble(v);
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
    case Type::Map:    AppendMap(out, r, budget); break;
    case Type::Array:  AppendArray(out, r, budget); break;
    default:           out.append("<?>"); r.SkipValue(); break;
    }
}

std::string PrettyOneLine(const uint8_t *bytes, uint32_t len, int budget)
{
    if (len == 0) return "(none)";
    nxt::rpc::msgpack::Reader r(bytes, len);
    std::string out;
    out.reserve(static_cast<size_t>(budget));
    AppendValue(out, r, budget);
    return out;
}

// --------------------------------------------------------------------------
// Tap entry — one row in the table. Carries pre-rendered strings so the UI
// thread never has to re-parse on scroll.
// --------------------------------------------------------------------------

struct TapEntry
{
    std::string  method;
    int64_t      id        = 0;
    int64_t      t_us      = 0;
    int64_t      dur_us    = 0;
    bool         isError   = false;
    std::string  paramsOneLine;
    std::string  replyOneLine;
    // Raw bytes for the detail pane — we re-parse on row click to render
    // a richer multi-line view.
    std::vector<uint8_t> paramsRaw;
    std::vector<uint8_t> replyRaw;
};

// Detect {id, error: "..."} vs {id, result: ...} from the raw reply bytes.
// Used at parse time so the row knows whether to color itself.
bool ReplyIsError(const uint8_t *bytes, uint32_t len)
{
    nxt::rpc::msgpack::Reader r(bytes, len);
    uint32_t mc;
    if (!r.ReadMapHeader(mc)) return false;
    for (uint32_t i = 0; i < mc; ++i)
    {
        const char *k = nullptr;
        uint32_t kLen = 0;
        if (!r.ReadString(k, kLen)) return false;
        if (nxt::rpc::msgpack::StrEq(k, kLen, "error")) return true;
        if (!r.SkipValue()) return false;
    }
    return false;
}

// Parse one rpc.tap data payload into a TapEntry. Called on the reader
// thread; UI thread only sees the finished struct.
bool ParseTap(const uint8_t *bytes, uint32_t len, TapEntry &out)
{
    nxt::rpc::msgpack::Reader r(bytes, len);
    uint32_t mc;
    if (!r.ReadMapHeader(mc)) return false;

    for (uint32_t i = 0; i < mc; ++i)
    {
        const char *k = nullptr;
        uint32_t kLen = 0;
        if (!r.ReadString(k, kLen)) return false;

        if (nxt::rpc::msgpack::StrEq(k, kLen, "method"))
        {
            const char *s = nullptr; uint32_t sLen = 0;
            if (!r.ReadString(s, sLen)) return false;
            out.method.assign(s, sLen);
        }
        else if (nxt::rpc::msgpack::StrEq(k, kLen, "id"))
        {
            if (!r.ReadInt(out.id)) return false;
        }
        else if (nxt::rpc::msgpack::StrEq(k, kLen, "t_us"))
        {
            if (!r.ReadInt(out.t_us)) return false;
        }
        else if (nxt::rpc::msgpack::StrEq(k, kLen, "dur_us"))
        {
            if (!r.ReadInt(out.dur_us)) return false;
        }
        else if (nxt::rpc::msgpack::StrEq(k, kLen, "params"))
        {
            const size_t start = r.Pos();
            if (!r.SkipValue()) return false;
            const uint32_t vLen = static_cast<uint32_t>(r.Pos() - start);
            out.paramsRaw.assign(bytes + start, bytes + start + vLen);
            out.paramsOneLine = PrettyOneLine(out.paramsRaw.data(), vLen,
                                              kPrettyCap);
        }
        else if (nxt::rpc::msgpack::StrEq(k, kLen, "reply"))
        {
            const size_t start = r.Pos();
            if (!r.SkipValue()) return false;
            const uint32_t vLen = static_cast<uint32_t>(r.Pos() - start);
            out.replyRaw.assign(bytes + start, bytes + start + vLen);
            out.replyOneLine = PrettyOneLine(out.replyRaw.data(), vLen,
                                             kPrettyCap);
            out.isError = ReplyIsError(out.replyRaw.data(), vLen);
        }
        else
        {
            if (!r.SkipValue()) return false;
        }
    }
    return true;
}

// --------------------------------------------------------------------------
// Panel state — function-static so it survives across frames + holds the
// tap client across attach cycles.
// --------------------------------------------------------------------------

struct PanelState
{
    rpc::TapClient        client;
    std::mutex            queueMx;
    std::deque<TapEntry>  ring;             // FIFO; oldest at front
    DWORD                 lastAttachedPid = 0;
    bool                  paused          = false;
    bool                  autoScroll      = true;
    char                  filter[64]      = {};
    int                   selectedRow     = -1;
    // Multi-line pretty render of the currently selected row's params + reply.
    std::string           selectedParamsMl;
    std::string           selectedReplyMl;
};

PanelState &State()
{
    static PanelState s;
    return s;
}

void OnTap(PanelState &s, const uint8_t *data, uint32_t len)
{
    TapEntry e{};
    if (!ParseTap(data, len, e)) return;
    std::lock_guard<std::mutex> lk(s.queueMx);
    if (s.paused) return;
    if (s.ring.size() >= kRingCap) s.ring.pop_front();
    s.ring.push_back(std::move(e));
}

// Pretty-print (multi-line) — reuses the one-line walker with no budget cap.
std::string PrettyMultiLine(const std::vector<uint8_t> &bytes)
{
    if (bytes.empty()) return "(none)";
    return PrettyOneLine(bytes.data(), static_cast<uint32_t>(bytes.size()),
                         1 << 20);    // effectively uncapped
}

void EnsureConnection(app::App &a, PanelState &s)
{
    DWORD pid = a.session.IsOpen() ? a.session.Pid() : 0;
    if (pid == s.lastAttachedPid && pid != 0 && s.client.IsConnected()) return;

    if (pid != s.lastAttachedPid)
    {
        s.client.Disconnect();
        {
            std::lock_guard<std::mutex> lk(s.queueMx);
            s.ring.clear();
            s.selectedRow = -1;
            s.selectedParamsMl.clear();
            s.selectedReplyMl.clear();
        }
        s.lastAttachedPid = pid;
    }
    if (pid != 0 && !s.client.IsConnected())
    {
        if (s.client.Connect(pid))
        {
            s.client.Subscribe("rpc.tap",
                [&s](const uint8_t *data, uint32_t len) { OnTap(s, data, len); });
        }
    }
}

const char *StatusText(rpc::TapStatus st)
{
    switch (st)
    {
    case rpc::TapStatus::Disconnected: return "DISC";
    case rpc::TapStatus::Connecting:   return "CONN";
    case rpc::TapStatus::Subscribed:   return "SUB";
    case rpc::TapStatus::Busy:         return "BUSY";
    case rpc::TapStatus::Error:        return "ERR";
    }
    return "?";
}

ImU32 StatusColor(rpc::TapStatus st)
{
    switch (st)
    {
    case rpc::TapStatus::Subscribed:   return theme::kGood;
    case rpc::TapStatus::Connecting:   return theme::kInfo;
    case rpc::TapStatus::Busy:         return theme::kWarn;
    case rpc::TapStatus::Error:        return theme::kBad;
    case rpc::TapStatus::Disconnected: return theme::kTextDim;
    }
    return theme::kTextDim;
}

bool MatchesFilter(const TapEntry &e, const char *filter)
{
    if (!filter || filter[0] == 0) return true;
    // Case-insensitive substring on method name.
    auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    };
    const size_t mLen = e.method.size();
    const size_t fLen = std::strlen(filter);
    if (fLen > mLen) return false;
    for (size_t i = 0; i + fLen <= mLen; ++i)
    {
        bool ok = true;
        for (size_t j = 0; j < fLen; ++j)
        {
            if (lower(e.method[i + j]) != lower(filter[j])) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

void DrawControlBar(app::App &a, PanelState &s, size_t shown, size_t total)
{
    if (!theme::BeginCard("rpctap.bar", "TAP")) { theme::EndCard(); return; }

    rpc::TapStatus st = s.client.LastStatus();
    theme::StatusDot(StatusColor(st),
                     st == rpc::TapStatus::Subscribed, a.dotPhase);
    ImGui::SameLine();
    theme::Pill(StatusText(st), (StatusColor(st) & 0x00FFFFFFu) | (50u << 24),
                StatusColor(st));

    ImGui::SameLine(0, ImGui::GetFontSize() * 1.5f);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%zu / %zu", shown, total);
    theme::HeroStat("frames", buf, theme::kTextHi);

    ImGui::SameLine(0, ImGui::GetFontSize() * 2);
    ImGui::BeginGroup();
    theme::Subheading("paused");
    theme::Toggle("##tappp", &s.paused);
    ImGui::EndGroup();

    ImGui::SameLine(0, ImGui::GetFontSize() * 1.5f);
    ImGui::BeginGroup();
    theme::Subheading("auto-scroll");
    theme::Toggle("##tapas", &s.autoScroll);
    ImGui::EndGroup();

    ImGui::SameLine(0, ImGui::GetFontSize() * 1.5f);
    if (ImGui::Button("Clear"))
    {
        std::lock_guard<std::mutex> lk(s.queueMx);
        s.ring.clear();
        s.selectedRow = -1;
        s.selectedParamsMl.clear();
        s.selectedReplyMl.clear();
    }

    ImGui::SameLine(0, ImGui::GetFontSize());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::InputTextWithHint("##tapfilter", "filter method",
                             s.filter, sizeof(s.filter));

    theme::EndCard();
}

void DrawTable(PanelState &s)
{
    const float fs = ImGui::GetFontSize();
    if (!ImGui::BeginTable("taprows", 5,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable))
    {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Id",     ImGuiTableColumnFlags_WidthFixed,  fs * 4.5f);
    ImGui::TableSetupColumn("Method", ImGuiTableColumnFlags_WidthFixed,  fs * 16.0f);
    ImGui::TableSetupColumn("dur μs", ImGuiTableColumnFlags_WidthFixed,  fs * 5.5f);
    ImGui::TableSetupColumn("Params", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Reply",  ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    std::lock_guard<std::mutex> lk(s.queueMx);
    int row = 0;
    for (const auto &e : s.ring)
    {
        if (!MatchesFilter(e, s.filter)) { ++row; continue; }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::ColorConvertU32ToFloat4(theme::kTextDim));
        ImGui::Text("%lld", static_cast<long long>(e.id));
        ImGui::PopStyleColor();

        ImGui::TableSetColumnIndex(1);
        bool selected = (row == s.selectedRow);
        ImU32 mCol = e.isError ? theme::kBad : theme::kTextHi;
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::ColorConvertU32ToFloat4(mCol));
        if (ImGui::Selectable(e.method.c_str(), selected,
                              ImGuiSelectableFlags_SpanAllColumns))
        {
            s.selectedRow      = row;
            s.selectedParamsMl = PrettyMultiLine(e.paramsRaw);
            s.selectedReplyMl  = PrettyMultiLine(e.replyRaw);
        }
        ImGui::PopStyleColor();

        ImGui::TableSetColumnIndex(2);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::ColorConvertU32ToFloat4(theme::kTextDim));
        ImGui::Text("%lld", static_cast<long long>(e.dur_us));
        ImGui::PopStyleColor();

        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(e.paramsOneLine.c_str());

        ImGui::TableSetColumnIndex(4);
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(e.isError ? theme::kBad : theme::kText));
        ImGui::TextUnformatted(e.replyOneLine.c_str());
        ImGui::PopStyleColor();

        ++row;
    }

    if (s.autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() -
                                                ImGui::GetFontSize())
    {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndTable();
}

void DrawDetail(PanelState &s)
{
    if (s.selectedRow < 0) return;
    if (!theme::BeginCard("rpctap.detail", "DETAIL")) { theme::EndCard(); return; }

    ImGui::TextDisabled("Params");
    ImGui::BeginChild("##tapparams",
        ImVec2(0, ImGui::GetFontSize() * 8),
        ImGuiChildFlags_Border);
    ImGui::TextUnformatted(s.selectedParamsMl.c_str());
    ImGui::EndChild();

    ImGui::TextDisabled("Reply");
    ImGui::BeginChild("##tapreply",
        ImVec2(0, ImGui::GetFontSize() * 10),
        ImGuiChildFlags_Border);
    ImGui::TextUnformatted(s.selectedReplyMl.c_str());
    ImGui::EndChild();

    theme::EndCard();
}

}

void DrawRpcTap(app::App &a)
{
    ImGui::SetNextWindowSize(
        ImVec2(ImGui::GetFontSize() * 72, ImGui::GetFontSize() * 34),
        ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("RPC Tap"))
    {
        ImGui::End();
        return;
    }

    PanelState &s = State();
    EnsureConnection(a, s);

    size_t total;
    {
        std::lock_guard<std::mutex> lk(s.queueMx);
        total = s.ring.size();
    }
    size_t shown = total;
    if (s.filter[0])
    {
        shown = 0;
        std::lock_guard<std::mutex> lk(s.queueMx);
        for (const auto &e : s.ring)
            if (MatchesFilter(e, s.filter)) ++shown;
    }

    DrawControlBar(a, s, shown, total);
    ImGui::Spacing();
    DrawTable(s);
    DrawDetail(s);

    ImGui::End();
}

}
