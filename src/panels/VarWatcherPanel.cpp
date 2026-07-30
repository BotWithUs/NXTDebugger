#include "Panels.h"

#include "app/App.h"
#include "app/Theme.h"
#include "ipc/Events.h"
#include "rpc/MsgPack.h"
#include "rpc/RpcClient.h"
#include "wire/EventReader.h"

#include "imgui.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace nxtdbg::panels
{

namespace
{

namespace mp = nxt::rpc::msgpack;

constexpr DWORD kRpcTimeoutMs   = 1000;
constexpr float kPollDecayPerSec = 1.5f;   // change-flash fade rate

// The four things a debugger watches. Varp / varc-int / varc-string are polled
// over the batched RPCs; varbit has NO read RPC on the agent (varbits decode
// consumer-side), so a varbit row updates only when a kEventVarbitChange flows
// past on the event ring — surfaced honestly in the value cell.
enum class VarType
{
    Varp,
    Varbit,
    VarcInt,
    VarcString,
};

struct Watch
{
    VarType     type   = VarType::Varp;
    int         id     = 0;
    bool        hasValue = false;
    bool        found    = false;
    int64_t     value    = 0;     // int-valued types
    std::string strValue;         // VarcString
    float       flash    = 0.0f;  // 1.0 on change, decays toward 0
};

struct WatchState
{
    std::vector<Watch> rows;
    int      addId    = 0;
    int      addType  = 0;
    bool     autoPoll = true;
    bool     forcePoll = false;
    float    pollAccum = 0.0f;
    float    pollIntervalSec = 0.6f;
    uint64_t lastSeenSeq = 0;
    bool     primedSeq = false;
    int      removeIdx = -1;
};

const char *TypeLabel(VarType t)
{
    switch (t)
    {
        case VarType::Varp:       return "varp";
        case VarType::Varbit:     return "varbit";
        case VarType::VarcInt:    return "varc-int";
        case VarType::VarcString: return "varc-str";
    }
    return "?";
}

ImU32 TypeColor(VarType t)
{
    switch (t)
    {
        case VarType::Varp:       return theme::kInfo;
        case VarType::Varbit:     return theme::kAccent;
        case VarType::VarcInt:    return theme::kGood;
        case VarType::VarcString: return theme::kWarn;
    }
    return theme::kTextDim;
}

// gameval type key for the symbolic-name lookup, or nullptr where the bundled
// tables have no matching family (varc).
const char *GamevalType(VarType t)
{
    switch (t)
    {
        case VarType::Varp:   return "varp";
        case VarType::Varbit: return "varbit";
        default:              return nullptr;
    }
}

ImU32 LerpColor(ImU32 from, ImU32 to, float t)
{
    ImVec4 a = ImGui::ColorConvertU32ToFloat4(from);
    ImVec4 b = ImGui::ColorConvertU32ToFloat4(to);
    float  k = std::clamp(t, 0.0f, 1.0f);
    ImVec4 r(a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k,
             a.z + (b.z - a.z) * k, a.w + (b.w - a.w) * k);
    return ImGui::ColorConvertFloat4ToU32(r);
}

// --- msgpack helpers -------------------------------------------------------

void EncodeIds(std::vector<uint8_t> &out, const std::vector<int> &ids)
{
    out.assign(16 + ids.size() * 9, 0);
    mp::Writer w(out.data(), out.size());
    w.WriteMapHeader(1);
    w.WriteCStr("ids");
    w.WriteArrayHeader(static_cast<uint32_t>(ids.size()));
    for (int id : ids)
    {
        w.WriteInt(id);
    }
    out.resize(w.BytesWritten());
}

void ReadIntArray(mp::Reader &r, std::vector<int64_t> &out)
{
    uint32_t n = 0;
    if (!r.ReadArrayHeader(n))
    {
        return;
    }
    out.resize(n);
    for (uint32_t j = 0; j < n; ++j)
    {
        int64_t v = 0;
        r.ReadInt(v);
        out[j] = v;
    }
}

void ReadBoolArray(mp::Reader &r, std::vector<uint8_t> &out)
{
    uint32_t n = 0;
    if (!r.ReadArrayHeader(n))
    {
        return;
    }
    out.resize(n);
    for (uint32_t j = 0; j < n; ++j)
    {
        bool b = false;
        r.ReadBool(b);
        out[j] = b ? 1u : 0u;
    }
}

void ReadStrArray(mp::Reader &r, std::vector<std::string> &out)
{
    uint32_t n = 0;
    if (!r.ReadArrayHeader(n))
    {
        return;
    }
    out.resize(n);
    for (uint32_t j = 0; j < n; ++j)
    {
        const char *s = nullptr;
        uint32_t    sl = 0;
        if (r.ReadString(s, sl))
        {
            out[j].assign(s, sl);
        }
    }
}

// --- value application -----------------------------------------------------

void ApplyInt(Watch &w, int64_t v, bool found)
{
    if (!w.hasValue || w.value != v)
    {
        w.flash = 1.0f;
    }
    w.value    = v;
    w.found    = found;
    w.hasValue = true;
}

void ApplyStr(Watch &w, std::string v, bool found)
{
    if (!w.hasValue || w.strValue != v)
    {
        w.flash = 1.0f;
    }
    w.strValue = std::move(v);
    w.found    = found;
    w.hasValue = true;
}

// --- polling ---------------------------------------------------------------

void PollIntType(app::App &a, WatchState &st, VarType type, const char *method)
{
    std::vector<int>    ids;
    std::vector<size_t> idxs;
    for (size_t i = 0; i < st.rows.size(); ++i)
    {
        if (st.rows[i].type == type)
        {
            ids.push_back(st.rows[i].id);
            idxs.push_back(i);
        }
    }
    if (ids.empty())
    {
        return;
    }
    std::vector<uint8_t> params;
    EncodeIds(params, ids);
    std::vector<uint8_t> reply;
    if (a.rpc.Call(method, params.data(), static_cast<uint32_t>(params.size()),
                   reply, kRpcTimeoutMs) != rpc::CallStatus::Ok)
    {
        return;
    }
    std::vector<int64_t> values;
    std::vector<uint8_t> found;
    mp::Reader r(reply.data(), reply.size());
    uint32_t n = 0;
    if (!r.ReadMapHeader(n))
    {
        return;
    }
    for (uint32_t i = 0; i < n; ++i)
    {
        const char *k = nullptr;
        uint32_t    kl = 0;
        if (!r.ReadString(k, kl))
        {
            break;
        }
        if (mp::StrEq(k, kl, "values"))      ReadIntArray(r, values);
        else if (mp::StrEq(k, kl, "found"))  ReadBoolArray(r, found);
        else                                 r.SkipValue();
    }
    for (size_t k = 0; k < idxs.size() && k < values.size(); ++k)
    {
        ApplyInt(st.rows[idxs[k]], values[k], k < found.size() ? found[k] != 0 : true);
    }
}

void PollStringType(app::App &a, WatchState &st)
{
    std::vector<int>    ids;
    std::vector<size_t> idxs;
    for (size_t i = 0; i < st.rows.size(); ++i)
    {
        if (st.rows[i].type == VarType::VarcString)
        {
            ids.push_back(st.rows[i].id);
            idxs.push_back(i);
        }
    }
    if (ids.empty())
    {
        return;
    }
    std::vector<uint8_t> params;
    EncodeIds(params, ids);
    std::vector<uint8_t> reply;
    if (a.rpc.Call("get_varcs_string", params.data(), static_cast<uint32_t>(params.size()),
                   reply, kRpcTimeoutMs) != rpc::CallStatus::Ok)
    {
        return;
    }
    std::vector<std::string> values;
    std::vector<uint8_t>     found;
    mp::Reader r(reply.data(), reply.size());
    uint32_t n = 0;
    if (!r.ReadMapHeader(n))
    {
        return;
    }
    for (uint32_t i = 0; i < n; ++i)
    {
        const char *k = nullptr;
        uint32_t    kl = 0;
        if (!r.ReadString(k, kl))
        {
            break;
        }
        if (mp::StrEq(k, kl, "values"))      ReadStrArray(r, values);
        else if (mp::StrEq(k, kl, "found"))  ReadBoolArray(r, found);
        else                                 r.SkipValue();
    }
    for (size_t k = 0; k < idxs.size() && k < values.size(); ++k)
    {
        ApplyStr(st.rows[idxs[k]], values[k], k < found.size() ? found[k] != 0 : true);
    }
}

void Poll(app::App &a, WatchState &st)
{
    bool due = st.forcePoll;
    st.forcePoll = false;
    if (st.autoPoll)
    {
        st.pollAccum += ImGui::GetIO().DeltaTime;
        if (st.pollAccum >= st.pollIntervalSec)
        {
            st.pollAccum = 0.0f;
            due = true;
        }
    }
    if (!due || !a.rpc.IsConnected())
    {
        return;
    }
    PollIntType(a, st, VarType::Varp,    "get_varps");
    PollIntType(a, st, VarType::VarcInt, "get_varcs_int");
    PollStringType(a, st);
}

// --- event correlation (instant flash + varbit values) ---------------------

void ApplyEvent(WatchState &st, const wire::EventRecord &r)
{
    VarType t;
    switch (r.type)
    {
        case nxt::ipc::kEventVarChange:    t = VarType::Varp;    break;
        case nxt::ipc::kEventVarbitChange: t = VarType::Varbit;  break;
        case nxt::ipc::kEventVarcChange:   t = VarType::VarcInt; break;
        default: return;
    }
    if (r.bodyLen < sizeof(nxt::ipc::VarChangeBody))
    {
        return;
    }
    // VarChangeBody and VarbitChangeBody share the {id, old, new} layout.
    auto *b = reinterpret_cast<const nxt::ipc::VarChangeBody *>(r.body);
    for (auto &w : st.rows)
    {
        if (w.type == t && w.id == b->varpId)
        {
            ApplyInt(w, b->newValue, true);
        }
    }
}

void ScanEvents(app::App &a, WatchState &st)
{
    if (st.primedSeq)
    {
        for (const auto &r : a.eventBacklog)
        {
            if (r.seq > st.lastSeenSeq)
            {
                ApplyEvent(st, r);
            }
        }
    }
    if (!a.eventBacklog.empty())
    {
        st.lastSeenSeq = a.eventBacklog.back().seq;
    }
    st.primedSeq = true;
}

void DecayFlash(WatchState &st)
{
    float dt = ImGui::GetIO().DeltaTime;
    for (auto &w : st.rows)
    {
        if (w.flash > 0.0f)
        {
            w.flash = std::max(0.0f, w.flash - dt * kPollDecayPerSec);
        }
    }
}

void AddWatch(WatchState &st, VarType t, int id)
{
    for (auto &w : st.rows)
    {
        if (w.type == t && w.id == id)
        {
            return;   // already watching
        }
    }
    Watch w;
    w.type = t;
    w.id   = id;
    st.rows.push_back(std::move(w));
}

// --- rendering -------------------------------------------------------------

void DrawToolbar(WatchState &st)
{
    const float fs = ImGui::GetFontSize();
    ImGui::PushItemWidth(fs * 6.5f);
    ImGui::InputInt("##id", &st.addId);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ImGui::PushItemWidth(fs * 7.0f);
    ImGui::Combo("##type", &st.addType, "varp\0varbit\0varc-int\0varc-str\0");
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Add") && st.addId >= 0)
    {
        AddWatch(st, static_cast<VarType>(st.addType), st.addId);
    }

    ImGui::SameLine(0.0f, fs);
    theme::Toggle("##auto", &st.autoPoll);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::AlignTextToFramePadding();
    theme::Subheading("auto-poll");
    ImGui::SameLine(0.0f, fs);
    if (ImGui::Button("Poll now"))
    {
        st.forcePoll = true;
    }
}

void DrawValueCell(const Watch &w)
{
    if (!w.hasValue)
    {
        ImGui::TextDisabled(w.type == VarType::Varbit ? "(awaiting change)" : "…");
        return;
    }
    ImU32 base = w.found ? theme::kTextHi : theme::kTextDim;
    ImU32 col  = LerpColor(base, theme::kAccent, w.flash);
    char  buf[80];
    if (w.type == VarType::VarcString)
    {
        std::snprintf(buf, sizeof(buf), "\"%s\"", w.strValue.c_str());
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(w.value));
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
    ImGui::TextUnformatted(buf);
    ImGui::PopStyleColor();
}

void DrawRow(app::App &a, WatchState &st, size_t i)
{
    Watch &w = st.rows[i];
    ImGui::TableNextRow();
    ImGui::PushID(static_cast<int>(i));

    ImGui::TableSetColumnIndex(0);
    theme::Pill(TypeLabel(w.type),
                (TypeColor(w.type) & 0x00FFFFFFu) | (50u << 24), TypeColor(w.type));

    ImGui::TableSetColumnIndex(1);
    ImGui::Text("%d", w.id);

    ImGui::TableSetColumnIndex(2);
    const char *gt   = GamevalType(w.type);
    const char *name = gt ? a.gameval.NameForCacheType(gt, w.id) : nullptr;
    if (name)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kInfo));
        ImGui::TextUnformatted(name);
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::TextDisabled("—");
    }

    ImGui::TableSetColumnIndex(3);
    DrawValueCell(w);

    ImGui::TableSetColumnIndex(4);
    if (ImGui::SmallButton("remove"))
    {
        st.removeIdx = static_cast<int>(i);
    }

    ImGui::PopID();
}

void DrawTable(app::App &a, WatchState &st)
{
    const float fs = ImGui::GetFontSize();
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                | ImGuiTableFlags_BordersInnerH;
    if (!ImGui::BeginTable("watch", 5, flags))
    {
        return;
    }
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, fs * 5.5f);
    ImGui::TableSetupColumn("Id",   ImGuiTableColumnFlags_WidthFixed, fs * 4.0f);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, fs * 7.0f);
    ImGui::TableSetupColumn("",     ImGuiTableColumnFlags_WidthFixed, fs * 5.0f);
    ImGui::TableHeadersRow();

    for (size_t i = 0; i < st.rows.size(); ++i)
    {
        DrawRow(a, st, i);
    }
    ImGui::EndTable();

    if (st.removeIdx >= 0 && st.removeIdx < static_cast<int>(st.rows.size()))
    {
        st.rows.erase(st.rows.begin() + st.removeIdx);
    }
    st.removeIdx = -1;
}

}   // namespace

void DrawVarWatcher(app::App &a)
{
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34, ImGui::GetFontSize() * 28),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Var Watcher"))
    {
        ImGui::End();
        return;
    }

    static WatchState st;
    ScanEvents(a, st);
    Poll(a, st);
    DecayFlash(st);

    if (theme::BeginCard("vw.add", "WATCH"))
    {
        DrawToolbar(st);
        if (!a.rpc.IsConnected())
        {
            ImGui::Spacing();
            theme::Subheading("RPC pipe not connected — varp/varc values won't poll.");
        }
    }
    theme::EndCard();
    ImGui::Spacing();

    if (st.rows.empty())
    {
        theme::Subheading("Add a varp / varbit / varc id to start watching.");
        ImGui::End();
        return;
    }

    DrawTable(a, st);
    ImGui::End();
}

}   // namespace nxtdbg::panels
