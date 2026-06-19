#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxtdbg::cache
{

enum class JsonType
{
    Null,
    Bool,
    Int,
    Double,
    String,
    Array,
    Object,
};

// A tiny tagged DOM node for the UTF-8 JSON the NXTCache C ABI emits. Objects
// preserve insertion order so the detail view renders fields the way the
// decoder serialised them. Deliberately not a variant — a flat tagged struct
// keeps the switch-based readers in panels simple under cpp-rules.
struct JsonValue
{
    JsonType    type    = JsonType::Null;
    bool        boolVal = false;
    int64_t     intVal  = 0;
    double      dblVal  = 0.0;
    std::string strVal;
    std::vector<JsonValue> arrVal;
    std::vector<std::pair<std::string, JsonValue>> objVal;

    bool IsNull()   const { return type == JsonType::Null; }
    bool IsBool()   const { return type == JsonType::Bool; }
    bool IsNumber() const { return type == JsonType::Int || type == JsonType::Double; }
    bool IsString() const { return type == JsonType::String; }
    bool IsArray()  const { return type == JsonType::Array; }
    bool IsObject() const { return type == JsonType::Object; }

    // Object field lookup by key. Returns nullptr when not an object or absent.
    const JsonValue *Find(std::string_view key) const;

    // Scalar coercions — safe on any node; return the default on type mismatch.
    int64_t     AsInt(int64_t def = 0) const;
    double      AsDouble(double def = 0.0) const;
    bool        AsBool(bool def = false) const;
    std::string AsString() const;   // scalars stringified; containers -> empty
};

// Parse a UTF-8 JSON document. Returns false (and leaves outValue = Null) on
// malformed input or trailing non-whitespace.
bool ParseJson(std::string_view text, JsonValue &outValue);

}
