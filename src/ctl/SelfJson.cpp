#include "ctl/SelfJson.h"

#include "wire/LocalPlayer.h"

#include <algorithm>
#include <cstdint>

namespace nxtdbg::ctl
{

JsonValue StatusPlayerJson(const nxt::ipc::Snapshot &s)
{
    if (!wire::HasLocalPlayer(s))
    {
        return JNull();
    }
    const nxt::ipc::LocalPlayer &p = s.self;
    JsonValue player = JObj();
    Put(player, "server_index", JInt(p.serverIndex));
    Put(player, "tile_x", JInt(p.tileX));
    Put(player, "tile_y", JInt(p.tileY));
    Put(player, "plane", JInt(p.plane));
    Put(player, "combat_level", JInt(p.combatLevel));
    Put(player, "animation", JInt(p.animationId));
    Put(player, "orientation", JInt(p.orientation));
    Put(player, "target_index", JInt(p.targetIndex));
    Put(player, "flags", JInt(p.flags));
    return player;
}

JsonValue SceneSelfJson(const nxt::ipc::Snapshot &s)
{
    if (!wire::HasLocalPlayer(s))
    {
        return JNull();
    }
    constexpr int32_t kConstitution = 3;
    constexpr int32_t kPrayer       = 5;
    const nxt::ipc::LocalPlayer &p = s.self;
    JsonValue self = JObj();
    Put(self, "x", JInt(p.tileX));
    Put(self, "y", JInt(p.tileY));
    Put(self, "plane", JInt(p.plane));
    Put(self, "anim", JInt(p.animationId));
    Put(self, "spot_anim", JInt(p.spotAnimId));
    Put(self, "target", JInt(p.targetIndex));
    const uint32_t skills = std::min<uint32_t>(p.skillCount, nxt::ipc::kSkillCap);
    for (uint32_t i = 0; i < skills; ++i)
    {
        const nxt::ipc::SkillEntry &k = p.skills[i];
        if (k.typeId == kConstitution)
        {
            Put(self, "constitution", JInt(k.boostedLevel));
        }
        else if (k.typeId == kPrayer)
        {
            Put(self, "prayer", JInt(k.boostedLevel));
        }
    }
    return self;
}

}
