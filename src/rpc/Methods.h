#pragma once
#include <cstdint>

namespace nxtdbg::rpc
{

// Client-side parameter hints for the methods the console renders typed
// forms for. The method list itself comes from rpc.list_methods at runtime
// — this table only tells the UI *how* to render the form. Every method
// missing from the table falls back to a raw-JSON params input.
//
// Kept deliberately small (the eight or so "common" RPCs); extending it
// is one Method{} row + the matching params array.

enum class ParamKind : uint8_t
{
    Int,    // 64-bit signed; UI uses ImGui::InputInt
    Bool,
    Str,    // UTF-8 string; UI uses ImGui::InputText (256 chars)
};

struct ParamSpec
{
    const char *name;
    ParamKind   kind;
    int64_t     defaultInt;
    bool        defaultBool;
    const char *defaultStr;
};

struct MethodSpec
{
    const char       *name;
    const char       *summary;     // one-line UI hint
    const ParamSpec  *params;
    uint32_t          paramCount;
};

// Lookup by method name. Returns nullptr if the method has no schema —
// caller renders the raw-JSON fallback. nameLen is the UTF-8 byte length.
const MethodSpec *FindMethodSpec(const char *name, uint32_t nameLen);

// Full table, for the UI's "show curated forms only" toggle.
const MethodSpec *MethodSpecTable(uint32_t &countOut);

}
