#pragma once
#include "ipc/SharedLayout.h"

#include <cstddef>
#include <cstdint>

// Decoding for the open-interface list (wire v22 count/total, v23 flags).
//
// Deliberately free of ImGui and of attach::Session, so the wire test target
// can link it on its own and test exactly the code the Interfaces panel runs.

namespace nxtdbg::wire
{

// The client's raw SubInterface open type, as published in bits 0-2 of
// openIfaceFlags. Only the values observed live are named; 2, 4, 5 and 6 are
// valid on the wire but their meaning is not yet known.
inline constexpr uint8_t kOpenTypeModal   = 0;
inline constexpr uint8_t kOpenTypeOverlay = 1;
inline constexpr uint8_t kOpenTypeChild   = 3;

// One openIfaceFlags byte, split into its fields.
//
// type is the predicate: every decision about an interface (is it modal?)
// reads type and nothing else. isClientOpened is supplementary, shown to the
// user as "cs2" and never used to decide anything. reservedBits is bits 4-7,
// which the producer writes as 0; it is kept so a test can say so.
struct OpenIfaceFlags
{
    uint8_t type;
    bool    isClientOpened;
    uint8_t reservedBits;

    constexpr bool IsModal() const { return type == kOpenTypeModal; }
};

constexpr OpenIfaceFlags DecodeOpenIfaceFlags(uint8_t raw)
{
    return OpenIfaceFlags{
        static_cast<uint8_t>(raw & nxt::ipc::kOpenIfaceTypeMask),
        (raw & nxt::ipc::kOpenIfaceFlagClientOpened) != 0,
        static_cast<uint8_t>(raw >> 4),
    };
}

// The open-interface list of one snapshot. ids and flags point into that
// snapshot and are index-parallel; only [0, count) may be read.
struct OpenIfaceList
{
    const int32_t *ids;
    const uint8_t *flags;
    uint32_t       count;   // already clamped to kOpenIfaceCap
    uint32_t       total;   // the client table's own size (v22)
};

// Reads openIfaceCount exactly once and clamps it to the array, so a torn or
// hostile count can never index past either array, and ids and flags are
// always bounded by the same value. snap may be null: the list is then empty.
OpenIfaceList ReadOpenIfaces(const nxt::ipc::Snapshot *snap);

// "modal", "overlay", "child", "type 2", "type 4".."type 6", or "unknown" (7).
const char *OpenIfaceTypeName(uint8_t type);

// The suffix the panel prints after an id, without parentheses: "modal",
// "overlay", "child, cs2". Always NUL-terminates when cap > 0.
void FormatOpenIfaceType(OpenIfaceFlags flags, char *out, size_t cap);

}
