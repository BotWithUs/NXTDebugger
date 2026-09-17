#pragma once
#include <cstdint>
#include <cstddef>

// Minimal MessagePack codec — pull-parser for reads, append-only writer for
// writes. No allocations, no Value tree. Sufficient for the RPC bridge:
//   nil, bool, int (all widths), float64, str (fixstr + str8/16/32),
//   array (fixarray + array16/32), map (fixmap + map16/32).
//
// We deliberately don't surface bin/ext/float32 on the read side — Java's
// MessagePackCodec emits int (compact), double (for floats), and string
// (utf-8) only for the request bodies we parse, so wider support would be
// dead code. Float32 is decoded too because it's cheap.

namespace nxt::rpc::msgpack {

// Tags for Peek(); we don't try to distinguish int/uint widths here, since
// callers ask for "give me an int64" and the reader handles narrowing.
enum class Type : uint8_t {
    Invalid,
    Nil,
    Bool,
    Int,
    Float,
    Str,
    Bin,
    Array,
    Map,
};

class Reader {
public:
    Reader(const uint8_t* data, size_t len) noexcept
        : m_data(data), m_pos(0), m_len(len) {}

    // Inspect the next byte's format family without consuming. Returns
    // Invalid if at EOF or the byte is in an unsupported range (bin/ext).
    Type Peek() const noexcept;

    // Each Read* consumes one value and returns false on type mismatch or
    // truncation. The caller treats false as a hard parse error — we don't
    // attempt rollback because the dispatcher discards malformed messages
    // entirely.
    bool ReadNil()                                    noexcept;
    bool ReadBool(bool& v)                            noexcept;
    bool ReadInt(int64_t& v)                          noexcept;
    bool ReadDouble(double& v)                        noexcept;
    // Returns a pointer into the source buffer — valid only as long as the
    // owning buffer outlives the use. Strings are NOT NUL-terminated; use
    // the returned len.
    bool ReadString(const char*& s, uint32_t& len)    noexcept;

    // Read a bin8/bin16/bin32 value. Same caller contract as ReadString
    // (pointer into source buffer, valid for buffer lifetime). Added for
    // agent.set_license (the signed-license payload is bin, not str). The
    // Java host doesn't emit bin today; this is the first inbound bin field.
    bool ReadBin(const uint8_t*& bytes, uint32_t& len) noexcept;

    bool ReadArrayHeader(uint32_t& count)             noexcept;
    bool ReadMapHeader(uint32_t& count)               noexcept;

    // Skip the next value of any supported type, including nested
    // arrays/maps. Returns false if the value is malformed.
    bool SkipValue()                                  noexcept;

    bool AtEnd() const noexcept { return m_pos >= m_len; }
    size_t Pos() const noexcept { return m_pos; }
    size_t Remaining() const noexcept { return m_len - m_pos; }
    // Underlying source buffer. Combined with Pos() this lets a handler
    // capture a value's raw msgpack bytes (save start, SkipValue, take
    // [Data()+start, Data()+Pos())). Used by _debug.publish to forward
    // an arbitrary `data` value to the broker verbatim.
    const uint8_t *Data() const noexcept { return m_data; }

private:
    bool Need(size_t n) const noexcept { return m_pos + n <= m_len; }
    uint8_t  R8()  noexcept;
    uint16_t R16() noexcept;
    uint32_t R32() noexcept;
    uint64_t R64() noexcept;

    const uint8_t* m_data;
    size_t         m_pos;
    size_t         m_len;
};

class Writer {
public:
    Writer(uint8_t* buf, size_t cap) noexcept
        : m_buf(buf), m_pos(0), m_cap(cap), m_overflow(false) {}

    // Each Write* picks the smallest format that fits (positive fixint
    // before uint8, fixstr before str8, etc.). On overflow the writer marks
    // itself failed and silently drops further writes; callers check
    // Overflowed() once at the end.
    void WriteNil()                                   noexcept;
    void WriteBool(bool v)                            noexcept;
    void WriteInt(int64_t v)                          noexcept;
    void WriteUInt(uint64_t v)                        noexcept;
    void WriteDouble(double v)                        noexcept;
    void WriteString(const char* s, uint32_t len)     noexcept;
    void WriteCStr(const char* s)                     noexcept;
    void WriteArrayHeader(uint32_t count)             noexcept;
    void WriteMapHeader(uint32_t count)               noexcept;

    // Splice a fully-formed msgpack value (one or more) into the stream
    // verbatim. Used by the rpc broker's tap to forward params/reply
    // bytes without re-decoding them. Caller is responsible for the bytes
    // being valid msgpack; this is a raw memcpy with overflow tracking.
    void WriteRaw(const uint8_t* bytes, uint32_t len) noexcept;

    bool   Overflowed() const noexcept { return m_overflow; }
    size_t BytesWritten() const noexcept { return m_pos; }
    uint8_t* Data() const noexcept { return m_buf; }

    // Rewind to a previously-saved BytesWritten() snapshot and clear any
    // overflow flag. Used by the dispatcher to discard a partial response
    // when a handler reports failure mid-write — we then build an error
    // envelope on top of the cleared buffer instead.
    void Rewind(size_t pos) noexcept {
        if (pos <= m_cap) m_pos = pos;
        m_overflow = false;
    }

private:
    bool Reserve(size_t n) noexcept;
    void W8(uint8_t v)     noexcept;
    void W16(uint16_t v)   noexcept;
    void W32(uint32_t v)   noexcept;
    void W64(uint64_t v)   noexcept;

    uint8_t* m_buf;
    size_t   m_pos;
    size_t   m_cap;
    bool     m_overflow;
};

// Compares a (ptr,len) string to a NUL-terminated literal — small helper for
// matching map keys. Equal iff lengths match and bytes are identical.
bool StrEq(const char* a, uint32_t aLen, const char* zlit) noexcept;

// Param-map helpers shared across handlers. All accept any caller's wire
// shape — empty buffer, missing map, or a map with extra keys — so handlers
// stay defensive without per-call boilerplate.

// Drops every key/value pair from a (possibly absent) param map. Use for
// no-arg handlers. Returns false on malformed parse.
bool DrainParams(Reader& r) noexcept;

// Walks each (key, value) pair, invoking `match(key, keyLen, reader)` with
// the reader positioned at the value. The lambda returns true if it consumed
// the value, false to let the walker SkipValue() it. Returns false only on
// malformed parse.
template <typename F>
bool ForEachParam(Reader& r, F&& match) noexcept
{
    if (r.Remaining() == 0) return true;
    uint32_t n;
    if (!r.ReadMapHeader(n)) return false;
    for (uint32_t i = 0; i < n; ++i) {
        const char* key   = nullptr;
        uint32_t    keyLen = 0;
        if (!r.ReadString(key, keyLen)) return false;
        if (!match(key, keyLen, r)) {
            if (!r.SkipValue()) return false;
        }
    }
    return true;
}

// Read a single named int / bool param from a map. `out` is left untouched
// if the key isn't present. Returns false on malformed parse.
bool ReadIntParam(Reader& r, const char* name, int64_t& out) noexcept;
bool ReadBoolParam(Reader& r, const char* name, bool& out) noexcept;

}
