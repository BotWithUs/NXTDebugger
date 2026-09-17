#include "Panels.h"

#include "app/App.h"
#include "app/Theme.h"
#include "rpc/MsgPack.h"
#include "rpc/TapClient.h"

#include "imgui.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nxtdbg::panels
{

namespace
{

// Wire topic + payload mirror — keep in sync with
// JBotWithUsV2/core/.../impl/ScriptContextChannel.java::TOPIC.
constexpr const char *kTopic = "script.context";

constexpr size_t kStateRingCap = 200;     // last N state transitions across all scripts
constexpr size_t kTraceRingCap = 5000;    // last N trace lines across all scripts
constexpr int    kCellTruncCap = 96;

// ---------------------------------------------------------------------------
// One-line msgpack pretty-printer — same shape as RpcTapPanel's. Kept local
// rather than shared because each panel evolves its own truncation rules and
// the helper is one screenful.
// ---------------------------------------------------------------------------

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
    if (len == 0) return "(nil)";
    nxt::rpc::msgpack::Reader r(bytes, len);
    std::string out;
    out.reserve(static_cast<size_t>(budget));
    AppendValue(out, r, budget);
    return out;
}

// ---------------------------------------------------------------------------
// Entry types — one parsed row per kind. The reader thread parses; the UI
// thread renders. Strings are eagerly populated so scroll doesn't re-parse.
// ---------------------------------------------------------------------------

struct StateEntry
{
    std::string script;
    std::string connection;
    int64_t     t_us = 0;
    std::string state;
    std::string detail;
};

struct TraceEntry
{
    std::string script;
    std::string connection;
    int64_t     t_us = 0;
    std::string level;
    std::string message;
};

struct AnnotationValue
{
    std::string script;
    std::string connection;
    int64_t     t_us = 0;
    std::string oneLine;
};

// One slot per (script, key). New annotation overwrites the prior value —
// annotations are "current value" not "history".
struct AnnotationKey
{
    std::string script;
    std::string key;
    bool operator==(const AnnotationKey &o) const noexcept
    {
        return script == o.script && key == o.key;
    }
};

struct AnnotationKeyHash
{
    size_t operator()(const AnnotationKey &k) const noexcept
    {
        // Cheap combined hash — std::hash<string> twice + xor.
        return std::hash<std::string>{}(k.script) * 31u
             ^ std::hash<std::string>{}(k.key);
    }
};

// ---------------------------------------------------------------------------
// Reader-side parser — runs on the TapClient thread, never touches ImGui.
// Returns true if the kind was recognised; unknown kinds drop silently.
// ---------------------------------------------------------------------------

bool ReadStringField(nxt::rpc::msgpack::Reader &r, std::string &out)
{
    const char *s = nullptr; uint32_t n = 0;
    if (!r.ReadString(s, n)) return false;
    out.assign(s, n);
    return true;
}

bool ReadIntField(nxt::rpc::msgpack::Reader &r, int64_t &out)
{
    return r.ReadInt(out);
}

// Decode {kind, script, connection?, t_us, ...kind-specific...}. The state /
// trace / annotation outputs are passed in by reference; whichever kind the
// payload matches is filled. Returns the kind enum.
enum class Kind : uint8_t { Unknown, State, Trace, Annotation };

struct DecodedPayload
{
    Kind             kind = Kind::Unknown;
    StateEntry       stateE;
    TraceEntry       traceE;
    AnnotationKey    annKey;
    AnnotationValue  annVal;
};

bool DecodePayload(const uint8_t *bytes, uint32_t len, DecodedPayload &out)
{
    nxt::rpc::msgpack::Reader r(bytes, len);
    uint32_t mc;
    if (!r.ReadMapHeader(mc)) return false;

    std::string script;
    std::string connection;
    int64_t     t_us = 0;
    std::string kindStr;
    std::string detail;
    std::string state;
    std::string level;
    std::string message;
    std::string key;
    const uint8_t *valuePtr = nullptr;
    uint32_t       valueLen = 0;

    for (uint32_t i = 0; i < mc; ++i)
    {
        const char *k = nullptr;
        uint32_t kLen = 0;
        if (!r.ReadString(k, kLen)) return false;

        if      (nxt::rpc::msgpack::StrEq(k, kLen, "script"))     { if (!ReadStringField(r, script))     return false; }
        else if (nxt::rpc::msgpack::StrEq(k, kLen, "connection")) { if (!ReadStringField(r, connection)) return false; }
        else if (nxt::rpc::msgpack::StrEq(k, kLen, "t_us"))       { if (!ReadIntField(r, t_us))          return false; }
        else if (nxt::rpc::msgpack::StrEq(k, kLen, "kind"))       { if (!ReadStringField(r, kindStr))    return false; }
        else if (nxt::rpc::msgpack::StrEq(k, kLen, "state"))      { if (!ReadStringField(r, state))      return false; }
        else if (nxt::rpc::msgpack::StrEq(k, kLen, "detail"))     { if (!ReadStringField(r, detail))     return false; }
        else if (nxt::rpc::msgpack::StrEq(k, kLen, "level"))      { if (!ReadStringField(r, level))      return false; }
        else if (nxt::rpc::msgpack::StrEq(k, kLen, "message"))    { if (!ReadStringField(r, message))    return false; }
        else if (nxt::rpc::msgpack::StrEq(k, kLen, "key"))        { if (!ReadStringField(r, key))        return false; }
        else if (nxt::rpc::msgpack::StrEq(k, kLen, "value"))
        {
            const size_t start = r.Pos();
            if (!r.SkipValue()) return false;
            valuePtr = bytes + start;
            valueLen = static_cast<uint32_t>(r.Pos() - start);
        }
        else
        {
            if (!r.SkipValue()) return false;
        }
    }

    if      (kindStr == "state")      out.kind = Kind::State;
    else if (kindStr == "trace")      out.kind = Kind::Trace;
    else if (kindStr == "annotation") out.kind = Kind::Annotation;
    else                              return false;

    switch (out.kind)
    {
    case Kind::State:
        out.stateE.script     = std::move(script);
        out.stateE.connection = std::move(connection);
        out.stateE.t_us       = t_us;
        out.stateE.state      = std::move(state);
        out.stateE.detail     = std::move(detail);
        break;
    case Kind::Trace:
        out.traceE.script     = std::move(script);
        out.traceE.connection = std::move(connection);
        out.traceE.t_us       = t_us;
        out.traceE.level      = std::move(level);
        out.traceE.message    = std::move(message);
        break;
    case Kind::Annotation:
        out.annKey.script        = script;       // copy — annKey + annVal both need script
        out.annKey.key           = std::move(key);
        out.annVal.script        = std::move(script);
        out.annVal.connection    = std::move(connection);
        out.annVal.t_us          = t_us;
        out.annVal.oneLine       = valuePtr
            ? PrettyOneLine(valuePtr, valueLen, kCellTruncCap)
            : std::string("nil");
        break;
    default:
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Panel state — function-static so it survives across frames and across
// attach cycles.
// ---------------------------------------------------------------------------

struct PanelState
{
    std::mutex                                   mx;
    std::deque<StateEntry>                       states;
    std::deque<TraceEntry>                       traces;
    std::unordered_map<AnnotationKey,
                       AnnotationValue,
                       AnnotationKeyHash>        annotations;
    DWORD                                        lastSubscribedPid = 0;
    bool                                         paused            = false;
    bool                                         autoScroll        = true;
    char                                         filter[64]        = {};
    uint64_t                                     totalReceived     = 0;
};

PanelState &State()
{
    static PanelState s;
    return s;
}

void OnPayload(PanelState &s, const uint8_t *data, uint32_t len)
{
    DecodedPayload d;
    if (!DecodePayload(data, len, d)) return;

    std::lock_guard<std::mutex> lk(s.mx);
    ++s.totalReceived;
    if (s.paused) return;

    switch (d.kind)
    {
    case Kind::State:
        if (s.states.size() >= kStateRingCap) s.states.pop_front();
        s.states.push_back(std::move(d.stateE));
        break;
    case Kind::Trace:
        if (s.traces.size() >= kTraceRingCap) s.traces.pop_front();
        s.traces.push_back(std::move(d.traceE));
        break;
    case Kind::Annotation:
        s.annotations[d.annKey] = std::move(d.annVal);
        break;
    default:
        break;
    }
}

void EnsureSubscription(app::App &a, PanelState &s)
{
    DWORD pid = a.tapConnectedPid;
    if (pid == s.lastSubscribedPid && (pid == 0 || a.tap.IsConnected())) return;

    if (pid != s.lastSubscribedPid)
    {
        {
            std::lock_guard<std::mutex> lk(s.mx);
            s.states.clear();
            s.traces.clear();
            s.annotations.clear();
            s.totalReceived = 0;
        }
        s.lastSubscribedPid = pid;
    }
    if (pid != 0 && a.tap.IsConnected())
    {
        a.tap.Subscribe(kTopic,
            [&s](const uint8_t *data, uint32_t len) { OnPayload(s, data, len); });
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

bool ContainsCI(const std::string &haystack, const char *needle)
{
    if (!needle || needle[0] == 0) return true;
    auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    };
    const size_t hLen = haystack.size();
    const size_t nLen = std::strlen(needle);
    if (nLen > hLen) return false;
    for (size_t i = 0; i + nLen <= hLen; ++i)
    {
        bool ok = true;
        for (size_t j = 0; j < nLen; ++j)
        {
            if (lower(haystack[i + j]) != lower(needle[j])) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

ImU32 StateColor(const std::string &state)
{
    if (state == "RUNNING")   return theme::kGood;
    if (state == "STARTING")  return theme::kInfo;
    if (state == "STOPPED")   return theme::kTextDim;
    if (state == "CRASHED")   return theme::kBad;
    // Stop-escalation states: the host's watchdog flags a script that won't
    // leave onLoop, then cuts it off, then writes it off. Produced by
    // JBotWithUsV2's ScriptRunner alongside the four above.
    if (state == "STALLED")   return theme::kWarn;
    if (state == "REVOKED")   return theme::kBad;
    if (state == "ABANDONED") return theme::kBad;
    return theme::kText;
}

ImU32 LevelColor(const std::string &level)
{
    if (level == "ERROR") return theme::kBad;
    if (level == "WARN")  return theme::kWarn;
    if (level == "DEBUG") return theme::kTextDim;
    return theme::kText;
}

void DrawControlBar(app::App &a, PanelState &s, size_t stateN, size_t traceN, size_t annN)
{
    if (!theme::BeginCard("scriptctx.bar", "SCRIPT.CONTEXT"))
    {
        theme::EndCard();
        return;
    }

    rpc::TapStatus st = a.tap.LastStatus();
    theme::StatusDot(StatusColor(st),
                     st == rpc::TapStatus::Subscribed, a.dotPhase);
    ImGui::SameLine();
    theme::Pill(StatusText(st), (StatusColor(st) & 0x00FFFFFFu) | (50u << 24),
                StatusColor(st));

    ImGui::SameLine(0, ImGui::GetFontSize() * 1.5f);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%zu / %zu / %zu",
                  stateN, traceN, annN);
    theme::HeroStat("state / trace / ann", buf, theme::kTextHi);

    ImGui::SameLine(0, ImGui::GetFontSize() * 1.5f);
    {
        std::lock_guard<std::mutex> lk(s.mx);
        std::snprintf(buf, sizeof(buf), "%llu",
                      static_cast<unsigned long long>(s.totalReceived));
    }
    theme::HeroStat("total", buf, theme::kTextDim);

    ImGui::SameLine(0, ImGui::GetFontSize() * 2);
    ImGui::BeginGroup();
    theme::Subheading("paused");
    theme::Toggle("##scxpp", &s.paused);
    ImGui::EndGroup();

    ImGui::SameLine(0, ImGui::GetFontSize() * 1.5f);
    ImGui::BeginGroup();
    theme::Subheading("auto-scroll");
    theme::Toggle("##scxas", &s.autoScroll);
    ImGui::EndGroup();

    ImGui::SameLine(0, ImGui::GetFontSize() * 1.5f);
    if (ImGui::Button("Clear"))
    {
        std::lock_guard<std::mutex> lk(s.mx);
        s.states.clear();
        s.traces.clear();
        s.annotations.clear();
    }

    ImGui::SameLine(0, ImGui::GetFontSize());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::InputTextWithHint("##scxfilter", "filter script",
                             s.filter, sizeof(s.filter));

    theme::EndCard();
}

void DrawStateTimeline(PanelState &s)
{
    if (!theme::BeginCard("scriptctx.state", "STATE TIMELINE"))
    {
        theme::EndCard();
        return;
    }
    const float fs = ImGui::GetFontSize();
    if (ImGui::BeginTable("scxstates", 4,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable,
        ImVec2(0, fs * 10)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Script",     ImGuiTableColumnFlags_WidthFixed, fs * 16);
        ImGui::TableSetupColumn("Connection", ImGuiTableColumnFlags_WidthFixed, fs * 12);
        ImGui::TableSetupColumn("State",      ImGuiTableColumnFlags_WidthFixed, fs * 9);
        ImGui::TableSetupColumn("Detail",     ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        std::lock_guard<std::mutex> lk(s.mx);
        for (const auto &e : s.states)
        {
            if (!ContainsCI(e.script, s.filter)) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(e.script.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::ColorConvertU32ToFloat4(theme::kTextDim));
            ImGui::TextUnformatted(e.connection.c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(2);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::ColorConvertU32ToFloat4(StateColor(e.state)));
            ImGui::TextUnformatted(e.state.c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(e.detail.c_str());
        }
        if (s.autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() -
                                                    ImGui::GetFontSize())
        {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndTable();
    }
    theme::EndCard();
}

void DrawTraceStream(PanelState &s)
{
    if (!theme::BeginCard("scriptctx.trace", "TRACE STREAM"))
    {
        theme::EndCard();
        return;
    }
    const float fs = ImGui::GetFontSize();
    if (ImGui::BeginTable("scxtraces", 4,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable,
        ImVec2(0, fs * 14)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Script",  ImGuiTableColumnFlags_WidthFixed, fs * 16);
        ImGui::TableSetupColumn("Level",   ImGuiTableColumnFlags_WidthFixed, fs * 5);
        ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("μs",      ImGuiTableColumnFlags_WidthFixed, fs * 11);
        ImGui::TableHeadersRow();

        std::lock_guard<std::mutex> lk(s.mx);
        for (const auto &e : s.traces)
        {
            if (!ContainsCI(e.script, s.filter)) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(e.script.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::ColorConvertU32ToFloat4(LevelColor(e.level)));
            ImGui::TextUnformatted(e.level.c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(e.message.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::ColorConvertU32ToFloat4(theme::kTextDim));
            ImGui::Text("%lld", static_cast<long long>(e.t_us));
            ImGui::PopStyleColor();
        }
        if (s.autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() -
                                                    ImGui::GetFontSize())
        {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndTable();
    }
    theme::EndCard();
}

void DrawAnnotationTable(PanelState &s)
{
    if (!theme::BeginCard("scriptctx.ann", "ANNOTATIONS"))
    {
        theme::EndCard();
        return;
    }
    const float fs = ImGui::GetFontSize();
    if (ImGui::BeginTable("scxann", 4,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable,
        ImVec2(0, fs * 10)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Script", ImGuiTableColumnFlags_WidthFixed, fs * 16);
        ImGui::TableSetupColumn("Key",    ImGuiTableColumnFlags_WidthFixed, fs * 14);
        ImGui::TableSetupColumn("Value",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("μs",     ImGuiTableColumnFlags_WidthFixed, fs * 11);
        ImGui::TableHeadersRow();

        std::lock_guard<std::mutex> lk(s.mx);
        for (const auto &kv : s.annotations)
        {
            const auto &k = kv.first;
            const auto &v = kv.second;
            if (!ContainsCI(k.script, s.filter)) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(k.script.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::ColorConvertU32ToFloat4(theme::kTextHi));
            ImGui::TextUnformatted(k.key.c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(v.oneLine.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::ColorConvertU32ToFloat4(theme::kTextDim));
            ImGui::Text("%lld", static_cast<long long>(v.t_us));
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }
    theme::EndCard();
}

}

void DrawScriptContext(app::App &a)
{
    ImGui::SetNextWindowSize(
        ImVec2(ImGui::GetFontSize() * 72, ImGui::GetFontSize() * 40),
        ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Script Context"))
    {
        ImGui::End();
        return;
    }

    PanelState &s = State();
    EnsureSubscription(a, s);

    size_t stateN, traceN, annN;
    {
        std::lock_guard<std::mutex> lk(s.mx);
        stateN = s.states.size();
        traceN = s.traces.size();
        annN   = s.annotations.size();
    }

    DrawControlBar(a, s, stateN, traceN, annN);
    ImGui::Spacing();
    DrawStateTimeline(s);
    ImGui::Spacing();
    DrawTraceStream(s);
    ImGui::Spacing();
    DrawAnnotationTable(s);

    ImGui::End();
}

}
