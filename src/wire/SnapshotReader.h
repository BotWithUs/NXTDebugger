#pragma once
#include "attach/Session.h"
#include "ipc/SharedLayout.h"

namespace nxtdbg::wire
{

// Returns the currently-front snapshot pointer, or nullptr if not attached.
// The view is valid only for the current frame — copy fields you need; do not
// hold across frames (the producer may flip frontIdx between calls).
const nxt::ipc::Snapshot *CurrentSnapshot(const attach::Session &s);

}
