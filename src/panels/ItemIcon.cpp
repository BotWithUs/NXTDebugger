#include "ItemIcon.h"

#include "app/App.h"
#include "app/Theme.h"
#include "render/Texture.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace nxtdbg::panels
{

ImTextureID ToTexId(unsigned int tex)
{
    return (ImTextureID)(intptr_t)tex;
}

ImU32 QtyColor(int qty)
{
    if (qty >= 10000000)
    {
        return theme::kGood;
    }
    if (qty >= 100000)
    {
        return theme::kTextHi;
    }
    return theme::kAccent;
}

void FormatQty(int qty, char *out, size_t n)
{
    if (qty >= 10000000)
    {
        std::snprintf(out, n, "%dM", qty / 1000000);
    }
    else if (qty >= 100000)
    {
        std::snprintf(out, n, "%dK", qty / 1000);
    }
    else
    {
        std::snprintf(out, n, "%d", qty);
    }
}

void DrawItemIcon(app::App &a, render::TextureCache &icons,
                  std::unordered_set<int> &ioFailed, int *ioBudget,
                  int itemId, ImVec2 p0, float box, bool dim)
{
    if (!icons.Contains(itemId)
        && ioFailed.find(itemId) == ioFailed.end()
        && *ioBudget > 0)
    {
        --*ioBudget;
        std::vector<uint8_t> rgba;
        if (a.cache.RenderItemIcon(itemId, 64, 64, 4, rgba) && rgba.size() == 64u * 64u * 4u)
        {
            icons.Put(itemId, rgba.data(), 64, 64);
        }
        else
        {
            ioFailed.insert(itemId);
        }
    }

    ImDrawList  *dl  = ImGui::GetWindowDrawList();
    unsigned int tex = icons.Get(itemId);
    if (tex != 0)
    {
        const float pad  = box * 0.08f;
        const ImU32 tint = dim ? IM_COL32(255, 255, 255, 64) : IM_COL32(255, 255, 255, 255);
        dl->AddImage(ToTexId(tex), ImVec2(p0.x + pad, p0.y + pad),
                     ImVec2(p0.x + box - pad, p0.y + box - pad),
                     ImVec2(0, 0), ImVec2(1, 1), tint);
    }
    else
    {
        char idb[12];
        std::snprintf(idb, sizeof(idb), "%d", itemId);
        ImVec2 ts = ImGui::CalcTextSize(idb);
        dl->AddText(ImVec2(p0.x + (box - ts.x) * 0.5f, p0.y + (box - ts.y) * 0.5f),
                    dim ? theme::kTextDim : theme::kText, idb);
    }
}

void DrawQtyBadge(ImDrawList *dl, ImVec2 cellMax, int qty)
{
    if (qty <= 1)
    {
        return;
    }
    char buf[16];
    FormatQty(qty, buf, sizeof(buf));
    ImVec2 ts = ImGui::CalcTextSize(buf);
    ImVec2 tp(cellMax.x - ts.x - 3.0f, cellMax.y - ts.y - 2.0f);
    dl->AddText(ImVec2(tp.x + 1.0f, tp.y + 1.0f), IM_COL32(0, 0, 0, 200), buf);
    dl->AddText(tp, QtyColor(qty), buf);
}

}
