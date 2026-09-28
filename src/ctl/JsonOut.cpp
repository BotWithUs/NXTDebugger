#include "JsonOut.h"

#include <cmath>
#include <cstdio>
#include <utility>

namespace nxtdbg::ctl
{

namespace
{

void AppendEscapedByte(std::string &out, unsigned char c)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
    out.append(buf);
}

// Length of the valid UTF-8 sequence starting at s[i], or 0 if it is invalid.
size_t Utf8SeqLen(const std::string &s, size_t i)
{
    const auto lead = static_cast<unsigned char>(s[i]);
    size_t     n    = 0;
    if (lead >= 0xC2 && lead <= 0xDF)
    {
        n = 2;
    }
    else if (lead >= 0xE0 && lead <= 0xEF)
    {
        n = 3;
    }
    else if (lead >= 0xF0 && lead <= 0xF4)
    {
        n = 4;
    }
    else
    {
        return 0;
    }
    if (i + n > s.size())
    {
        return 0;
    }
    for (size_t k = 1; k < n; ++k)
    {
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
        {
            return 0;
        }
    }
    return n;
}

void AppendString(std::string &out, const std::string &s)
{
    out.push_back('"');
    size_t i = 0;
    while (i < s.size())
    {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c == '"' || c == '\\')
        {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        if (c < 0x20)
        {
            switch (c)
            {
                case '\n': out.append("\\n"); break;
                case '\r': out.append("\\r"); break;
                case '\t': out.append("\\t"); break;
                default:   AppendEscapedByte(out, c); break;
            }
            ++i;
            continue;
        }
        if (c < 0x80)
        {
            out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }
        const size_t n = Utf8SeqLen(s, i);
        if (n == 0)
        {
            AppendEscapedByte(out, c);
            ++i;
            continue;
        }
        out.append(s, i, n);
        i += n;
    }
    out.push_back('"');
}

void AppendDouble(std::string &out, double d)
{
    if (!std::isfinite(d))
    {
        out.append("null");
        return;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", d);
    out.append(buf);
}

}

JsonValue JNull()
{
    return JsonValue{};
}

JsonValue JBool(bool v)
{
    JsonValue j;
    j.type    = JsonType::Bool;
    j.boolVal = v;
    return j;
}

JsonValue JInt(int64_t v)
{
    JsonValue j;
    j.type   = JsonType::Int;
    j.intVal = v;
    return j;
}

JsonValue JDouble(double v)
{
    JsonValue j;
    j.type   = JsonType::Double;
    j.dblVal = v;
    return j;
}

JsonValue JStr(std::string v)
{
    JsonValue j;
    j.type   = JsonType::String;
    j.strVal = std::move(v);
    return j;
}

JsonValue JStr(const char *v)
{
    if (!v)
    {
        return JNull();
    }
    return JStr(std::string(v));
}

JsonValue JArr()
{
    JsonValue j;
    j.type = JsonType::Array;
    return j;
}

JsonValue JObj()
{
    JsonValue j;
    j.type = JsonType::Object;
    return j;
}

void Put(JsonValue &obj, std::string key, JsonValue v)
{
    obj.type = JsonType::Object;
    obj.objVal.emplace_back(std::move(key), std::move(v));
}

void Push(JsonValue &arr, JsonValue v)
{
    arr.type = JsonType::Array;
    arr.arrVal.push_back(std::move(v));
}

void WriteJson(const JsonValue &v, std::string &out)
{
    switch (v.type)
    {
        case JsonType::Null:
            out.append("null");
            return;
        case JsonType::Bool:
            out.append(v.boolVal ? "true" : "false");
            return;
        case JsonType::Int:
            out.append(std::to_string(v.intVal));
            return;
        case JsonType::Double:
            AppendDouble(out, v.dblVal);
            return;
        case JsonType::String:
            AppendString(out, v.strVal);
            return;
        case JsonType::Array:
        {
            out.push_back('[');
            for (size_t i = 0; i < v.arrVal.size(); ++i)
            {
                if (i)
                {
                    out.push_back(',');
                }
                WriteJson(v.arrVal[i], out);
            }
            out.push_back(']');
            return;
        }
        case JsonType::Object:
        {
            out.push_back('{');
            for (size_t i = 0; i < v.objVal.size(); ++i)
            {
                if (i)
                {
                    out.push_back(',');
                }
                AppendString(out, v.objVal[i].first);
                out.push_back(':');
                WriteJson(v.objVal[i].second, out);
            }
            out.push_back('}');
            return;
        }
    }
}

std::string ToJson(const JsonValue &v)
{
    std::string out;
    WriteJson(v, out);
    return out;
}

}
