#pragma once
#include <cstdint>
#include <vector>

namespace nxtdbg::app
{

// One component's on-screen box, in game CLIENT-space pixels (the same space
// the agent's find_component_at hit-tests in, i.e. relative to the game
// window's client-area top-left). Published by the Interfaces panel — the sole
// writer — from the component tree it already fetched over RPC, and consumed
// read-only by the transparent OverlayWindow. Mirrors only the box-relevant
// fields of the panel's internal Comp, not its full identity/content set.
struct OverlayBox
{
    int32_t x        = 0;
    int32_t y        = 0;
    int32_t w        = 0;
    int32_t h        = 0;
    int32_t category = 0;    // WireCategory, drives the outline style
    int32_t comp     = -1;   // component id — label + hover match
    int32_t hidden   = -1;   // -1 unsupported (treat visible) / 0 visible / 1 hidden
    char    label[48] = {};  // precomputed "<comp> <Category>"
};

// Shared view-model of the currently selected interface. The Interfaces panel
// rewrites it every frame it draws; the overlay renders whatever is here. This
// is the whole data path between the two — the overlay opens no pipe and issues
// no RPC, it just paints the tree the panel already has. `rev` bumps on each
// rewrite so a consumer can cheaply tell fresh from stale.
struct InterfaceView
{
    int32_t                 iface      = -1;
    std::vector<OverlayBox> boxes;
    int32_t                 hoverComp  = -1;    // picked/selected comp id, -1 if none
    bool                    pickActive = false;
    uint64_t                rev        = 0;
};

}
