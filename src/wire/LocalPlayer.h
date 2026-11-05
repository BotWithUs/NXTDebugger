#pragma once
#include "ipc/SharedLayout.h"

namespace nxtdbg::wire
{

// Whether this frame has a local player. It takes both tests. The agent publishes
// ownIndex -1 whenever there is no player, including at gameState 30 before the
// client's logged-in player resolves, so state 30 alone is not enough. A frame from an
// agent before the pre-publish fix can be all zeros, so ownIndex alone is not enough
// either. Never use self.serverIndex: it is -1 outside the world on a current agent
// and 0 on an older one, and it says nothing about whether a session is attached.
inline bool HasLocalPlayer(const nxt::ipc::Snapshot &s) noexcept
{
    constexpr int32_t kGameStateInGame = 30;
    return s.gameState == kGameStateInGame && s.ownIndex >= 0;
}

// What the Player panel should draw. Pure, so WireTests can drive the panel's own
// decision without ImGui. snap is wire::CurrentSnapshot's result: null means there is no
// attached session, and nothing else does.
enum class PlayerPanelState
{
    NotAttached,
    NoLocalPlayer,
    InWorld,
};

inline PlayerPanelState ClassifyPlayerPanel(const nxt::ipc::Snapshot *snap) noexcept
{
    if (snap == nullptr)
    {
        return PlayerPanelState::NotAttached;
    }
    return HasLocalPlayer(*snap) ? PlayerPanelState::InWorld : PlayerPanelState::NoLocalPlayer;
}

}
