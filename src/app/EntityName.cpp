#include "EntityName.h"

#include "App.h"

#include <cstring>

namespace nxtdbg::app
{

std::string EntityName(App &a, const char *type, int id)
{
    if (id < 0) return {};
    std::string name;
    if (a.cache.IsOpen())
    {
        std::string json = a.cache.GetJson(type, id);
        const char *p = json.empty() ? nullptr : std::strstr(json.c_str(), "\"name\"");
        if (p && (p = std::strchr(p, ':')) != nullptr)
        {
            ++p;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p == '"')
            {
                ++p;
                if (const char *q = std::strchr(p, '"'))
                {
                    name.assign(p, size_t(q - p));
                }
            }
        }
    }
    if (name.empty())
    {
        // Fallback: the cache has no display name (locs especially, and any
        // entity when the cache isn't open) -- use the bundled gameval name.
        if (const char *gv = a.gameval.NameForCacheType(type, id))
        {
            name = gv;
        }
    }
    return name;
}

}
