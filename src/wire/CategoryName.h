#pragma once
#include <cstdint>

namespace nxtdbg::wire
{

// Display name for a component map's `category` value (WireCategory,
// PROTOCOL.md section 5). An unknown value from a newer agent renders as "?"
// rather than failing -- the enum is append-only.
const char *CategoryName(int32_t category);

}
