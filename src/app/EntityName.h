#pragma once
#include <string>

namespace nxtdbg::app
{

struct App;

// Display name for a cache-typed id ("npc", "loc", "item"): the cache
// definition's `name` when the cache is open and has one, else the bundled
// gameval symbol, else empty. Shared by the Entities panel and the overlay
// layers so both label a thing the same way.
std::string EntityName(App &a, const char *type, int id);

}
