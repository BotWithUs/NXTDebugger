#include "Json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nxtdbg::cache
{

namespace
{

struct Cursor
{
    const char *p;
    const char *end;
};

bool ParseValue(Cursor &c, JsonValue &out);

void SkipWs(Cursor &c)
{
    while (c.p < c.end)
    {
        char ch = *c.p;
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')
        {
            ++c.p;
        }
        else
        {
            break;
        }
    }
}

bool Matches(Cursor &c, const char *lit)
{
    size_t n = std::strlen(lit);
    if (static_cast<size_t>(c.end - c.p) < n)
    {
        return false;
    }
    if (std::strncmp(c.p, lit, n) != 0)
    {
        return false;
    }
    c.p += n;
    return true;
}

void AppendUtf8(std::string &out, uint32_t cp)
{
    if (cp <= 0x7F)
    {
        out.push_back(static_cast<char>(cp));
    }
    else if (cp <= 0x7FF)
    {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp <= 0xFFFF)
    {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool ParseHex4(Cursor &c, uint32_t &out)
{
    if (c.end - c.p < 4)
    {
        return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
    {
        char ch = c.p[i];
        v <<= 4;
        if (ch >= '0' && ch <= '9')
        {
            v |= static_cast<uint32_t>(ch - '0');
        }
        else if (ch >= 'a' && ch <= 'f')
        {
            v |= static_cast<uint32_t>(ch - 'a' + 10);
        }
        else if (ch >= 'A' && ch <= 'F')
        {
            v |= static_cast<uint32_t>(ch - 'A' + 10);
        }
        else
        {
            return false;
        }
    }
    c.p += 4;
    out = v;
    return true;
}

bool ParseEscape(Cursor &c, std::string &out)
{
    char ch = *c.p++;   // the char after the backslash
    switch (ch)
    {
    case '"':  out.push_back('"');  return true;
    case '\\': out.push_back('\\'); return true;
    case '/':  out.push_back('/');  return true;
    case 'b':  out.push_back('\b'); return true;
    case 'f':  out.push_back('\f'); return true;
    case 'n':  out.push_back('\n'); return true;
    case 'r':  out.push_back('\r'); return true;
    case 't':  out.push_back('\t'); return true;
    case 'u':
    {
        uint32_t cp = 0;
        if (!ParseHex4(c, cp))
        {
            return false;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF && c.end - c.p >= 6
            && c.p[0] == '\\' && c.p[1] == 'u')
        {
            c.p += 2;
            uint32_t lo = 0;
            if (!ParseHex4(c, lo))
            {
                return false;
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        }
        AppendUtf8(out, cp);
        return true;
    }
    default:
        return false;
    }
}

bool ParseStringRaw(Cursor &c, std::string &out)
{
    ++c.p;   // opening quote
    while (c.p < c.end)
    {
        char ch = *c.p++;
        if (ch == '"')
        {
            return true;
        }
        if (ch == '\\')
        {
            if (c.p >= c.end || !ParseEscape(c, out))
            {
                return false;
            }
        }
        else
        {
            out.push_back(ch);
        }
    }
    return false;
}

bool ParseNumber(Cursor &c, JsonValue &out)
{
    const char *start = c.p;
    bool isDouble = false;
    if (c.p < c.end && (*c.p == '-' || *c.p == '+'))
    {
        ++c.p;
    }
    while (c.p < c.end)
    {
        char ch = *c.p;
        if (ch >= '0' && ch <= '9')
        {
            ++c.p;
        }
        else if (ch == '.' || ch == 'e' || ch == 'E' || ch == '+' || ch == '-')
        {
            if (ch == '.' || ch == 'e' || ch == 'E')
            {
                isDouble = true;
            }
            ++c.p;
        }
        else
        {
            break;
        }
    }
    if (c.p == start)
    {
        return false;
    }
    std::string tok(start, c.p);
    if (isDouble)
    {
        out.type   = JsonType::Double;
        out.dblVal = std::strtod(tok.c_str(), nullptr);
    }
    else
    {
        out.type   = JsonType::Int;
        out.intVal = std::strtoll(tok.c_str(), nullptr, 10);
    }
    return true;
}

bool ParseArray(Cursor &c, JsonValue &out)
{
    out.type = JsonType::Array;
    ++c.p;   // '['
    SkipWs(c);
    if (c.p < c.end && *c.p == ']')
    {
        ++c.p;
        return true;
    }
    while (c.p < c.end)
    {
        JsonValue v;
        if (!ParseValue(c, v))
        {
            return false;
        }
        out.arrVal.push_back(std::move(v));
        SkipWs(c);
        if (c.p >= c.end)
        {
            return false;
        }
        if (*c.p == ',')
        {
            ++c.p;
            continue;
        }
        if (*c.p == ']')
        {
            ++c.p;
            return true;
        }
        return false;
    }
    return false;
}

bool ParseObject(Cursor &c, JsonValue &out)
{
    out.type = JsonType::Object;
    ++c.p;   // '{'
    SkipWs(c);
    if (c.p < c.end && *c.p == '}')
    {
        ++c.p;
        return true;
    }
    while (c.p < c.end)
    {
        SkipWs(c);
        if (c.p >= c.end || *c.p != '"')
        {
            return false;
        }
        std::string key;
        if (!ParseStringRaw(c, key))
        {
            return false;
        }
        SkipWs(c);
        if (c.p >= c.end || *c.p != ':')
        {
            return false;
        }
        ++c.p;
        JsonValue v;
        if (!ParseValue(c, v))
        {
            return false;
        }
        out.objVal.emplace_back(std::move(key), std::move(v));
        SkipWs(c);
        if (c.p >= c.end)
        {
            return false;
        }
        if (*c.p == ',')
        {
            ++c.p;
            continue;
        }
        if (*c.p == '}')
        {
            ++c.p;
            return true;
        }
        return false;
    }
    return false;
}

bool ParseValue(Cursor &c, JsonValue &out)
{
    SkipWs(c);
    if (c.p >= c.end)
    {
        return false;
    }
    switch (*c.p)
    {
    case '{':
        return ParseObject(c, out);
    case '[':
        return ParseArray(c, out);
    case '"':
        out.type = JsonType::String;
        return ParseStringRaw(c, out.strVal);
    case 't':
        if (!Matches(c, "true"))
        {
            return false;
        }
        out.type    = JsonType::Bool;
        out.boolVal = true;
        return true;
    case 'f':
        if (!Matches(c, "false"))
        {
            return false;
        }
        out.type    = JsonType::Bool;
        out.boolVal = false;
        return true;
    case 'n':
        if (!Matches(c, "null"))
        {
            return false;
        }
        out.type = JsonType::Null;
        return true;
    default:
        return ParseNumber(c, out);
    }
}

}

const JsonValue *JsonValue::Find(std::string_view key) const
{
    if (type != JsonType::Object)
    {
        return nullptr;
    }
    for (const auto &kv : objVal)
    {
        if (kv.first.size() == key.size()
            && std::memcmp(kv.first.data(), key.data(), key.size()) == 0)
        {
            return &kv.second;
        }
    }
    return nullptr;
}

int64_t JsonValue::AsInt(int64_t def) const
{
    switch (type)
    {
    case JsonType::Int:    return intVal;
    case JsonType::Double: return static_cast<int64_t>(dblVal);
    case JsonType::Bool:   return boolVal ? 1 : 0;
    default:               return def;
    }
}

double JsonValue::AsDouble(double def) const
{
    switch (type)
    {
    case JsonType::Double: return dblVal;
    case JsonType::Int:    return static_cast<double>(intVal);
    default:               return def;
    }
}

bool JsonValue::AsBool(bool def) const
{
    switch (type)
    {
    case JsonType::Bool: return boolVal;
    case JsonType::Int:  return intVal != 0;
    default:             return def;
    }
}

std::string JsonValue::AsString() const
{
    char buf[40];
    switch (type)
    {
    case JsonType::String:
        return strVal;
    case JsonType::Int:
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(intVal));
        return buf;
    case JsonType::Double:
        std::snprintf(buf, sizeof(buf), "%g", dblVal);
        return buf;
    case JsonType::Bool:
        return boolVal ? "true" : "false";
    case JsonType::Null:
        return "null";
    default:
        return {};
    }
}

bool ParseJson(std::string_view text, JsonValue &outValue)
{
    outValue = JsonValue{};
    Cursor c{ text.data(), text.data() + text.size() };
    if (!ParseValue(c, outValue))
    {
        outValue = JsonValue{};
        return false;
    }
    SkipWs(c);
    if (c.p != c.end)
    {
        outValue = JsonValue{};
        return false;
    }
    return true;
}

}
