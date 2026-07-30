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

constexpr int kHpSkillIdx = 3;  // RS3 stat slot for constitution

const char *SkillName(int typeId)
{
    static const char *names[] = {
        "Attack", "Defence", "Strength", "Constitution", "Range",
        "Prayer", "Magic", "Cooking", "Woodcut", "Fletch",
        "Fishing", "Firemaking", "Crafting", "Smithing", "Mining",
        "Herblore", "Agility", "Thieving", "Slayer", "Farming",
        "Runecraft", "Hunter", "Construct", "Summon", "Dungeon",
        "Divine", "Invention", "Archaeology", "Necromancy",
    };
    if (typeId >= 0 && typeId < int(sizeof(names) / sizeof(names[0])))
    {
        return names[typeId];
    }
    return "?";
}

// KeyLine for an optional id field, rendering "(none)" for the -1 sentinel the
// wire uses (followingIndex, spotAnimId).
void KeyLineId(const char *label, int id)
{
    char buf[24];
    if (id >= 0)
    {
        std::snprintf(buf, sizeof(buf), "%d", id);
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "(none)");
    }
    theme::KeyLine(label, buf);
}

void DrawNotAttached()
{
    if (!theme::BeginCard("ps.idle", "PLAYER", theme::kTextDim))
    {
        theme::EndCard();
        return;
    }
    theme::Subheading("Not attached to an agent.");
    theme::EndCard();
}

void DrawLocationCard(const nxt::ipc::LocalPlayer &self)
{
    if (!theme::BeginCard("ps.loc", "LOCATION"))
    {
        theme::EndCard();
        return;
    }
    char tile[64];
    std::snprintf(tile, sizeof(tile), "%d, %d", self.tileX, self.tileY);
    theme::HeroStat("world tile (x, y)", tile);
    ImGui::SameLine(0, ImGui::GetFontSize() * 2.5f);
    char plane[16];
    std::snprintf(plane, sizeof(plane), "%d", int(self.plane));
    theme::HeroStat("plane", plane, theme::kAccent);

    ImGui::Spacing();
    char idx[24];
    std::snprintf(idx, sizeof(idx), "%d", self.serverIndex);
    theme::KeyLine("server index", idx);
    theme::KeyLine("moving", (self.flags & nxt::ipc::kFlagMoving) ? "yes" : "no");
    theme::KeyLine("members", self.isMember ? "yes" : "no");
    KeyLineId("following", self.followingIndex);
    theme::EndCard();
}

void DrawCombatCard(const nxt::ipc::LocalPlayer &self)
{
    if (!theme::BeginCard("ps.combat", "COMBAT"))
    {
        theme::EndCard();
        return;
    }
    int hp    = 0;
    int hpMax = 0;
    for (uint32_t i = 0; i < self.skillCount && i < nxt::ipc::kSkillCap; ++i)
    {
        if (self.skills[i].typeId == kHpSkillIdx)
        {
            hp    = self.skills[i].boostedLevel;
            hpMax = self.skills[i].actualLevel;
            break;
        }
    }
    float frac = hpMax > 0 ? float(hp) / float(hpMax) : 0.0f;
    ImU32 col  = frac > 0.5f ? theme::kGood
               : frac > 0.2f ? theme::kWarn
                             : theme::kBad;

    char cap[32];
    std::snprintf(cap, sizeof(cap), "%d / %d", hp, hpMax);
    theme::ProgressStrip(frac, col, cap);
    ImGui::Spacing();

    char lvl[16];
    std::snprintf(lvl, sizeof(lvl), "%d", self.combatLevel);
    theme::KeyLine("combat level", lvl);

    char tgt[64];
    if (self.targetIndex >= 0)
    {
        std::snprintf(tgt, sizeof(tgt), "idx %d  type %d",
                      self.targetIndex, int(self.targetType));
    }
    else
    {
        std::snprintf(tgt, sizeof(tgt), "(none)");
    }
    theme::KeyLine("target", tgt);

    char anim[24];
    std::snprintf(anim, sizeof(anim), "%d", self.animationId);
    theme::KeyLine("animation", anim);

    char stance[24];
    std::snprintf(stance, sizeof(stance), "%d", self.stanceId);
    theme::KeyLine("stance", stance);
    KeyLineId("spot anim", self.spotAnimId);
    theme::EndCard();
}

void DrawSkillBar(const nxt::ipc::SkillEntry &sk)
{
    ImGui::PushID(sk.typeId);
    const float fs = ImGui::GetFontSize();
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::ColorConvertU32ToFloat4(theme::kTextHi));
    ImGui::TextUnformatted(SkillName(sk.typeId));
    ImGui::PopStyleColor();

    char val[32];
    std::snprintf(val, sizeof(val), "%d / %d",
                  sk.boostedLevel, sk.actualLevel);
    float frac = sk.actualLevel > 0
                     ? float(sk.boostedLevel) / float(sk.actualLevel)
                     : 0.0f;
    ImU32 col = sk.boostedLevel > sk.actualLevel ? theme::kAccent : theme::kInfo;
    theme::ProgressStrip(frac, col, val);

    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::ColorConvertU32ToFloat4(theme::kTextDim));
    ImGui::Text("xp %d", sk.experience);
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, fs * 0.3f));
    ImGui::PopID();
}

void DrawSkillsCard(const nxt::ipc::LocalPlayer &self)
{
    if (!theme::BeginCard("ps.skills", "SKILLS"))
    {
        theme::EndCard();
        return;
    }
    char cnt[24];
    std::snprintf(cnt, sizeof(cnt), "%u", self.skillCount);
    theme::KeyLine("count", cnt);
    theme::AccentRule();

    const float fs = ImGui::GetFontSize();
    int cols = 3;
    if (ImGui::GetContentRegionAvail().x < fs * 30) cols = 2;
    if (ImGui::GetContentRegionAvail().x < fs * 20) cols = 1;

    if (ImGui::BeginTable("skills", cols,
                          ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingStretchSame))
    {
        for (uint32_t i = 0; i < self.skillCount && i < nxt::ipc::kSkillCap; ++i)
        {
            if (i % cols == 0)
            {
                ImGui::TableNextRow();
            }
            ImGui::TableSetColumnIndex(int(i % cols));
            DrawSkillBar(self.skills[i]);
        }
        ImGui::EndTable();
    }
    theme::EndCard();
}

}

void DrawPlayerState(app::App &a)
{
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 38, ImGui::GetFontSize() * 36),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Player"))
    {
        ImGui::End();
        return;
    }

    const auto *snap = a.session.IsOpen()
                           ? wire::CurrentSnapshot(a.session)
                           : nullptr;
    if (!snap || snap->self.serverIndex < 0)
    {
        DrawNotAttached();
        ImGui::End();
        return;
    }

    DrawLocationCard(snap->self);
    ImGui::Spacing();
    DrawCombatCard(snap->self);
    ImGui::Spacing();
    DrawSkillsCard(snap->self);

    ImGui::End();
}

}
