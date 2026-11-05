#pragma once

// Decimal text of the msgpack integer the reader is positioned on, for code that
// renders arbitrary values.
//
// Reader::ReadInt refuses a uint64 above INT64_MAX without advancing, and the agent
// really sends those (last_seed in the humanizer stats reply). A renderer that only
// calls ReadInt prints a wrong number and then misreads everything after it, so every
// generic renderer comes through here.

#include "rpc/MsgPack.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>

namespace nxtdbg::rpc
{

// "?" for a value truncated by the end of the frame; the reader is then not advanced,
// which is how a caller's walk of the rest of that frame already ends.
inline std::string ReadIntText(nxt::rpc::msgpack::Reader &reader)
{
    char buffer[32];
    int64_t signedValue = 0;
    if (reader.ReadInt(signedValue))
    {
        std::snprintf(buffer, sizeof(buffer), "%" PRId64, signedValue);
        return buffer;
    }
    uint64_t unsignedValue = 0;
    if (reader.ReadUInt(unsignedValue))
    {
        std::snprintf(buffer, sizeof(buffer), "%" PRIu64, unsignedValue);
        return buffer;
    }
    return "?";
}

} // namespace nxtdbg::rpc
