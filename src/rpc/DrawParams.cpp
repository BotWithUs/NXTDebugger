#include "DrawParams.h"

#include <cstdio>

namespace nxtdbg::rpc
{

using nxt::rpc::msgpack::Reader;
using nxt::rpc::msgpack::Type;
using nxt::rpc::msgpack::Writer;

namespace
{

constexpr uint32_t kMaxParamEntries = 32;
constexpr size_t   kParamsBufBytes  = 8192;

std::string ArrayToText(Reader &r)
{
    uint32_t n = 0;
    r.ReadArrayHeader(n);
    std::string out = "[";
    for (uint32_t i = 0; i < n; ++i)
    {
        if (i != 0)
        {
            out.append(", ");
        }
        out.append(ValueToText(r));
    }
    out.push_back(']');
    return out;
}

std::string MapToText(Reader &r)
{
    uint32_t n = 0;
    r.ReadMapHeader(n);
    std::string out = "{";
    for (uint32_t i = 0; i < n; ++i)
    {
        if (i != 0)
        {
            out.append(", ");
        }
        const char *k  = nullptr;
        uint32_t    kn = 0;
        if (!r.ReadString(k, kn))
        {
            out.append("<bad key>}");
            return out;
        }
        out.append(k, kn);
        out.append(": ");
        out.append(ValueToText(r));
    }
    out.push_back('}');
    return out;
}

}   // namespace

uint32_t ArgbFromFloats(const float rgba[4])
{
    auto quant = [](float v) -> uint32_t
    {
        const float c = (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
        return static_cast<uint32_t>(c * 255.0f + 0.5f);
    };
    return (quant(rgba[3]) << 24) | (quant(rgba[0]) << 16)
         | (quant(rgba[1]) << 8)  | quant(rgba[2]);
}

void ParamBuilder::AddInt(const char *name, int64_t v)
{
    if (count >= kMaxParamEntries) { return; }
    entries[count++] = { name, ParamEntry::Kind::Int, v, false, nullptr, nullptr, 0 };
}

void ParamBuilder::AddBool(const char *name, bool v)
{
    if (count >= kMaxParamEntries) { return; }
    entries[count++] = { name, ParamEntry::Kind::Bool, 0, v, nullptr, nullptr, 0 };
}

void ParamBuilder::AddStr(const char *name, const char *s)
{
    if (count >= kMaxParamEntries) { return; }
    entries[count++] = { name, ParamEntry::Kind::Str, 0, false, s, nullptr, 0 };
}

void ParamBuilder::AddIntArray(const char *name, const int32_t *arr, uint32_t n)
{
    if (count >= kMaxParamEntries) { return; }
    entries[count++] = { name, ParamEntry::Kind::IntArray, 0, false, nullptr, arr, n };
}

void WriteParamEntry(Writer &w, const ParamEntry &e)
{
    w.WriteCStr(e.name);
    switch (e.kind)
    {
    case ParamEntry::Kind::Int:
        w.WriteInt(e.asInt);
        break;
    case ParamEntry::Kind::Bool:
        w.WriteBool(e.asBool);
        break;
    case ParamEntry::Kind::Str:
        w.WriteCStr(e.asStr);
        break;
    case ParamEntry::Kind::IntArray:
        w.WriteArrayHeader(e.arrayLen);
        for (uint32_t i = 0; i < e.arrayLen; ++i)
        {
            w.WriteInt(e.asArray[i]);
        }
        break;
    }
}

void WriteParamMap(Writer &w, const ParamBuilder &b)
{
    w.WriteMapHeader(b.count);
    for (uint32_t i = 0; i < b.count; ++i)
    {
        WriteParamEntry(w, b.entries[i]);
    }
}

bool EmitParams(const ParamBuilder &b, std::vector<uint8_t> &out)
{
    out.assign(kParamsBufBytes, 0);
    Writer w(out.data(), out.size());
    WriteParamMap(w, b);
    if (w.Overflowed())
    {
        out.clear();
        return false;
    }
    out.resize(w.BytesWritten());
    return true;
}

bool ReadStrInto(Reader &r, std::string &out)
{
    const char *s = nullptr;
    uint32_t    n = 0;
    if (!r.ReadString(s, n))
    {
        return false;
    }
    out.assign(s, n);
    return true;
}

std::string ValueToText(Reader &r)
{
    char buf[64];
    switch (r.Peek())
    {
    case Type::Nil:
        r.ReadNil();
        return "nil";
    case Type::Bool:
    {
        bool v = false;
        r.ReadBool(v);
        return v ? "true" : "false";
    }
    case Type::Int:
    {
        int64_t v = 0;
        r.ReadInt(v);
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        return buf;
    }
    case Type::Str:
    {
        std::string s;
        ReadStrInto(r, s);
        return s;
    }
    case Type::Float:
    {
        double v = 0.0;
        r.ReadDouble(v);
        std::snprintf(buf, sizeof(buf), "%g", v);
        return buf;
    }
    case Type::Array:
        return ArrayToText(r);
    // EVERY successful reply on this surface is a map -- {key},
    // {count, dropped, error}, {removed}, {removed, scope}, {enabled}, and the
    // whole of the probe's result. Falling into `default` here rendered all of
    // them as "?" while error replies, which are plain strings, read fine: the
    // panel looked healthy in exactly the case where it was not.
    case Type::Map:
        return MapToText(r);
    default:
        r.SkipValue();
        return "?";
    }
}

const char *StatusText(CallStatus s)
{
    switch (s)
    {
    case CallStatus::Ok:            return "OK";
    case CallStatus::Busy:          return "PIPE BUSY";
    case CallStatus::Disconnected:  return "DISCONNECTED";
    case CallStatus::Timeout:       return "TIMEOUT";
    case CallStatus::ProtocolError: return "PROTOCOL ERROR";
    case CallStatus::HandlerError:  return "REJECTED";
    }
    return "?";
}

}
