#pragma once
#include "ctl/JsonOut.h"

#include <cstdint>
#include <vector>

// Lossless-enough bridges between the agent's msgpack and the control pipe's
// JSON. Generic on purpose: an RPC reply is converted field-for-field, so a key
// the agent adds tomorrow reaches the control client without a code change
// here. Bin values become lowercase hex strings (nothing on the query surface
// returns bin today).
namespace nxtdbg::ctl
{

// Decode one msgpack value. False on malformed / truncated input or nesting
// past msgpack::kMaxNesting; outValue is unspecified on false.
bool MsgPackToJson(const uint8_t *data, size_t len, JsonValue &outValue);
bool MsgPackToJson(const std::vector<uint8_t> &bytes, JsonValue &outValue);

// Encode a JSON value as msgpack (ints stay ints, doubles become float64).
// False only if the value is absurdly large (> 4 MiB encoded).
bool JsonToMsgPack(const JsonValue &value, std::vector<uint8_t> &outBytes);

}
