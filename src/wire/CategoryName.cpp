#include "CategoryName.h"

#include "ipc/WireCategory.h"

namespace nxtdbg::wire
{

const char *CategoryName(int32_t cat)
{
    using nxt::ipc::WireCategory;
    switch (static_cast<WireCategory>(cat))
    {
    case WireCategory::Unknown:  return "Unknown";
    case WireCategory::Layer:    return "Layer";
    case WireCategory::Box:      return "Box";
    case WireCategory::Text:     return "Text";
    case WireCategory::Sprite:   return "Sprite";
    case WireCategory::Model:    return "Model";
    case WireCategory::Button:   return "Button";
    case WireCategory::Divider:  return "Divider";
    case WireCategory::List:     return "List";
    case WireCategory::Input:    return "Input";
    case WireCategory::Combo:    return "Combo";
    case WireCategory::Media:    return "Media";
    case WireCategory::Tooltip:  return "Tooltip";
    case WireCategory::CrmView:  return "CrmView";
    case WireCategory::Table:    return "Table";
    case WireCategory::Cutscene: return "Cutscene";
    }
    return "?";
}

}
