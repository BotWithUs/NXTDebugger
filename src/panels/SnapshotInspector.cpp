#include "Panels.h"

#include "app/App.h"
#include "app/Theme.h"
#include "ipc/Events.h"
#include "rpc/MsgPack.h"
#include "rpc/RpcClient.h"
#include "wire/EventReader.h"
#include "wire/SnapshotReader.h"

#include "imgui.h"

#include <cstdio>
#include <vector>

namespace nxtdbg::panels
{

namespace
{

const char *GameStateName(int32_t s)
{
    switch (s)
    {
        case 0:  return "no_client";
        case 10: return "login";
        case 20: return "lobby";
        case 30: return "in_game";
        default: return "?";
    }
}

ImU32 GameStateColor(int32_t s)
{
    switch (s)
    {
        case 30: return theme::kGood;
        case 20: return theme::kInfo;
        case 10: return theme::kWarn;
        default: return theme::kTextDim;
    }
}

void DrawHero(app::App &a, const nxt::ipc::Snapshot &snap)
{
    if (!theme::BeginCard("snap.hero", "TICK"))
    {
        theme::EndCard();
        return;
    }
    const float fs = ImGui::GetFontSize();

    // Three clocks, three stats. serverTick leads because it is the one a script
    // author reasons in; publishSeq carries the pulse because it is the field that
    // moves on every republish.
    char tickBuf[32];
    std::snprintf(tickBuf, sizeof(tickBuf), "%d", snap.serverTick);
    theme::HeroStat("serverTick", tickBuf, IM_COL32(0x66, 0xBB, 0x6A, 0xFF));

    ImGui::SameLine(0, fs * 1.5f);
    char cycleBuf[32];
    std::snprintf(cycleBuf, sizeof(cycleBuf), "%d", snap.gameCycle);
    theme::HeroStat("gameCycle", cycleBuf, theme::kAccent);

    ImGui::SameLine(0, fs * 1.5f);
    char seqBuf[32];
    std::snprintf(seqBuf, sizeof(seqBuf), "%llu",
                  static_cast<unsigned long long>(snap.publishSeq));
    const auto alpha = static_cast<uint8_t>(160.0f + 95.0f * a.tickPulseT);
    ImU32 seqCol     = IM_COL32(0xFF, 0xAB, 0x47, alpha);
    theme::HeroStat("publishSeq", seqBuf, seqCol);

    ImGui::SameLine(0, fs * 2.0f);
    ImGui::BeginGroup();
    theme::Pill(GameStateName(snap.gameState),
                (GameStateColor(snap.gameState) & 0x00FFFFFFu) | (50u << 24),
                GameStateColor(snap.gameState));
    char ownBuf[24];
    std::snprintf(ownBuf, sizeof(ownBuf), "own #%d", snap.ownIndex);
    theme::Pill(ownBuf, theme::kAccentSoft, theme::kAccent);
    ImGui::EndGroup();

    theme::EndCard();
}

void DrawCounterTile(const char *label, uint32_t value, ImU32 col)
{
    ImGui::BeginGroup();
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%u", value);
    theme::HeroStat(label, buf, col);
    ImGui::EndGroup();
}

void DrawCounters(const nxt::ipc::Snapshot &snap)
{
    if (!theme::BeginCard("snap.cnt", "COUNTS"))
    {
        theme::EndCard();
        return;
    }
    const float fs = ImGui::GetFontSize();
    if (ImGui::BeginTable("cnt", 5, ImGuiTableFlags_SizingStretchSame))
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); DrawCounterTile("npcs",        snap.npcCount,       theme::kTextHi);
        ImGui::TableSetColumnIndex(1); DrawCounterTile("players",     snap.playerCount,    theme::kInfo);
        ImGui::TableSetColumnIndex(2); DrawCounterTile("locations",   snap.locationCount,  theme::kAccent);
        ImGui::TableSetColumnIndex(3); DrawCounterTile("inventories", snap.inventoryCount, theme::kGood);
        ImGui::TableSetColumnIndex(4); DrawCounterTile("inv items",   snap.invItemCount,   theme::kTextHi);
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0, fs * 0.2f));
    theme::EndCard();
}

void DrawProducer(const nxt::ipc::Snapshot &snap)
{
    if (!theme::BeginCard("snap.prod", "PRODUCER"))
    {
        theme::EndCard();
        return;
    }
    const auto &p = snap.producer;

    char qbuf[24];
    std::snprintf(qbuf, sizeof(qbuf), "%u", p.actionQueueSize);
    theme::KeyLine("action queue depth", qbuf);
    theme::KeyLine("actions blocked", p.actionsBlocked ? "yes" : "no");
    theme::KeyLine("on break",        p.onBreak        ? "yes" : "no");

    char last[32];
    std::snprintf(last, sizeof(last), "%llu",
                  static_cast<unsigned long long>(p.lastActionTimeMs));
    theme::KeyLine("last action time (ms)", last);

    if (p.breakUntilMs)
    {
        char br[32];
        std::snprintf(br, sizeof(br), "%llu",
                      static_cast<unsigned long long>(p.breakUntilMs));
        theme::KeyLine("break until (ms)", br);
    }

    char sv[16];
    std::snprintf(sv, sizeof(sv), "%u", p.sceneVersion);
    theme::KeyLine("scene version", sv);
    theme::EndCard();
}

void DrawRaw(const nxt::ipc::Snapshot &snap)
{
    if (!ImGui::CollapsingHeader("Raw fields"))
    {
        return;
    }
    char iface[16];
    std::snprintf(iface, sizeof(iface), "%d", snap.rootIfaceId);
    theme::KeyLine("rootIfaceId", iface);
}

// --- Action history (RPC) --------------------------------------------------
//
// The producer keeps a timestamped log the snapshot's ProducerState only
// summarises (depth + last-action-ms). get_action_history returns the full
// rows over the shared RPC client; refreshed on a timer and whenever an
// action_executed event flows past on the ring.

namespace mp = nxt::rpc::msgpack;

constexpr DWORD kActionRpcTimeoutMs = 1000;

struct ActionRow
{
    int64_t actionId  = 0;
    int64_t p1        = 0;
    int64_t p2        = 0;
    int64_t p3        = 0;
    int64_t timestamp = 0;
    int64_t delta     = 0;
};

struct ActionHistState
{
    std::vector<ActionRow> rows;
    float    pollAccum   = 0.0f;
    uint64_t lastSeenSeq = 0;
    bool     primedSeq   = false;
};

bool DecodeActionRow(mp::Reader &r, ActionRow &row)
{
    return mp::ForEachParam(r,
        [&](const char *k, uint32_t kl, mp::Reader &rr) -> bool
    {
        if (mp::StrEq(k, kl, "action_id"))
        {
            return rr.ReadInt(row.actionId);
        }
        if (mp::StrEq(k, kl, "param1"))
        {
            return rr.ReadInt(row.p1);
        }
        if (mp::StrEq(k, kl, "param2"))
        {
            return rr.ReadInt(row.p2);
        }
        if (mp::StrEq(k, kl, "param3"))
        {
            return rr.ReadInt(row.p3);
        }
        if (mp::StrEq(k, kl, "timestamp"))
        {
            return rr.ReadInt(row.timestamp);
        }
        if (mp::StrEq(k, kl, "delta"))
        {
            return rr.ReadInt(row.delta);
        }
        return false;
    });
}

void FetchActionHistory(app::App &a, ActionHistState &st)
{
    std::vector<uint8_t> params;
    params.assign(32, 0);
    mp::Writer w(params.data(), params.size());
    w.WriteMapHeader(1);
    w.WriteCStr("max_results");
    w.WriteInt(64);
    params.resize(w.BytesWritten());

    std::vector<uint8_t> reply;
    if (a.rpc.Call("get_action_history", params.data(), static_cast<uint32_t>(params.size()),
                   reply, kActionRpcTimeoutMs) != rpc::CallStatus::Ok)
    {
        return;
    }
    mp::Reader r(reply.data(), reply.size());
    uint32_t   n = 0;
    if (!r.ReadArrayHeader(n))   // get_action_history result is a top-level array
    {
        return;
    }
    st.rows.clear();
    for (uint32_t i = 0; i < n; ++i)
    {
        ActionRow row{};
        if (!DecodeActionRow(r, row))
        {
            break;
        }
        st.rows.push_back(row);
    }
}

bool ActionEventsArrived(app::App &a, ActionHistState &st)
{
    bool arrived = false;
    if (st.primedSeq)
    {
        for (const auto &r : a.eventBacklog)
        {
            if (r.seq > st.lastSeenSeq && r.type == nxt::ipc::kEventActionExecuted)
            {
                arrived = true;
            }
        }
    }
    if (!a.eventBacklog.empty())
    {
        st.lastSeenSeq = a.eventBacklog.back().seq;
    }
    st.primedSeq = true;
    return arrived;
}

void UpdateActionHistory(app::App &a, ActionHistState &st)
{
    bool fired = ActionEventsArrived(a, st);
    st.pollAccum += ImGui::GetIO().DeltaTime;
    bool due = fired || st.pollAccum >= 1.0f;
    if (!due)
    {
        return;
    }
    st.pollAccum = 0.0f;
    if (a.rpc.IsConnected())
    {
        FetchActionHistory(a, st);
    }
}

void DrawActionRows(const ActionHistState &st)
{
    const float fs = ImGui::GetFontSize();
    if (!ImGui::BeginTable("acth", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                    | ImGuiTableFlags_BordersInnerH, ImVec2(0, fs * 11.0f)))
    {
        return;
    }
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, fs * 5.0f);
    ImGui::TableSetupColumn("Params", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Δms",    ImGuiTableColumnFlags_WidthFixed, fs * 5.0f);
    ImGui::TableHeadersRow();
    for (const auto &row : st.rows)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kAccent));
        ImGui::Text("%lld", static_cast<long long>(row.actionId));
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%lld, %lld, %lld", static_cast<long long>(row.p1),
                    static_cast<long long>(row.p2), static_cast<long long>(row.p3));
        ImGui::TableSetColumnIndex(2);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kTextDim));
        ImGui::Text("%lld", static_cast<long long>(row.delta));
        ImGui::PopStyleColor();
    }
    ImGui::EndTable();
}

void DrawActionHistory(app::App &a, const ActionHistState &st)
{
    if (!theme::BeginCard("snap.actions", "ACTION HISTORY"))
    {
        theme::EndCard();
        return;
    }
    if (!a.rpc.IsConnected())
    {
        theme::Subheading("RPC pipe not connected — action history needs the pipe.");
    }
    else if (st.rows.empty())
    {
        theme::Subheading("No actions executed yet.");
    }
    else
    {
        DrawActionRows(st);
    }
    theme::EndCard();
}

}

void DrawSnapshotInspector(app::App &a)
{
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 46, 0),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Snapshot"))
    {
        ImGui::End();
        return;
    }

    const auto *snap = a.session.IsOpen()
                           ? wire::CurrentSnapshot(a.session)
                           : nullptr;
    if (!snap)
    {
        theme::Subheading("Attach an agent to inspect snapshots.");
        ImGui::End();
        return;
    }

    static ActionHistState actionHist;
    UpdateActionHistory(a, actionHist);

    DrawHero(a, *snap);
    ImGui::Spacing();
    DrawCounters(*snap);
    ImGui::Spacing();
    DrawProducer(*snap);
    ImGui::Spacing();
    DrawActionHistory(a, actionHist);
    ImGui::Spacing();
    DrawRaw(*snap);

    ImGui::End();
}

}
