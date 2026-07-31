#include "MsgPack.h"
#include <Windows.h>   // RtlCopyMemory

namespace nxt::rpc::msgpack {

namespace {

// Big-endian load/store helpers. msgpack's multi-byte integers are BE; x64
// is LE, so we always swap. Hand-rolled because we can't depend on _byteswap
// intrinsics being constexpr-safe in freestanding headers and we want the
// helpers visible to the optimizer.
inline uint16_t SwapBE16(uint16_t v) noexcept {
    return static_cast<uint16_t>((v << 8) | (v >> 8));
}
inline uint32_t SwapBE32(uint32_t v) noexcept {
    return ((v & 0xFF000000u) >> 24) |
           ((v & 0x00FF0000u) >> 8)  |
           ((v & 0x0000FF00u) << 8)  |
           ((v & 0x000000FFu) << 24);
}
inline uint64_t SwapBE64(uint64_t v) noexcept {
    return (static_cast<uint64_t>(SwapBE32(static_cast<uint32_t>(v))) << 32) |
            static_cast<uint64_t>(SwapBE32(static_cast<uint32_t>(v >> 32)));
}

// Format prefix bytes (msgpack spec).
constexpr uint8_t kNil      = 0xC0;
constexpr uint8_t kFalse    = 0xC2;
constexpr uint8_t kTrue     = 0xC3;
constexpr uint8_t kFloat32  = 0xCA;
constexpr uint8_t kFloat64  = 0xCB;
constexpr uint8_t kUInt8    = 0xCC;
constexpr uint8_t kUInt16   = 0xCD;
constexpr uint8_t kUInt32   = 0xCE;
constexpr uint8_t kUInt64   = 0xCF;
constexpr uint8_t kInt8     = 0xD0;
constexpr uint8_t kInt16    = 0xD1;
constexpr uint8_t kInt32    = 0xD2;
constexpr uint8_t kInt64    = 0xD3;
constexpr uint8_t kBin8     = 0xC4;
constexpr uint8_t kBin16    = 0xC5;
constexpr uint8_t kBin32    = 0xC6;
constexpr uint8_t kStr8     = 0xD9;
constexpr uint8_t kStr16    = 0xDA;
constexpr uint8_t kStr32    = 0xDB;
constexpr uint8_t kArray16  = 0xDC;
constexpr uint8_t kArray32  = 0xDD;
constexpr uint8_t kMap16    = 0xDE;
constexpr uint8_t kMap32    = 0xDF;

}

// =========================================================================
// Reader
// =========================================================================

uint8_t Reader::R8() noexcept {
    return m_data[m_pos++];
}
uint16_t Reader::R16() noexcept {
    uint16_t v;
    RtlCopyMemory(&v, m_data + m_pos, 2);
    m_pos += 2;
    return SwapBE16(v);
}
uint32_t Reader::R32() noexcept {
    uint32_t v;
    RtlCopyMemory(&v, m_data + m_pos, 4);
    m_pos += 4;
    return SwapBE32(v);
}
uint64_t Reader::R64() noexcept {
    uint64_t v;
    RtlCopyMemory(&v, m_data + m_pos, 8);
    m_pos += 8;
    return SwapBE64(v);
}

Type Reader::Peek() const noexcept {
    if (!Need(1)) return Type::Invalid;
    uint8_t b = m_data[m_pos];
    if (b <= 0x7F)              return Type::Int;          // positive fixint
    if (b >= 0xE0)              return Type::Int;          // negative fixint
    if (b >= 0x80 && b <= 0x8F) return Type::Map;          // fixmap
    if (b >= 0x90 && b <= 0x9F) return Type::Array;        // fixarray
    if (b >= 0xA0 && b <= 0xBF) return Type::Str;          // fixstr
    switch (b) {
    case kNil:                  return Type::Nil;
    case kFalse: case kTrue:    return Type::Bool;
    case kFloat32: case kFloat64:           return Type::Float;
    case kUInt8: case kUInt16: case kUInt32: case kUInt64:
    case kInt8:  case kInt16:  case kInt32:  case kInt64:  return Type::Int;
    case kStr8: case kStr16: case kStr32:   return Type::Str;
    case kBin8: case kBin16: case kBin32:   return Type::Bin;
    case kArray16: case kArray32:           return Type::Array;
    case kMap16: case kMap32:               return Type::Map;
    default:                                 return Type::Invalid;
    }
}

bool Reader::ReadNil() noexcept {
    if (!Need(1) || m_data[m_pos] != kNil) return false;
    ++m_pos;
    return true;
}

bool Reader::ReadBool(bool& v) noexcept {
    if (!Need(1)) return false;
    uint8_t b = m_data[m_pos];
    if (b == kTrue)       { v = true;  ++m_pos; return true; }
    if (b == kFalse)      { v = false; ++m_pos; return true; }
    return false;
}

bool Reader::ReadInt(int64_t& v) noexcept {
    if (!Need(1)) return false;
    uint8_t b = m_data[m_pos];
    // positive fixint
    if (b <= 0x7F)             { v = static_cast<int8_t>(b); ++m_pos; return true; }
    // negative fixint
    if (b >= 0xE0)             { v = static_cast<int8_t>(b); ++m_pos; return true; }
    ++m_pos;
    switch (b) {
    case kUInt8:
        if (!Need(1)) { --m_pos; return false; }
        v = static_cast<int64_t>(R8()); return true;
    case kUInt16:
        if (!Need(2)) { --m_pos; return false; }
        v = static_cast<int64_t>(R16()); return true;
    case kUInt32:
        if (!Need(4)) { --m_pos; return false; }
        v = static_cast<int64_t>(R32()); return true;
    case kUInt64: {
        if (!Need(8)) { --m_pos; return false; }
        uint64_t u = R64();
        // We narrow uint64 -> int64 unconditionally; the RPC surface never
        // sends values that overflow int64 (Java IDs / sizes are int).
        v = static_cast<int64_t>(u);
        return true;
    }
    case kInt8:
        if (!Need(1)) { --m_pos; return false; }
        v = static_cast<int8_t>(R8()); return true;
    case kInt16:
        if (!Need(2)) { --m_pos; return false; }
        v = static_cast<int16_t>(R16()); return true;
    case kInt32:
        if (!Need(4)) { --m_pos; return false; }
        v = static_cast<int32_t>(R32()); return true;
    case kInt64:
        if (!Need(8)) { --m_pos; return false; }
        v = static_cast<int64_t>(R64()); return true;
    default:
        --m_pos;
        return false;
    }
}

bool Reader::ReadDouble(double& v) noexcept {
    if (!Need(1)) return false;
    uint8_t b = m_data[m_pos];
    ++m_pos;
    if (b == kFloat32) {
        if (!Need(4)) { --m_pos; return false; }
        uint32_t bits = R32();
        float f;
        RtlCopyMemory(&f, &bits, 4);
        v = static_cast<double>(f);
        return true;
    }
    if (b == kFloat64) {
        if (!Need(8)) { --m_pos; return false; }
        uint64_t bits = R64();
        RtlCopyMemory(&v, &bits, 8);
        return true;
    }
    --m_pos;
    return false;
}

bool Reader::ReadString(const char*& s, uint32_t& len) noexcept {
    if (!Need(1)) return false;
    uint8_t b = m_data[m_pos];
    uint32_t n = 0;
    if (b >= 0xA0 && b <= 0xBF) {
        n = b & 0x1Fu;
        ++m_pos;
    } else if (b == kStr8) {
        if (!Need(2)) return false;
        ++m_pos;
        n = R8();
    } else if (b == kStr16) {
        if (!Need(3)) return false;
        ++m_pos;
        n = R16();
    } else if (b == kStr32) {
        if (!Need(5)) return false;
        ++m_pos;
        n = R32();
    } else {
        return false;
    }
    if (!Need(n)) { return false; }
    s = reinterpret_cast<const char*>(m_data + m_pos);
    len = n;
    m_pos += n;
    return true;
}

bool Reader::ReadBin(const uint8_t*& bytes, uint32_t& len) noexcept {
    if (!Need(1)) return false;
    uint8_t b = m_data[m_pos];
    uint32_t n = 0;
    if (b == kBin8) {
        if (!Need(2)) return false;
        ++m_pos;
        n = R8();
    } else if (b == kBin16) {
        if (!Need(3)) return false;
        ++m_pos;
        n = R16();
    } else if (b == kBin32) {
        if (!Need(5)) return false;
        ++m_pos;
        n = R32();
    } else {
        return false;
    }
    if (!Need(n)) { return false; }
    bytes = m_data + m_pos;
    len   = n;
    m_pos += n;
    return true;
}

bool Reader::ReadArrayHeader(uint32_t& count) noexcept {
    if (!Need(1)) return false;
    uint8_t b = m_data[m_pos];
    if (b >= 0x90 && b <= 0x9F) { count = b & 0x0Fu; ++m_pos; return true; }
    if (b == kArray16) {
        if (!Need(3)) return false;
        ++m_pos; count = R16(); return true;
    }
    if (b == kArray32) {
        if (!Need(5)) return false;
        ++m_pos; count = R32(); return true;
    }
    return false;
}

bool Reader::ReadMapHeader(uint32_t& count) noexcept {
    if (!Need(1)) return false;
    uint8_t b = m_data[m_pos];
    if (b >= 0x80 && b <= 0x8F) { count = b & 0x0Fu; ++m_pos; return true; }
    if (b == kMap16) {
        if (!Need(3)) return false;
        ++m_pos; count = R16(); return true;
    }
    if (b == kMap32) {
        if (!Need(5)) return false;
        ++m_pos; count = R32(); return true;
    }
    return false;
}

bool Reader::SkipValue() noexcept {
    Type t = Peek();
    switch (t) {
    case Type::Nil:    return ReadNil();
    case Type::Bool:   { bool b; return ReadBool(b); }
    case Type::Int:    { int64_t v; return ReadInt(v); }
    case Type::Float:  { double v; return ReadDouble(v); }
    case Type::Str:    { const char* s; uint32_t n; return ReadString(s, n); }
    case Type::Bin:    { const uint8_t* p; uint32_t n; return ReadBin(p, n); }
    case Type::Array: {
        uint32_t n;
        if (!ReadArrayHeader(n)) return false;
        for (uint32_t i = 0; i < n; ++i) if (!SkipValue()) return false;
        return true;
    }
    case Type::Map: {
        uint32_t n;
        if (!ReadMapHeader(n)) return false;
        for (uint32_t i = 0; i < n; ++i) {
            if (!SkipValue()) return false;   // key
            if (!SkipValue()) return false;   // value
        }
        return true;
    }
    case Type::Invalid:
    default:
        return false;
    }
}

// =========================================================================
// Writer
// =========================================================================

bool Writer::Reserve(size_t n) noexcept {
    if (m_overflow) return false;
    if (m_pos + n > m_cap) { m_overflow = true; return false; }
    return true;
}

void Writer::W8(uint8_t v) noexcept {
    if (!Reserve(1)) return;
    m_buf[m_pos++] = v;
}
void Writer::W16(uint16_t v) noexcept {
    if (!Reserve(2)) return;
    uint16_t be = SwapBE16(v);
    RtlCopyMemory(m_buf + m_pos, &be, 2);
    m_pos += 2;
}
void Writer::W32(uint32_t v) noexcept {
    if (!Reserve(4)) return;
    uint32_t be = SwapBE32(v);
    RtlCopyMemory(m_buf + m_pos, &be, 4);
    m_pos += 4;
}
void Writer::W64(uint64_t v) noexcept {
    if (!Reserve(8)) return;
    uint64_t be = SwapBE64(v);
    RtlCopyMemory(m_buf + m_pos, &be, 8);
    m_pos += 8;
}

void Writer::WriteNil() noexcept           { W8(kNil); }
void Writer::WriteBool(bool v) noexcept    { W8(v ? kTrue : kFalse); }

void Writer::WriteInt(int64_t v) noexcept {
    // Smallest format that round-trips. Java's MessagePackCodec hands us
    // back the same logical value regardless of width, so picking the
    // narrowest form just saves bytes.
    if (v >= 0) {
        WriteUInt(static_cast<uint64_t>(v));
        return;
    }
    if (v >= -32)             { W8(static_cast<uint8_t>(v)); return; }
    if (v >= -128)            { W8(kInt8);  W8(static_cast<uint8_t>(v));               return; }
    if (v >= -32768)          { W8(kInt16); W16(static_cast<uint16_t>(v));             return; }
    if (v >= -2147483648LL)   { W8(kInt32); W32(static_cast<uint32_t>(v));             return; }
    W8(kInt64); W64(static_cast<uint64_t>(v));
}

void Writer::WriteUInt(uint64_t v) noexcept {
    if (v <= 0x7F)            { W8(static_cast<uint8_t>(v)); return; }
    if (v <= 0xFF)            { W8(kUInt8);  W8(static_cast<uint8_t>(v));   return; }
    if (v <= 0xFFFF)          { W8(kUInt16); W16(static_cast<uint16_t>(v)); return; }
    if (v <= 0xFFFFFFFFu)     { W8(kUInt32); W32(static_cast<uint32_t>(v)); return; }
    W8(kUInt64); W64(v);
}

void Writer::WriteDouble(double v) noexcept {
    W8(kFloat64);
    uint64_t bits;
    RtlCopyMemory(&bits, &v, 8);
    W64(bits);
}

void Writer::WriteString(const char* s, uint32_t len) noexcept {
    if (len <= 31)            { W8(static_cast<uint8_t>(0xA0u | len)); }
    else if (len <= 0xFF)     { W8(kStr8);  W8(static_cast<uint8_t>(len)); }
    else if (len <= 0xFFFF)   { W8(kStr16); W16(static_cast<uint16_t>(len)); }
    else                      { W8(kStr32); W32(len); }
    if (!Reserve(len)) return;
    if (len) RtlCopyMemory(m_buf + m_pos, s, len);
    m_pos += len;
}

void Writer::WriteCStr(const char* s) noexcept {
    uint32_t n = 0;
    if (s) while (s[n]) ++n;
    WriteString(s, n);
}

void Writer::WriteArrayHeader(uint32_t count) noexcept {
    if (count <= 15)          { W8(static_cast<uint8_t>(0x90u | count)); return; }
    if (count <= 0xFFFF)      { W8(kArray16); W16(static_cast<uint16_t>(count)); return; }
    W8(kArray32); W32(count);
}

void Writer::WriteMapHeader(uint32_t count) noexcept {
    if (count <= 15)          { W8(static_cast<uint8_t>(0x80u | count)); return; }
    if (count <= 0xFFFF)      { W8(kMap16); W16(static_cast<uint16_t>(count)); return; }
    W8(kMap32); W32(count);
}

void Writer::WriteRaw(const uint8_t* bytes, uint32_t len) noexcept {
    if (!Reserve(len)) return;
    if (len) RtlCopyMemory(m_buf + m_pos, bytes, len);
    m_pos += len;
}

// =========================================================================
// Helpers
// =========================================================================

bool StrEq(const char* a, uint32_t aLen, const char* zlit) noexcept {
    if (!zlit) return false;
    uint32_t i = 0;
    while (i < aLen && zlit[i]) {
        if (a[i] != zlit[i]) return false;
        ++i;
    }
    return i == aLen && zlit[i] == '\0';
}

bool DrainParams(Reader& r) noexcept {
    if (r.Remaining() == 0) return true;
    uint32_t n;
    if (!r.ReadMapHeader(n)) return false;
    for (uint32_t i = 0; i < n; ++i) {
        if (!r.SkipValue()) return false;
        if (!r.SkipValue()) return false;
    }
    return true;
}

bool ReadIntParam(Reader& r, const char* name, int64_t& out) noexcept {
    bool found = false;
    bool ok = ForEachParam(r, [&](const char* k, uint32_t kn, Reader& rd) {
        if (StrEq(k, kn, name)) {
            int64_t v;
            if (rd.ReadInt(v)) { out = v; found = true; return true; }
        }
        return false;
    });
    return ok && found;
}

bool ReadBoolParam(Reader& r, const char* name, bool& out) noexcept {
    bool found = false;
    bool ok = ForEachParam(r, [&](const char* k, uint32_t kn, Reader& rd) {
        if (StrEq(k, kn, name)) {
            bool v;
            if (rd.ReadBool(v)) { out = v; found = true; return true; }
        }
        return false;
    });
    return ok && found;
}

}
