#include "Panels.h"

#include "app/App.h"
#include "app/Theme.h"
#include "wire/SnapshotReader.h"

#include "imgui.h"

#include <cstdio>

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

    char tickBuf[32];
    std::snprintf(tickBuf, sizeof(tickBuf), "%llu",
                  static_cast<unsigned long long>(snap.tickId));
    const auto alpha = static_cast<uint8_t>(160.0f + 95.0f * a.tickPulseT);
    ImU32 tickCol    = IM_COL32(0xFF, 0xAB, 0x47, alpha);
    theme::HeroStat("tickId", tickBuf, tickCol);

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

    DrawHero(a, *snap);
    ImGui::Spacing();
    DrawCounters(*snap);
    ImGui::Spacing();
    DrawProducer(*snap);
    ImGui::Spacing();
    DrawRaw(*snap);

    ImGui::End();
}

}
