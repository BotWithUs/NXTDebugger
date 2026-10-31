#include "MsgPackJson.h"

#include "rpc/MsgPack.h"

#include <cstdio>
#include <string>

namespace nxtdbg::ctl
{

namespace
{

namespace mp = nxt::rpc::msgpack;

constexpr size_t kMaxEncodedBytes = 4u * 1024u * 1024u;   // RPC frame ceiling

bool DecodeValue(mp::Reader &r, JsonValue &out, uint32_t depth);

bool DecodeMapKey(mp::Reader &r, std::string &outKey)
{
    if (r.Peek() == mp::Type::Str)
    {
        const char *s = nullptr;
        uint32_t    n = 0;
        if (!r.ReadString(s, n))
        {
            return false;
        }
        outKey.assign(s, n);
        return true;
    }
    // Non-string keys never appear on this wire; stringify rather than fail.
    JsonValue k;
    if (!DecodeValue(r, k, mp::kMaxNesting))
    {
        return false;
    }
    outKey = ToJson(k);
    return true;
}

bool DecodeContainer(mp::Reader &r, mp::Type t, JsonValue &out, uint32_t depth)
{
    if (depth >= mp::kMaxNesting)
    {
        return false;
    }
    uint32_t n = 0;
    if (t == mp::Type::Array)
    {
        if (!r.ReadArrayHeader(n))
        {
            return false;
        }
        out = JArr();
        out.arrVal.resize(n);
        for (uint32_t i = 0; i < n; ++i)
        {
            if (!DecodeValue(r, out.arrVal[i], depth + 1))
            {
                return false;
            }
        }
        return true;
    }
    if (!r.ReadMapHeader(n))
    {
        return false;
    }
    out = JObj();
    out.objVal.resize(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        if (!DecodeMapKey(r, out.objVal[i].first)
            || !DecodeValue(r, out.objVal[i].second, depth + 1))
        {
            return false;
        }
    }
    return true;
}

bool DecodeBin(mp::Reader &r, JsonValue &out)
{
    const uint8_t *b = nullptr;
    uint32_t       n = 0;
    if (!r.ReadBin(b, n))
    {
        return false;
    }
    std::string hex;
    hex.reserve(size_t(n) * 2);
    char pair[4];
    for (uint32_t i = 0; i < n; ++i)
    {
        std::snprintf(pair, sizeof(pair), "%02x", b[i]);
        hex.append(pair);
    }
    out = JStr(std::move(hex));
    return true;
}

bool DecodeValue(mp::Reader &r, JsonValue &out, uint32_t depth)
{
    const mp::Type t = r.Peek();
    switch (t)
    {
        case mp::Type::Nil:
            out = JNull();
            return r.ReadNil();
        case mp::Type::Bool:
        {
            bool v = false;
            out    = JBool(false);
            if (!r.ReadBool(v))
            {
                return false;
            }
            out.boolVal = v;
            return true;
        }
        case mp::Type::Int:
        {
            int64_t v = 0;
            if (!r.ReadInt(v))
            {
                return false;
            }
            out = JInt(v);
            return true;
        }
        case mp::Type::Float:
        {
            double v = 0.0;
            if (!r.ReadDouble(v))
            {
                return false;
            }
            out = JDouble(v);
            return true;
        }
        case mp::Type::Str:
        {
            const char *s = nullptr;
            uint32_t    n = 0;
            if (!r.ReadString(s, n))
            {
                return false;
            }
            out = JStr(std::string(s, n));
            return true;
        }
        case mp::Type::Bin:
            return DecodeBin(r, out);
        case mp::Type::Array:
        case mp::Type::Map:
            return DecodeContainer(r, t, out, depth);
        case mp::Type::Invalid:
            return false;
    }
    return false;
}

void EncodeValue(mp::Writer &w, const JsonValue &v)
{
    switch (v.type)
    {
        case JsonType::Null:
            w.WriteNil();
            return;
        case JsonType::Bool:
            w.WriteBool(v.boolVal);
            return;
        case JsonType::Int:
            w.WriteInt(v.intVal);
            return;
        case JsonType::Double:
            w.WriteDouble(v.dblVal);
            return;
        case JsonType::String:
            w.WriteString(v.strVal.data(), static_cast<uint32_t>(v.strVal.size()));
            return;
        case JsonType::Array:
            w.WriteArrayHeader(static_cast<uint32_t>(v.arrVal.size()));
            for (const JsonValue &e : v.arrVal)
            {
                EncodeValue(w, e);
            }
            return;
        case JsonType::Object:
            w.WriteMapHeader(static_cast<uint32_t>(v.objVal.size()));
            for (const auto &kv : v.objVal)
            {
                w.WriteString(kv.first.data(), static_cast<uint32_t>(kv.first.size()));
                EncodeValue(w, kv.second);
            }
            return;
    }
}

}

bool MsgPackToJson(const uint8_t *data, size_t len, JsonValue &outValue)
{
    if (!data || len == 0)
    {
        outValue = JNull();
        return false;
    }
    mp::Reader r(data, len);
    return DecodeValue(r, outValue, 0);
}

bool MsgPackToJson(const std::vector<uint8_t> &bytes, JsonValue &outValue)
{
    return MsgPackToJson(bytes.data(), bytes.size(), outValue);
}

bool JsonToMsgPack(const JsonValue &value, std::vector<uint8_t> &outBytes)
{
    // The Writer is fixed-capacity and flags overflow; grow and retry.
    for (size_t cap = 4096; cap <= kMaxEncodedBytes; cap *= 2)
    {
        outBytes.assign(cap, 0);
        mp::Writer w(outBytes.data(), outBytes.size());
        EncodeValue(w, value);
        if (!w.Overflowed())
        {
            outBytes.resize(w.BytesWritten());
            return true;
        }
    }
    outBytes.clear();
    return false;
}

}
