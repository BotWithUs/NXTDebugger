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

constexpr DWORD  kRpcTimeoutMs   = 1500;
constexpr size_t kTimelineCap    = 12;

// One token-lifecycle event kept for the panel's mini timeline. `aux` carries
// the body's seconds field (remaining-at-fire / until-expiry); -1 for the
// body-less "failed".
struct TokenEvt
{
    uint64_t seq;
    uint32_t type;
    int64_t  aux;
};

struct SessionState
{
    bool     enabled   = false;
    int64_t  expirySec = 0;
    int64_t  secsUntil = 0;
    bool     inFlight  = false;
    int64_t  grace     = 0;
    int64_t  jitter    = 0;
    bool     haveData  = false;

    float    pollAccum       = 0.0f;
    float    pollIntervalSec  = 1.0f;

    uint64_t lastSeenSeq      = 0;
    bool     primedSeq        = false;
    uint64_t lastFailedSeq    = 0;
    uint64_t lastRefreshedSeq = 0;
    std::vector<TokenEvt> timeline;

    std::string triggerMsg;
    float       triggerFlash = 0.0f;
};

// --- RPC -------------------------------------------------------------------

void EncodeEmpty(std::vector<uint8_t> &out)
{
    out.assign(8, 0);
    mp::Writer w(out.data(), out.size());
    w.WriteMapHeader(0);
    out.resize(w.BytesWritten());
}

void PollToken(app::App &a, SessionState &st)
{
    std::vector<uint8_t> params;
    EncodeEmpty(params);
    std::vector<uint8_t> reply;
    if (a.rpc.Call("get_token_refresher", params.data(), static_cast<uint32_t>(params.size()),
                   reply, kRpcTimeoutMs) != rpc::CallStatus::Ok)
    {
        return;
    }
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
        if      (mp::StrEq(k, kl, "enabled"))              r.ReadBool(st.enabled);
        else if (mp::StrEq(k, kl, "expiry_sec"))           r.ReadInt(st.expirySec);
        else if (mp::StrEq(k, kl, "seconds_until_expiry")) r.ReadInt(st.secsUntil);
        else if (mp::StrEq(k, kl, "refresh_in_flight"))    r.ReadBool(st.inFlight);
        else if (mp::StrEq(k, kl, "grace_seconds"))        r.ReadInt(st.grace);
        else if (mp::StrEq(k, kl, "jitter_seconds"))       r.ReadInt(st.jitter);
        else                                               r.SkipValue();
    }
    st.haveData = true;
}

void TriggerRefresh(app::App &a, SessionState &st)
{
    std::vector<uint8_t> params;
    EncodeEmpty(params);
    std::vector<uint8_t> reply;
    if (a.rpc.Call("trigger_token_refresh", params.data(), static_cast<uint32_t>(params.size()),
                   reply, kRpcTimeoutMs) != rpc::CallStatus::Ok)
    {
        st.triggerMsg  = "trigger failed (no reply)";
        st.triggerFlash = 1.0f;
        return;
    }
    bool        fired = false;
    std::string reason;
    mp::Reader  r(reply.data(), reply.size());
    uint32_t    n = 0;
    if (r.ReadMapHeader(n))
    {
        for (uint32_t i = 0; i < n; ++i)
        {
            const char *k = nullptr;
            uint32_t    kl = 0;
            if (!r.ReadString(k, kl))
            {
                break;
            }
            if (mp::StrEq(k, kl, "fired"))
            {
                r.ReadBool(fired);
            }
            else if (mp::StrEq(k, kl, "reason"))
            {
                const char *s = nullptr;
                uint32_t    sl = 0;
                if (r.ReadString(s, sl))
                {
                    reason.assign(s, sl);
                }
            }
            else
            {
                r.SkipValue();
            }
        }
    }
    if (fired)
    {
        st.triggerMsg = "refresh fired";
    }
    else
    {
        st.triggerMsg = reason.empty() ? "declined" : ("declined — " + reason);
    }
    st.triggerFlash = 1.0f;
}

void Poll(app::App &a, SessionState &st)
{
    if (st.triggerFlash > 0.0f)
    {
        st.triggerFlash = (std::max)(0.0f, st.triggerFlash - ImGui::GetIO().DeltaTime * 0.4f);
    }
    if (!a.rpc.IsConnected())
    {
        st.haveData = false;
        return;
    }
    st.pollAccum += ImGui::GetIO().DeltaTime;
    if (st.pollAccum < st.pollIntervalSec)
    {
        return;
    }
    st.pollAccum = 0.0f;
    PollToken(a, st);
}

// --- event timeline --------------------------------------------------------

int64_t TokenAux(const wire::EventRecord &r)
{
    if (r.type == nxt::ipc::kEventTokenRefreshFired
        && r.bodyLen >= sizeof(nxt::ipc::TokenRefreshFiredBody))
    {
        return reinterpret_cast<const nxt::ipc::TokenRefreshFiredBody *>(r.body)->secondsRemainingAtFire;
    }
    if (r.type == nxt::ipc::kEventTokenRefreshed
        && r.bodyLen >= sizeof(nxt::ipc::TokenRefreshedBody))
    {
        return reinterpret_cast<const nxt::ipc::TokenRefreshedBody *>(r.body)->secondsUntilExpiry;
    }
    return -1;
}

void ApplyTokenEvent(SessionState &st, const wire::EventRecord &r)
{
    if (r.type != nxt::ipc::kEventTokenRefreshFired
        && r.type != nxt::ipc::kEventTokenRefreshed
        && r.type != nxt::ipc::kEventTokenRefreshFailed)
    {
        return;
    }
    if (r.type == nxt::ipc::kEventTokenRefreshFailed)
    {
        st.lastFailedSeq = r.seq;
    }
    else if (r.type == nxt::ipc::kEventTokenRefreshed)
    {
        st.lastRefreshedSeq = r.seq;
    }
    st.timeline.push_back({ r.seq, r.type, TokenAux(r) });
    if (st.timeline.size() > kTimelineCap)
    {
        st.timeline.erase(st.timeline.begin());
    }
}

void ScanEvents(app::App &a, SessionState &st)
{
    if (st.primedSeq)
    {
        for (const auto &r : a.eventBacklog)
        {
            if (r.seq > st.lastSeenSeq)
            {
                ApplyTokenEvent(st, r);
            }
        }
    }
    if (!a.eventBacklog.empty())
    {
        st.lastSeenSeq = a.eventBacklog.back().seq;
    }
    st.primedSeq = true;
}

// --- status derivation -----------------------------------------------------

struct Status
{
    const char *label;
    ImU32       color;
    bool        alive;
};

Status DeriveStatus(const SessionState &st)
{
    if (!st.haveData)
    {
        return { "no data", theme::kTextDim, false };
    }
    if (!st.enabled)
    {
        return { "disabled", theme::kTextDim, false };
    }
    if (st.inFlight)
    {
        return { "refreshing", theme::kWarn, true };
    }
    if (st.lastFailedSeq > st.lastRefreshedSeq && st.lastFailedSeq != 0)
    {
        return { "last refresh failed", theme::kBad, false };
    }
    if (st.expirySec != 0 && st.secsUntil <= 0)
    {
        return { "expired", theme::kBad, false };
    }
    return { "valid", theme::kGood, true };
}

void FormatCountdown(int64_t secs, char *out, size_t n)
{
    bool    neg = secs < 0;
    int64_t s   = neg ? -secs : secs;
    std::snprintf(out, n, "%s%lld:%02lld", neg ? "-" : "",
                  static_cast<long long>(s / 60), static_cast<long long>(s % 60));
}

// --- rendering -------------------------------------------------------------

void DrawTokenCard(app::App &a, SessionState &st)
{
    if (!theme::BeginCard("sh.token", "ACCESS TOKEN"))
    {
        theme::EndCard();
        return;
    }
    Status status = DeriveStatus(st);
    theme::StatusDot(status.color, status.alive, a.dotPhase);
    ImGui::SameLine();
    theme::Pill(status.label, (status.color & 0x00FFFFFFu) | (50u << 24), status.color);

    ImGui::Spacing();
    char cd[24];
    FormatCountdown(st.secsUntil, cd, sizeof(cd));
    theme::HeroStat("expires in (m:s)", st.haveData ? cd : "—",
                    st.secsUntil <= 60 && st.haveData ? theme::kWarn : theme::kTextHi);

    ImGui::Spacing();
    char buf[32];
    theme::KeyLine("enabled", st.enabled ? "yes" : "no");
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(st.expirySec));
    theme::KeyLine("expiry (epoch s)", buf);
    std::snprintf(buf, sizeof(buf), "%llds", static_cast<long long>(st.grace));
    theme::KeyLine("grace", buf);
    std::snprintf(buf, sizeof(buf), "%llds", static_cast<long long>(st.jitter));
    theme::KeyLine("jitter", buf);

    ImGui::Spacing();
    if (ImGui::Button("Trigger refresh now") && a.rpc.IsConnected())
    {
        TriggerRefresh(a, st);
    }
    if (st.triggerFlash > 0.0f && !st.triggerMsg.empty())
    {
        ImGui::SameLine();
        ImU32 c = (theme::kAccent & 0x00FFFFFFu)
                | (static_cast<uint32_t>(220.0f * st.triggerFlash) << 24);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(c));
        ImGui::TextUnformatted(st.triggerMsg.c_str());
        ImGui::PopStyleColor();
    }
    theme::EndCard();
}

const char *TokenEvtLabel(uint32_t type)
{
    switch (type)
    {
        case nxt::ipc::kEventTokenRefreshFired:  return "fired";
        case nxt::ipc::kEventTokenRefreshed:     return "refreshed";
        case nxt::ipc::kEventTokenRefreshFailed: return "failed";
    }
    return "?";
}

ImU32 TokenEvtColor(uint32_t type)
{
    switch (type)
    {
        case nxt::ipc::kEventTokenRefreshFired:  return theme::kWarn;
        case nxt::ipc::kEventTokenRefreshed:     return theme::kGood;
        case nxt::ipc::kEventTokenRefreshFailed: return theme::kBad;
    }
    return theme::kTextDim;
}

void DrawTimelineCard(SessionState &st)
{
    if (!theme::BeginCard("sh.timeline", "REFRESH TIMELINE", theme::kInfo, true))
    {
        theme::EndCard();
        return;
    }
    if (st.timeline.empty())
    {
        theme::Subheading("No token-refresh events seen yet this session.");
        theme::EndCard();
        return;
    }
    if (ImGui::BeginTable("tl", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                 | ImGuiTableFlags_BordersInnerH))
    {
        const float fs = ImGui::GetFontSize();
        ImGui::TableSetupColumn("Seq",   ImGuiTableColumnFlags_WidthFixed, fs * 6.0f);
        ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthFixed, fs * 7.0f);
        ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (auto it = st.timeline.rbegin(); it != st.timeline.rend(); ++it)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextDim));
            ImGui::Text("%llu", static_cast<unsigned long long>(it->seq));
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(1);
            theme::Pill(TokenEvtLabel(it->type),
                        (TokenEvtColor(it->type) & 0x00FFFFFFu) | (50u << 24),
                        TokenEvtColor(it->type));
            ImGui::TableSetColumnIndex(2);
            if (it->aux >= 0)
            {
                ImGui::Text("%lld s", static_cast<long long>(it->aux));
            }
            else
            {
                ImGui::TextDisabled("—");
            }
        }
        ImGui::EndTable();
    }
    theme::EndCard();
}

}   // namespace

void DrawSessionHealth(app::App &a)
{
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30, ImGui::GetFontSize() * 34),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Session"))
    {
        ImGui::End();
        return;
    }

    static SessionState st;
    ScanEvents(a, st);
    Poll(a, st);

    if (!a.session.IsOpen())
    {
        theme::Subheading("Attach an agent to read session/token health.");
        ImGui::End();
        return;
    }

    DrawTokenCard(a, st);
    ImGui::Spacing();
    DrawTimelineCard(st);
    ImGui::End();
}

}   // namespace nxtdbg::panels
