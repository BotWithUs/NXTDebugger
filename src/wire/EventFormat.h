#pragma once
#include "wire/EventReader.h"

#include <cstddef>

namespace nxtdbg::wire
{

// One-line human rendering of an event body ("id 1234  0 -> 5", "[2] Bob: hi"),
// exactly as the Events panel shows it. Shared by that panel and the control
// pipe's `events` command so the two never disagree. Always NUL-terminates
// `out`; an unknown type renders as "(<n> bytes)".
void FormatEventBody(const EventRecord &r, char *out, size_t n);

}
