#include "Methods.h"

#include <cstring>

namespace nxtdbg::rpc
{

namespace
{

constexpr ParamSpec kNoParams[] = { { nullptr, ParamKind::Int, 0, false, nullptr } };

// ----- Curated typed forms ------------------------------------------------
//
// Eight methods cover the common debugging workflows: liveness ping and
// client count, the two real state probes (cycle + login), single-key reads
// (varp + component), the screen-pick component lookup, and the action-queue
// enqueue. Everything else falls back to the raw-JSON params input — that
// path always works, just without typed widgets.

constexpr ParamSpec kVarpParams[] = {
    { "id",    ParamKind::Int, 0, false, nullptr },
};

constexpr ParamSpec kComponentParams[] = {
    { "iface_id",   ParamKind::Int, 1477, false, nullptr },
    { "comp_index", ParamKind::Int, 0,    false, nullptr },
};

constexpr ParamSpec kFindAtParams[] = {
    { "screen_x", ParamKind::Int, 0, false, nullptr },
    { "screen_y", ParamKind::Int, 0, false, nullptr },
};

constexpr ParamSpec kQueueActionParams[] = {
    { "action_id", ParamKind::Int, 57, false, nullptr },
    { "param1",    ParamKind::Int, 0,  false, nullptr },
    { "param2",    ParamKind::Int, 0,  false, nullptr },
    { "param3",    ParamKind::Int, 0,  false, nullptr },
};

constexpr MethodSpec kMethods[] = {
    { "rpc.ping",         "Dispatcher liveness check.",         kNoParams,        0 },
    { "rpc.client_count", "How many clients on this pipe.",     kNoParams,        0 },
    { "get_game_cycle",   "TransmissionManager::client_cycle.", kNoParams,        0 },
    { "get_login_state",  "Game state + login progress/status.", kNoParams,       0 },
    { "get_varp",         "Read a varp by id.",                 kVarpParams,      1 },
    { "get_component",    "Single component by (iface, index).", kComponentParams, 2 },
    { "find_component_at","Deepest visible component at a screen pixel.", kFindAtParams, 2 },
    { "queue_action",     "Enqueue a game action.",             kQueueActionParams, 4 },
};

constexpr uint32_t kMethodCount = sizeof(kMethods) / sizeof(kMethods[0]);

bool NameEq(const char *a, uint32_t aLen, const char *zlit)
{
    if (!zlit) return false;
    uint32_t i = 0;
    while (i < aLen && zlit[i])
    {
        if (a[i] != zlit[i]) return false;
        ++i;
    }
    return i == aLen && zlit[i] == '\0';
}

}

const MethodSpec *FindMethodSpec(const char *name, uint32_t nameLen)
{
    for (uint32_t i = 0; i < kMethodCount; ++i)
    {
        if (NameEq(name, nameLen, kMethods[i].name))
        {
            return &kMethods[i];
        }
    }
    return nullptr;
}

const MethodSpec *MethodSpecTable(uint32_t &countOut)
{
    countOut = kMethodCount;
    return kMethods;
}

}
