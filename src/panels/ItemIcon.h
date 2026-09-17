#pragma once
#include "imgui.h"

#include <cstddef>
#include <unordered_set>

namespace nxtdbg::app { struct App; }
namespace nxtdbg::render { class TextureCache; }

namespace nxtdbg::panels
{

// Shared item-icon helpers, used by the Inventory panel and the Entities
// "Ground" tab. Item icons are software model renders pulled from NXTCache; an
// LRU TextureCache plus a per-frame decode budget keep a filled container or a
// drop-heavy scene from hitching the first time it appears — it fills in over a
// few frames instead.

// ImTextureID is an integer handle in this ImGui build; map the GL name through
// intptr_t for ImDrawList::AddImage.
ImTextureID ToTexId(unsigned int tex);

// RuneScape stack-size palette: amber under 100k, white up to 10m, green beyond.
ImU32 QtyColor(int qty);

// Compact stack-size text (e.g. "14K", "3M"); plain integer below 100k.
void FormatQty(int qty, char *out, size_t n);

// Draw an item icon into the `box`-sized square at screen pos `p0`. When the
// icon isn't cached and the budget allows, queue one decode this frame and draw
// the numeric id meanwhile; otherwise draw the id placeholder. `dim` fades a
// non-matching cell. Spends one unit of *ioBudget per decode it performs.
void DrawItemIcon(app::App &a, render::TextureCache &icons,
                  std::unordered_set<int> &ioFailed, int *ioBudget,
                  int itemId, ImVec2 p0, float box, bool dim);

// Bottom-right stack-size badge with a 1px shadow. No-op for qty <= 1
// (singletons read as "1" and don't need a badge).
void DrawQtyBadge(ImDrawList *dl, ImVec2 cellMax, int qty);

}
