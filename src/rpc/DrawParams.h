#pragma once
#include "rpc/MsgPack.h"
#include "rpc/RpcClient.h"

#include <cstdint>
#include <string>
#include <vector>

// Encoding and decoding helpers for the agent's debug_draw_* / highlight_*
// surface, shared by the Debug Draw panel (the hand driver) and the Layers
// sync (overlay/LayerSync). Lifted out of panels/DebugDrawPanel.cpp unchanged
// so there is one definition of each; nothing here touches ImGui or App.
namespace nxtdbg::rpc
{

// The wire packs colours 0xAARRGGBB; `rgba` is ImGui's float order.
uint32_t ArgbFromFloats(const float rgba[4]);

// ---------------------------------------------------------------------------
// Param builder.
//
// msgpack maps carry their entry count in the header, so the count has to be
// known before the first key is written. Collecting entries first and emitting
// once removes the class of bug where an optional param is added to the body
// and not to the count -- which produces a frame the agent parses as garbage.
// Strings and arrays are borrowed: they must outlive the emit.
// ---------------------------------------------------------------------------

struct ParamEntry
{
    const char    *name;
    enum class Kind : uint8_t { Int, Bool, Str, IntArray } kind;
    int64_t        asInt;
    bool           asBool;
    const char    *asStr;
    const int32_t *asArray;
    uint32_t       arrayLen;
};

struct ParamBuilder
{
    ParamEntry entries[32];
    uint32_t   count = 0;

    void AddInt(const char *name, int64_t v);
    void AddBool(const char *name, bool v);
    void AddStr(const char *name, const char *s);
    void AddIntArray(const char *name, const int32_t *arr, uint32_t n);
};

void WriteParamEntry(nxt::rpc::msgpack::Writer &w, const ParamEntry &e);

// Map header + every entry. Used both for a whole params map and for one item
// inside a batch's `items` array.
void WriteParamMap(nxt::rpc::msgpack::Writer &w, const ParamBuilder &b);

// Encode `b` as a standalone params map. False (and `out` empty) on overflow.
bool EmitParams(const ParamBuilder &b, std::vector<uint8_t> &out);

// ---------------------------------------------------------------------------
// Reply decoding
// ---------------------------------------------------------------------------

bool ReadStrInto(nxt::rpc::msgpack::Reader &r, std::string &out);

// Generic one-line renderer of any msgpack value ("{count: 3, dropped: 0}").
std::string ValueToText(nxt::rpc::msgpack::Reader &r);

const char *StatusText(CallStatus s);

}
