#pragma once
#include "cache/Json.h"

#include <cstdint>
#include <string>

// Builders and a compact serializer over cache::JsonValue -- the one JSON DOM
// this project already has. The control pipe speaks line-delimited JSON, so
// every response is built as a JsonValue and written on one line.
//
// Shared by nxt_debugger.exe and nxtdbgctl.exe; depends on nothing but
// cache/Json, so the client can link it without the rest of the debugger.
namespace nxtdbg::ctl
{

using cache::JsonType;
using cache::JsonValue;

JsonValue JNull();
JsonValue JBool(bool v);
JsonValue JInt(int64_t v);
JsonValue JDouble(double v);
JsonValue JStr(std::string v);
JsonValue JStr(const char *v);    // nullptr -> null
JsonValue JArr();
JsonValue JObj();

// Append a key to an object. No de-duplication: callers build each object once.
void Put(JsonValue &obj, std::string key, JsonValue v);
void Push(JsonValue &arr, JsonValue v);

// Compact single-line JSON. Invalid UTF-8 bytes in strings are emitted as
// \u00XX (treated as Latin-1) so the output is always valid JSON whatever the
// game put in a component's text. NaN / infinities become null.
void        WriteJson(const JsonValue &v, std::string &out);
std::string ToJson(const JsonValue &v);

}
