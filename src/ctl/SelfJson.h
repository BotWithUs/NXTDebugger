#pragma once
#include "ctl/JsonOut.h"
#include "ipc/SharedLayout.h"

// The self block as the control pipe reports it. Each is null when the frame has no
// local player (wire::HasLocalPlayer), so a client can never read a self at tile 0,0.
// Split from Control.cpp, which needs the whole App, so WireTests can drive the same
// functions the commands call.
namespace nxtdbg::ctl
{

JsonValue StatusPlayerJson(const nxt::ipc::Snapshot &s);  // status: "player"
JsonValue SceneSelfJson(const nxt::ipc::Snapshot &s);     // scene: "self"

}
