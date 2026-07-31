#pragma once
#include <cstdint>

namespace nxt::ipc
{

// Stable, build-independent semantic category for an interface component.
//
// A component also carries a raw type byte, but that byte is assigned by the
// client and its meaning drifts between game builds — the same number denotes
// different kinds of component in different releases, so a consumer cannot
// switch on it and stay correct across an update. WireCategory is the permanent
// vocabulary the RPC layer emits instead: it appears as the `category` field of
// the component map returned by `get_component` / `get_interface_tree` /
// `find_component_at`, and consumer-side enums (Java `ComponentType`, and the
// .NET equivalent) mirror it one-for-one.
//
// These numeric values are a wire contract. Never renumber an existing entry —
// only append. The raw type byte is published alongside this for diagnostics;
// this is the durable, queryable form, and the one to write logic against.
enum class WireCategory : int32_t
{
    Unknown  = 0,
    Layer    = 1,
    Box      = 2,
    Text     = 3,
    Sprite   = 4,
    Model    = 5,
    Button   = 6,
    Divider  = 7,
    List     = 8,
    Input    = 9,
    Combo    = 10,
    Media    = 11,
    Tooltip  = 12,
    CrmView  = 13,
    Table    = 14,
    Cutscene = 15,
};

}
