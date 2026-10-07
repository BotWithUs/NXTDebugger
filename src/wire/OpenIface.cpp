#include "OpenIface.h"

#include <algorithm>
#include <cstdio>

namespace nxtdbg::wire
{

OpenIfaceList ReadOpenIfaces(const nxt::ipc::Snapshot *snap)
{
    if (!snap)
    {
        return OpenIfaceList{nullptr, nullptr, 0, 0};
    }
    const uint32_t rawCount = snap->openIfaceCount;
    return OpenIfaceList{
        snap->openIfaces,
        snap->openIfaceFlags,
        std::min(rawCount, nxt::ipc::kOpenIfaceCap),
        snap->openIfaceTotal,
    };
}

const char *OpenIfaceTypeName(uint8_t type)
{
    switch (type)
    {
    case kOpenTypeModal:
        return "modal";
    case kOpenTypeOverlay:
        return "overlay";
    case kOpenTypeChild:
        return "child";
    case 2:
        return "type 2";
    case 4:
        return "type 4";
    case 5:
        return "type 5";
    case 6:
        return "type 6";
    default:
        return "unknown";
    }
}

void FormatOpenIfaceType(OpenIfaceFlags flags, char *out, size_t cap)
{
    if (!out || cap == 0)
    {
        return;
    }
    std::snprintf(out, cap, "%s%s", OpenIfaceTypeName(flags.type),
                  flags.isClientOpened ? ", cs2" : "");
}

}
