#include "Panels.h"

#include "app/App.h"
#include "app/Theme.h"
#include "rpc/MsgPack.h"
#include "rpc/RpcClient.h"

#include "imgui.h"

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ===========================================================================
// Debug Draw panel — a hand driver for the agent's retained overlay store.
//
// The drawing API (9 debug_draw_* methods plus the four highlight helpers) had
// no interactive consumer: it was exercised only by scenario JSON and by a Java
// script. This panel is the manual one. It SEES the store (including other
// connections' commands, via scope "all") and DRIVES every method by hand.
//
// Normative contract: wire/PROTOCOL.md section 4.4. Every cap and reply field
// below was read against the producer's handlers at NXTLibrary@7a655a8
// (src/rpc/Handlers.cpp, src/overlay/DrawTypes.h) rather than taken from prose.
//
// Four wire behaviours this panel exists to render HONESTLY, because each one
// is invisible or misleading if a UI renders the obvious thing instead:
//
//   * `project` is a five-valued state, not a bool. Collapsing off_viewport,
//     behind_camera and unavailable into "not resolved" throws away the reason,
//     which is the thing a debugger is for. Each state gets its own colour.
//   * `resolved: true` does not promise a paintable area. A zero-extent rect is
//     legal (a distant footprint, and `text` by design), so the table flags
//     zero extents separately from unresolved.
//   * `ttl_ms` in a list reply is REMAINING, not what was sent — and it is -1,
//     not 0, for a command set with `ttl_ms: 0`. The column is labelled "ttl
//     left" and -1 renders as "none", never as a negative millisecond count.
//   * `debug_draw_set_batch` reports a cap hit as SUCCESS, carrying `dropped`
//     and only the FIRST error string; an envelope error means the store state
//     is UNKNOWN, not clean. Both are surfaced, and an envelope error forces a
//     re-list rather than leaving the table showing a state nobody verified.
//
// Pipe budget: this panel calls through the shared App::rpc client, like every
// other round-trip panel. kPipeMaxInstances is 4 and the debugger already
// spends three (shared rpc, shared tap, RPC console) — a private connection
// here would take the last one and lock the Java host out.
// ===========================================================================

namespace nxtdbg::panels
{

namespace
{

using nxt::rpc::msgpack::Reader;
using nxt::rpc::msgpack::Type;
using nxt::rpc::msgpack::Writer;

// --- Caps, mirrored from the contract --------------------------------------
//
// A cap the user cannot see is a cap they discover by hitting it. These are
// displayed, not just enforced. Sourced from overlay/DrawTypes.h at the pinned
// producer sha and PROTOCOL.md section 4.4.
constexpr uint32_t kMaxDrawCmds     = 512;
constexpr uint32_t kMaxTextSlots    = 64;
constexpr uint32_t kMaxPolySlots    = 64;
constexpr uint32_t kMaxBatchItems   = 256;
constexpr uint32_t kMaxKeyBytes     = 47;      // kMaxKeyChars 48, incl. NUL
constexpr uint32_t kMaxTextUnits    = 127;     // kTextSlotChars 128, incl. NUL
constexpr uint32_t kListPageMax     = 128;
constexpr int32_t  kMaxScreenCoord  = 1 << 20;
constexpr int32_t  kMaxScreenExtent = 1 << 14;
constexpr int32_t  kMaxWorldCoord   = 1 << 23;
constexpr int32_t  kMaxWorldTile    = 32768;

constexpr DWORD kCallTimeoutMs = 2000;

// Deliberately NOT "entity" / "tile": debug_draw_set rejects those two with
// `unknown kind`, because they carry semantics no geometry key can express.
// They are reachable only through the highlight helpers, which is why this
// combo and the Highlights tab are separate surfaces.
constexpr const char *kKindNames[] = {
    "line", "rect", "ellipse", "poly", "text", "component",
};
constexpr int kKindCount = static_cast<int>(sizeof(kKindNames) / sizeof(kKindNames[0]));

constexpr const char *kFontNames[] = { "normal", "small", "large", "heading" };
constexpr int kFontCount = static_cast<int>(sizeof(kFontNames) / sizeof(kFontNames[0]));

// ---------------------------------------------------------------------------
// Colour conversion. The wire packs 0xAARRGGBB; ImGui packs RGBA in memory
// order. Getting this backwards paints a red command blue, so it lives in one
// place with the shift widths written out.
// ---------------------------------------------------------------------------

ImU32 ImColFromArgb(uint32_t argb)
{
    return IM_COL32((argb >> 16) & 0xFFu, (argb >> 8) & 0xFFu,
                    argb & 0xFFu, (argb >> 24) & 0xFFu);
}

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

// ---------------------------------------------------------------------------
// Param builder.
//
// msgpack maps carry their entry count in the header, so the count has to be
// known before the first key is written. Collecting entries first and emitting
// once removes the class of bug where an optional param is added to the body
// and not to the count — which produces a frame the agent parses as garbage.
// ---------------------------------------------------------------------------

struct ParamEntry
{
    const char    *name;
    enum class Kind : uint8_t { Int, Bool, Str, IntArray } kind;
    int64_t        asInt;
    bool           asBool;
    const char    *asStr;
    const int32_t *asArray;
    uint32_t       arrayLen;
};

struct ParamBuilder
{
    ParamEntry entries[32];
    uint32_t   count = 0;

    void AddInt(const char *name, int64_t v)
    {
        if (count >= 32) { return; }
        entries[count++] = { name, ParamEntry::Kind::Int, v, false, nullptr, nullptr, 0 };
    }

    void AddBool(const char *name, bool v)
    {
        if (count >= 32) { return; }
        entries[count++] = { name, ParamEntry::Kind::Bool, 0, v, nullptr, nullptr, 0 };
    }

    void AddStr(const char *name, const char *s)
    {
        if (count >= 32) { return; }
        entries[count++] = { name, ParamEntry::Kind::Str, 0, false, s, nullptr, 0 };
    }

    void AddIntArray(const char *name, const int32_t *arr, uint32_t n)
    {
        if (count >= 32) { return; }
        entries[count++] = { name, ParamEntry::Kind::IntArray, 0, false, nullptr, arr, n };
    }
};

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

bool EmitParams(const ParamBuilder &b, std::vector<uint8_t> &out)
{
    out.assign(8192, 0);
    Writer w(out.data(), out.size());
    w.WriteMapHeader(b.count);
    for (uint32_t i = 0; i < b.count; ++i)
    {
        WriteParamEntry(w, b.entries[i]);
    }
    if (w.Overflowed())
    {
        out.clear();
        return false;
    }
    out.resize(w.BytesWritten());
    return true;
}

// ---------------------------------------------------------------------------
// One decoded debug_draw_list row. Field names match the wire exactly; the
// producer writes a 16-entry map per item and this mirrors all sixteen, because
// the fields a debugger most needs (project, rect, resolved) are the ones a
// summary would drop.
// ---------------------------------------------------------------------------

struct DrawRow
{
    std::string key;
    std::string kind;
    std::string space;
    std::string project;
    std::string font;
    std::string text;
    int64_t     plane      = 0;
    uint32_t    color      = 0;
    int64_t     thickness  = 0;
    int64_t     z          = 0;
    bool        isFilled   = false;
    bool        isClosed   = false;
    bool        isResolved = false;
    int64_t     geom[4]    = {};
    int64_t     rect[4]    = {};
    int64_t     ttlMs      = 0;
};

bool ReadInt4(Reader &r, int64_t out[4])
{
    uint32_t n = 0;
    if (!r.ReadArrayHeader(n))
    {
        return false;
    }
    for (uint32_t i = 0; i < n; ++i)
    {
        int64_t v = 0;
        if (!r.ReadInt(v))
        {
            return false;
        }
        if (i < 4)
        {
            out[i] = v;
        }
    }
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

bool MatchRowString(const char *k, uint32_t n, Reader &r, DrawRow &out)
{
    using nxt::rpc::msgpack::StrEq;
    if (StrEq(k, n, "key"))     { return ReadStrInto(r, out.key); }
    if (StrEq(k, n, "kind"))    { return ReadStrInto(r, out.kind); }
    if (StrEq(k, n, "space"))   { return ReadStrInto(r, out.space); }
    if (StrEq(k, n, "project")) { return ReadStrInto(r, out.project); }
    if (StrEq(k, n, "font"))    { return ReadStrInto(r, out.font); }
    if (StrEq(k, n, "text"))    { return ReadStrInto(r, out.text); }
    return false;
}

bool MatchRowScalar(const char *k, uint32_t n, Reader &r, DrawRow &out, bool &isHit)
{
    using nxt::rpc::msgpack::StrEq;
    isHit = true;
    int64_t v = 0;
    if (StrEq(k, n, "plane"))     { if (!r.ReadInt(v)) { return false; } out.plane = v; return true; }
    if (StrEq(k, n, "color"))     { if (!r.ReadInt(v)) { return false; } out.color = static_cast<uint32_t>(v); return true; }
    if (StrEq(k, n, "thickness")) { if (!r.ReadInt(v)) { return false; } out.thickness = v; return true; }
    if (StrEq(k, n, "z"))         { if (!r.ReadInt(v)) { return false; } out.z = v; return true; }
    if (StrEq(k, n, "ttl_ms"))    { if (!r.ReadInt(v)) { return false; } out.ttlMs = v; return true; }
    bool b = false;
    if (StrEq(k, n, "filled"))    { if (!r.ReadBool(b)) { return false; } out.isFilled = b; return true; }
    if (StrEq(k, n, "closed"))    { if (!r.ReadBool(b)) { return false; } out.isClosed = b; return true; }
    if (StrEq(k, n, "resolved"))  { if (!r.ReadBool(b)) { return false; } out.isResolved = b; return true; }
    if (StrEq(k, n, "geom"))      { return ReadInt4(r, out.geom); }
    if (StrEq(k, n, "rect"))      { return ReadInt4(r, out.rect); }
    isHit = false;
    return true;
}

bool ReadDrawRow(Reader &r, DrawRow &out)
{
    uint32_t n = 0;
    if (!r.ReadMapHeader(n))
    {
        return false;
    }
    for (uint32_t i = 0; i < n; ++i)
    {
        const char *k  = nullptr;
        uint32_t    kn = 0;
        if (!r.ReadString(k, kn))
        {
            return false;
        }
        if (MatchRowString(k, kn, r, out))
        {
            continue;
        }
        bool isHit = false;
        if (!MatchRowScalar(k, kn, r, out, isHit))
        {
            return false;
        }
        if (!isHit && !r.SkipValue())
        {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Generic one-line value renderer, used for the stats card. Rendering stats
// generically rather than from a hardcoded field list means a field the agent
// gains shows up without a code change here — and the agent currently answers
// 26 of them, four more than PROTOCOL.md enumerates.
// ---------------------------------------------------------------------------

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
    case Type::Array:
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
    default:
        r.SkipValue();
        return "?";
    }
}

// ---------------------------------------------------------------------------
// Panel state.
//
// Function-local static, matching every other panel here: App::panels holds
// plain function pointers with no per-panel instance, so panel state lives with
// the draw function. (cpp-rules would prefer this hung off a context object;
// the codebase convention wins, and changing it is a whole-project refactor.)
// ---------------------------------------------------------------------------

struct StylingForm
{
    float colorRgba[4] = { 1.0f, 0.67f, 0.28f, 1.0f };   // theme amber
    int   thickness    = 2;
    int   z            = 0;
    int   ttlMs        = 3000;
    bool  isFilled     = false;
};

struct ComposeForm
{
    char  key[64]      = "dbg-box";
    int   kindIdx      = 1;                 // rect
    bool  isWorld      = false;
    // Paired coordinates live in arrays because ImGui::InputInt2 takes one
    // pointer and writes two ints; handing it the address of a scalar member
    // and letting it reach the next one is pointer arithmetic across distinct
    // members, which the struct layout is not required to make legal.
    int   xy[2] = { 60, 60 };
    int   wh[2] = { 160, 90 };
    int   p1[2] = { 40, 40 };
    int   p2[2] = { 220, 160 };
    int   iface = 1477, comp = 0;
    int   plane        = 0;
    char  points[256]  = "0,0, 80,0, 80,60";
    char  text[256]    = "hello";
    char  label[128]   = "";
    int   fontIdx      = 0;
    bool  isClosed     = true;
    bool  useValue     = false;
    int   value        = 1234;
    int   decimals     = 2;
    StylingForm style;
};

struct BatchForm
{
    int  itemCount    = 8;
    int  strideX      = 24;
    bool isCorrupting = false;
    int  corruptIndex = 4;
};

struct HighlightForm
{
    int   tab        = 0;                  // 0 comp, 1 entity, 2 tile, 3 area
    int   iface = 1477, comp = 0;
    int   entityMode = 2;                  // 0 npc, 1 player, 2 self
    int   entityIndex = 0;
    int   entityW = 1, entityH = 1;
    int   tileX = 3200, tileY = 3200, plane = 0;
    int   areaW = 3, areaH = 3;
    char  key[64]    = "";                 // blank = let the agent generate one
    char  label[128] = "";
    int   fontIdx    = 0;
    StylingForm style;
};

struct ProbeForm
{
    bool  isByKey    = true;
    char  key[64]    = "dbg-box";
    int   inset      = 2;
    int   xy[2] = { 0, 0 };
    int   wh[2] = { 64, 64 };
    float colorRgba[4] = { 1.0f, 0.67f, 0.28f, 1.0f };
    int   sourceIdx  = 1;                  // 1 = overlay surface (cannot occlude)
};

struct DrawState
{
    std::vector<DrawRow> rows;
    uint32_t    total          = 0;
    bool        isScopeAll     = true;
    bool        isAutoRefresh  = true;
    float       refreshHz      = 5.0f;
    float       refreshAccum   = 0.0f;
    bool        forceRefresh   = true;
    int         selectedRow    = -1;
    std::string listError;

    bool        isEnabled      = true;
    bool        hasEnableState = false;

    std::vector<std::pair<std::string, std::string>> stats;
    bool        forceStats     = true;

    // Ownership tracking. Drawings belong to the CONNECTION, so the shared
    // client dropping and re-opening silently destroys everything this panel
    // drew. Without this the panel would simply look broken.
    bool        wasConnected   = false;
    uint32_t    setsIssued     = 0;
    bool        didLoseDrawings = false;

    // Last-call feedback, and the sticky "store state unknown" flag an
    // envelope error from a batch has to raise.
    std::string lastCall;
    std::string lastResult;
    ImU32       lastColor      = theme::kTextDim;
    bool        isStoreUnknown = false;

    ComposeForm   compose;
    BatchForm     batch;
    HighlightForm highlight;
    ProbeForm     probe;
};

// ---------------------------------------------------------------------------
// RPC plumbing
// ---------------------------------------------------------------------------

const char *StatusText(rpc::CallStatus s)
{
    switch (s)
    {
    case rpc::CallStatus::Ok:            return "OK";
    case rpc::CallStatus::Busy:          return "PIPE BUSY";
    case rpc::CallStatus::Disconnected:  return "DISCONNECTED";
    case rpc::CallStatus::Timeout:       return "TIMEOUT";
    case rpc::CallStatus::ProtocolError: return "PROTOCOL ERROR";
    case rpc::CallStatus::HandlerError:  return "REJECTED";
    }
    return "?";
}

// One call, with the reply rendered into the panel's feedback line. `outResult`
// receives the raw result bytes on Ok so callers can decode further.
rpc::CallStatus CallAndReport(app::App &a, DrawState &st, const char *method,
                              const ParamBuilder &params,
                              std::vector<uint8_t> &outResult)
{
    outResult.clear();
    std::vector<uint8_t> body;
    if (!EmitParams(params, body))
    {
        st.lastCall   = method;
        st.lastResult = "params too large to encode locally";
        st.lastColor  = theme::kBad;
        return rpc::CallStatus::ProtocolError;
    }
    const rpc::CallStatus status = a.rpc.Call(
        method, body.empty() ? nullptr : body.data(),
        static_cast<uint32_t>(body.size()), outResult, kCallTimeoutMs);

    st.lastCall = method;
    if (status == rpc::CallStatus::Ok)
    {
        Reader r(outResult.data(), outResult.size());
        st.lastResult = ValueToText(r);
        st.lastColor  = theme::kGood;
    }
    else if (status == rpc::CallStatus::HandlerError)
    {
        Reader r(outResult.data(), outResult.size());
        st.lastResult = std::string("rejected: ") + ValueToText(r);
        st.lastColor  = theme::kWarn;
    }
    else
    {
        st.lastResult = StatusText(status);
        st.lastColor  = theme::kBad;
    }
    return status;
}

// Decode {total, offset, returned, items}. `total` is the whole store under the
// requested scope; `returned` is this page. Rows append so the caller can page.
// Append one page of items. Rows accumulate across pages, so this appends
// rather than assigns — RefreshList clears once, before the first page.
bool ReadListItems(Reader &r, DrawState &st, uint32_t &outReturned)
{
    uint32_t n = 0;
    if (!r.ReadArrayHeader(n))
    {
        st.listError = "debug_draw_list items was not an array";
        return false;
    }
    for (uint32_t i = 0; i < n; ++i)
    {
        DrawRow row;
        if (!ReadDrawRow(r, row))
        {
            st.listError = "debug_draw_list item was malformed";
            return false;
        }
        st.rows.push_back(std::move(row));
    }
    outReturned = n;
    return true;
}

bool ParseListReply(Reader &r, uint32_t fields, DrawState &st, uint32_t &outReturned)
{
    using nxt::rpc::msgpack::StrEq;
    for (uint32_t i = 0; i < fields; ++i)
    {
        const char *k  = nullptr;
        uint32_t    kn = 0;
        if (!r.ReadString(k, kn))
        {
            st.listError = "debug_draw_list reply was malformed";
            return false;
        }
        if (StrEq(k, kn, "items"))
        {
            if (!ReadListItems(r, st, outReturned))
            {
                return false;
            }
            continue;
        }
        if (StrEq(k, kn, "total"))
        {
            int64_t v = 0;
            if (!r.ReadInt(v))
            {
                st.listError = "debug_draw_list total was not an integer";
                return false;
            }
            st.total = static_cast<uint32_t>(v);
            continue;
        }
        if (!r.SkipValue())
        {
            st.listError = "debug_draw_list reply was malformed";
            return false;
        }
    }
    return true;
}

// Walk every page of debug_draw_list. `limit` is capped at 128 by the agent, so
// a full 512-command store is four round trips; the page budget stops a
// runaway if `total` ever disagrees with what the pages return.
bool FetchListPage(app::App &a, DrawState &st, uint32_t offset, uint32_t &outReturned)
{
    ParamBuilder p;
    p.AddStr("scope", st.isScopeAll ? "all" : "mine");
    p.AddInt("offset", offset);
    p.AddInt("limit", kListPageMax);

    std::vector<uint8_t> body;
    if (!EmitParams(p, body))
    {
        return false;
    }
    std::vector<uint8_t> result;
    if (a.rpc.Call("debug_draw_list", body.data(), static_cast<uint32_t>(body.size()),
                   result, kCallTimeoutMs) != rpc::CallStatus::Ok)
    {
        st.listError = "debug_draw_list failed";
        return false;
    }
    Reader   r(result.data(), result.size());
    uint32_t fields = 0;
    if (!r.ReadMapHeader(fields))
    {
        st.listError = "debug_draw_list reply was not a map";
        return false;
    }
    return ParseListReply(r, fields, st, outReturned);
}

void RefreshList(app::App &a, DrawState &st)
{
    if (!a.rpc.IsConnected())
    {
        return;
    }
    st.rows.clear();
    st.total     = 0;
    st.listError.clear();
    uint32_t offset = 0;
    for (int page = 0; page < 4; ++page)
    {
        uint32_t returned = 0;
        if (!FetchListPage(a, st, offset, returned) || returned == 0)
        {
            break;
        }
        offset += returned;
        if (offset >= st.total)
        {
            break;
        }
    }
    st.isStoreUnknown = false;
}

void RefreshEnable(app::App &a, DrawState &st)
{
    if (!a.rpc.IsConnected())
    {
        return;
    }
    std::vector<uint8_t> result;
    // No params = read, don't write. The agent distinguishes the two by whether
    // `enabled` was present, so an empty map is the read.
    if (a.rpc.Call("debug_draw_enable", nullptr, 0, result, kCallTimeoutMs)
        != rpc::CallStatus::Ok)
    {
        return;
    }
    Reader   r(result.data(), result.size());
    uint32_t n = 0;
    if (!r.ReadMapHeader(n))
    {
        return;
    }
    for (uint32_t i = 0; i < n; ++i)
    {
        const char *k  = nullptr;
        uint32_t    kn = 0;
        if (!r.ReadString(k, kn))
        {
            return;
        }
        bool v = false;
        if (nxt::rpc::msgpack::StrEq(k, kn, "enabled") && r.ReadBool(v))
        {
            st.isEnabled      = v;
            st.hasEnableState = true;
            continue;
        }
        if (!r.SkipValue())
        {
            return;
        }
    }
}

void RefreshStats(app::App &a, DrawState &st)
{
    if (!a.rpc.IsConnected())
    {
        return;
    }
    std::vector<uint8_t> result;
    if (a.rpc.Call("debug_draw_stats", nullptr, 0, result, kCallTimeoutMs)
        != rpc::CallStatus::Ok)
    {
        return;
    }
    st.stats.clear();
    Reader   r(result.data(), result.size());
    uint32_t n = 0;
    if (!r.ReadMapHeader(n))
    {
        return;
    }
    for (uint32_t i = 0; i < n; ++i)
    {
        const char *k  = nullptr;
        uint32_t    kn = 0;
        if (!r.ReadString(k, kn))
        {
            return;
        }
        st.stats.emplace_back(std::string(k, kn), ValueToText(r));
    }
}

// ---------------------------------------------------------------------------
// Compose — building debug_draw_set params by kind
// ---------------------------------------------------------------------------

void AddStyling(ParamBuilder &p, const StylingForm &s)
{
    p.AddInt("color", static_cast<int64_t>(ArgbFromFloats(s.colorRgba)));
    p.AddInt("thickness", s.thickness);
    p.AddInt("z", s.z);
    p.AddInt("ttl_ms", s.ttlMs);
    p.AddBool("filled", s.isFilled);
}

// text / label / value are three spellings of ONE payload and exactly one is
// legal per command — the agent rejects any pair rather than ranking them. The
// caller picks with `useValue`; `decimals` is only legal alongside `value`.
void AddCaption(ParamBuilder &p, const ComposeForm &f, bool isComponent)
{
    if (f.useValue)
    {
        p.AddInt("value", f.value);
        p.AddInt("decimals", f.decimals);
    }
    else if (isComponent)
    {
        p.AddStr("label", f.label);
    }
    else
    {
        p.AddStr("text", f.text);
    }
    // `font` is rejected on every kind that draws no caption, so it is only
    // ever added here — a shared style object that carried it would make a
    // plain rect fail with "only text and component commands take a font".
    p.AddStr("font", kFontNames[f.fontIdx]);
}

uint32_t ParsePoints(const char *s, int32_t *out, uint32_t cap)
{
    uint32_t n = 0;
    const char *p = s;
    while (*p != '\0' && n < cap)
    {
        while (*p == ',' || *p == ' ' || *p == '\t')
        {
            ++p;
        }
        if (*p == '\0')
        {
            break;
        }
        char      *end = nullptr;
        const long v   = std::strtol(p, &end, 10);
        if (end == p)
        {
            break;
        }
        out[n++] = static_cast<int32_t>(v);
        p = end;
    }
    return n;
}

void AddComposeGeometry(const ComposeForm &f, int offsetX, ParamBuilder &p,
                        int32_t *pointBuf, uint32_t pointCap)
{
    const char *kind = kKindNames[f.kindIdx];
    if (std::strcmp(kind, "line") == 0)
    {
        p.AddInt("x1", f.p1[0] + offsetX); p.AddInt("y1", f.p1[1]);
        p.AddInt("x2", f.p2[0] + offsetX); p.AddInt("y2", f.p2[1]);
    }
    else if (std::strcmp(kind, "rect") == 0 || std::strcmp(kind, "ellipse") == 0)
    {
        p.AddInt("x", f.xy[0] + offsetX); p.AddInt("y", f.xy[1]);
        p.AddInt("w", f.wh[0]);           p.AddInt("h", f.wh[1]);
    }
    else if (std::strcmp(kind, "poly") == 0)
    {
        const uint32_t n = ParsePoints(f.points, pointBuf, pointCap);
        p.AddIntArray("points", pointBuf, n);
        p.AddBool("closed", f.isClosed);
    }
    else if (std::strcmp(kind, "text") == 0)
    {
        p.AddInt("x", f.xy[0] + offsetX); p.AddInt("y", f.xy[1]);
        AddCaption(p, f, false);
    }
    else   // component — no geometry at all; the agent re-resolves the rect
    {
        p.AddInt("iface", f.iface);
        p.AddInt("comp", f.comp);
        AddCaption(p, f, true);
    }
}

void BuildComposeParams(const ComposeForm &f, const char *key, int offsetX,
                        ParamBuilder &p, int32_t *pointBuf, uint32_t pointCap)
{
    p.AddStr("key", key);
    p.AddStr("kind", kKindNames[f.kindIdx]);
    if (f.isWorld)
    {
        p.AddStr("space", "world");
        // `plane` is accepted ONLY in world space — a screen-space command
        // carrying one is refused with "only world space takes a plane", not
        // ignored, so it must not be sent unconditionally.
        p.AddInt("plane", f.plane);
    }
    AddStyling(p, f.style);
    AddComposeGeometry(f, offsetX, p, pointBuf, pointCap);
}

// ---------------------------------------------------------------------------
// Batch. Encoded by hand rather than through ParamBuilder because `items` is an
// array of maps and the builder is flat.
//
// The optional corrupt item is deliberate fault injection: it writes an integer
// where the handler expects a map, which is the exact shape that makes the
// agent apply every item BEFORE it and then answer with an envelope error and
// no `count`. Nothing else in the tree can produce that state on demand, and a
// consumer that treats an envelope error as "nothing drew" is wrong about it.
// ---------------------------------------------------------------------------

bool EncodeBatch(const ComposeForm &f, const BatchForm &b, std::vector<uint8_t> &out)
{
    out.assign(256 * 1024, 0);
    Writer w(out.data(), out.size());
    w.WriteMapHeader(1);
    w.WriteCStr("items");
    w.WriteArrayHeader(static_cast<uint32_t>(b.itemCount));
    for (int i = 0; i < b.itemCount; ++i)
    {
        if (b.isCorrupting && i == b.corruptIndex)
        {
            w.WriteInt(0);
            continue;
        }
        char keyBuf[80];
        std::snprintf(keyBuf, sizeof(keyBuf), "%.48s-%d", f.key, i);
        ParamBuilder item;
        int32_t      points[64];
        BuildComposeParams(f, keyBuf, i * b.strideX, item, points, 64);
        w.WriteMapHeader(item.count);
        for (uint32_t j = 0; j < item.count; ++j)
        {
            WriteParamEntry(w, item.entries[j]);
        }
    }
    if (w.Overflowed())
    {
        out.clear();
        return false;
    }
    out.resize(w.BytesWritten());
    return true;
}

void SendBatch(app::App &a, DrawState &st)
{
    std::vector<uint8_t> body;
    if (!EncodeBatch(st.compose, st.batch, body))
    {
        st.lastCall   = "debug_draw_set_batch";
        st.lastResult = "batch too large to encode locally";
        st.lastColor  = theme::kBad;
        return;
    }
    std::vector<uint8_t> result;
    const rpc::CallStatus status = a.rpc.Call(
        "debug_draw_set_batch", body.data(), static_cast<uint32_t>(body.size()),
        result, kCallTimeoutMs);

    st.lastCall = "debug_draw_set_batch";
    if (status == rpc::CallStatus::Ok)
    {
        Reader r(result.data(), result.size());
        st.lastResult = ValueToText(r);
        // A cap hit answers a NORMAL reply carrying `dropped` and only the
        // first error string, so a client that checks the envelope alone reads
        // a batch that drew nothing as a success. Both halves are shown; the
        // amber is the reminder to read `dropped`.
        st.lastColor  = theme::kWarn;
    }
    else if (status == rpc::CallStatus::HandlerError)
    {
        Reader r(result.data(), result.size());
        st.lastResult = std::string("rejected: ") + ValueToText(r)
                      + "  — store state UNKNOWN, re-listed";
        st.lastColor  = theme::kBad;
        // An envelope error does NOT mean nothing drew: an item that is
        // structurally malformed leaves everything before it applied, and the
        // accumulated count dies with the call. The only honest recovery is to
        // re-read the store.
        st.isStoreUnknown = true;
        st.forceRefresh   = true;
    }
    else
    {
        st.lastResult = StatusText(status);
        st.lastColor  = theme::kBad;
    }
    ++st.setsIssued;
}

// ---------------------------------------------------------------------------
// Highlights
// ---------------------------------------------------------------------------

// Mirrors the agent's auto-key generation so the panel can show the key a
// call WILL get before it is sent. The formats are contract (PROTOCOL.md
// section 4.4, "Auto keys") — without them a caller who omitted `key` has no
// way to name the highlight again in order to clear it. Note the tile/area
// keys carry the caller's TILE values, not the sub-tile values the store holds.
void FormatAutoKey(const HighlightForm &f, char *out, size_t cap)
{
    switch (f.tab)
    {
    case 0:
        std::snprintf(out, cap, "comp:%d:%d", f.iface, f.comp);
        break;
    case 1:
        if (f.entityMode == 2)
        {
            std::snprintf(out, cap, "self");
        }
        else
        {
            std::snprintf(out, cap, "%s:%d",
                          (f.entityMode == 0) ? "npc" : "player", f.entityIndex);
        }
        break;
    case 2:
        std::snprintf(out, cap, "tile:%d:%d:%d", f.tileX, f.tileY, f.plane);
        break;
    default:
        std::snprintf(out, cap, "area:%d:%d:%d:%d:%d",
                      f.tileX, f.tileY, f.areaW, f.areaH, f.plane);
        break;
    }
}

const char *HighlightMethod(int tab)
{
    switch (tab)
    {
    case 0:  return "highlight_component";
    case 1:  return "highlight_entity";
    case 2:  return "highlight_tile";
    default: return "highlight_area";
    }
}

void BuildHighlightParams(const HighlightForm &f, ParamBuilder &p)
{
    if (f.key[0] != '\0')
    {
        p.AddStr("key", f.key);
    }
    AddStyling(p, f.style);
    switch (f.tab)
    {
    case 0:
        p.AddInt("iface", f.iface);
        p.AddInt("comp", f.comp);
        break;
    case 1:
        // Exactly one of npc / player / self — two is an error and none is an
        // error, because "which wins" is not a rule a caller can guess.
        if (f.entityMode == 0)      { p.AddInt("npc", f.entityIndex); }
        else if (f.entityMode == 1) { p.AddInt("player", f.entityIndex); }
        else                        { p.AddBool("self", true); }
        p.AddInt("w", f.entityW);
        p.AddInt("h", f.entityH);
        p.AddInt("plane", f.plane);
        break;
    case 2:
        // highlight_tile REFUSES w/h (use highlight_area) rather than
        // dropping them, so they are absent here by construction.
        p.AddInt("x", f.tileX);
        p.AddInt("y", f.tileY);
        p.AddInt("plane", f.plane);
        break;
    default:
        p.AddInt("x", f.tileX);
        p.AddInt("y", f.tileY);
        p.AddInt("w", f.areaW);
        p.AddInt("h", f.areaH);
        p.AddInt("plane", f.plane);
        break;
    }
    if (f.label[0] != '\0')
    {
        p.AddStr("label", f.label);
        p.AddStr("font", kFontNames[f.fontIdx]);
    }
}

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------

// Mirrors the producer's NeedsResolve (overlay/DrawTypes.h): only a component,
// entity or tile command, or anything in world space, is ever resolved.
//
// This matters more than it looks. A plain screen-space rect reports
// `resolved: false, rect: [0,0,0,0]` for its whole life and is drawing
// perfectly — its geometry is in `geom` and no projection is involved. A panel
// that renders a red "unresolved" badge on every screen rect is telling the
// user their working drawing is broken.
bool NeedsResolve(const DrawRow &row)
{
    return row.kind == "component" || row.kind == "entity" || row.kind == "tile"
        || row.space == "world";
}

ImU32 ProjectColor(const std::string &project)
{
    if (project == "ok")            { return theme::kGood; }
    if (project == "off_viewport")  { return theme::kWarn; }
    if (project == "behind_camera") { return theme::kBad; }
    if (project == "unavailable")   { return theme::kBad; }
    return theme::kTextDim;         // "n/a" — nothing was projected
}

void TtlText(int64_t ttlMs, char *out, size_t cap)
{
    // -1 is the agent's "no expiry" sentinel for a command set with ttl_ms: 0.
    // It is NOT documented in PROTOCOL.md's list-field paragraph; rendering it
    // raw gives the user "-1 ms remaining".
    if (ttlMs < 0)
    {
        std::snprintf(out, cap, "none");
    }
    else if (ttlMs >= 1000)
    {
        std::snprintf(out, cap, "%.1fs", static_cast<double>(ttlMs) / 1000.0);
    }
    else
    {
        std::snprintf(out, cap, "%lldms", static_cast<long long>(ttlMs));
    }
}

void DrawStateCell(const DrawRow &row)
{
    if (!NeedsResolve(row))
    {
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(theme::kTextDim));
        ImGui::TextUnformatted("direct");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Screen-space primitive: drawn straight from "
                              "geom, never resolved or projected. resolved=false "
                              "and rect=[0,0,0,0] are correct here.");
        }
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertU32ToFloat4(ProjectColor(row.project)));
    ImGui::TextUnformatted(row.project.empty() ? "?" : row.project.c_str());
    ImGui::PopStyleColor();
    // EITHER extent being zero makes the rect unpaintable, not both. Observed
    // live: a distant world footprint resolved to rect [1438, -555, 1, 0] —
    // one pixel wide, no height. An && here would have called that paintable.
    const bool isZeroExtent = row.isResolved && (row.rect[2] == 0 || row.rect[3] == 0);
    if (isZeroExtent)
    {
        ImGui::SameLine();
        theme::Pill("0-area", theme::kAccentSoft, theme::kWarn);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("resolved, but the rect has zero extent — a legal "
                              "answer for text and for a footprint far enough "
                              "away that its corners round to one pixel. The "
                              "position is real; there is nothing to fill.");
        }
    }
}

void RectText(const DrawRow &row, char *out, size_t cap)
{
    if (NeedsResolve(row) && !row.isResolved)
    {
        // rect is zeroed — not stale — whenever resolved is false, so there is
        // no last-good rectangle to show and "0,0,0,0" would read as one.
        std::snprintf(out, cap, "—");
        return;
    }
    if (!NeedsResolve(row))
    {
        std::snprintf(out, cap, "geom %lld,%lld,%lld,%lld",
                      static_cast<long long>(row.geom[0]),
                      static_cast<long long>(row.geom[1]),
                      static_cast<long long>(row.geom[2]),
                      static_cast<long long>(row.geom[3]));
        return;
    }
    std::snprintf(out, cap, "%lld,%lld %lldx%lld",
                  static_cast<long long>(row.rect[0]),
                  static_cast<long long>(row.rect[1]),
                  static_cast<long long>(row.rect[2]),
                  static_cast<long long>(row.rect[3]));
}

// theme::KeyLine right-aligns its value across the WHOLE available width. That
// reads well in the narrow cards every other panel uses; this panel's cards
// span the full docked width, where it would put a label and its number at
// opposite ends of the monitor. So every KeyLine block here gets a capped
// column. `rows` of 0 fills the remaining height. Pair with ImGui::EndChild.
void BeginKeyLineColumn(const char *id, float rows)
{
    const float wanted = ImGui::GetFontSize() * 26.0f;
    const float avail  = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild(id, ImVec2((avail < wanted) ? avail : wanted,
                                 (rows > 0.0f) ? ImGui::GetFrameHeight() * rows : 0.0f));
}

const char *StatValue(const DrawState &st, const char *name)
{
    for (const auto &kv : st.stats)
    {
        if (kv.first == name)
        {
            return kv.second.c_str();
        }
    }
    return "—";
}

// ---------------------------------------------------------------------------
// Frame bookkeeping
// ---------------------------------------------------------------------------

void TrackConnection(app::App &a, DrawState &st)
{
    const bool isConnected = a.rpc.IsConnected();
    // Ownership is per CONNECTION: the agent drops everything a connection drew
    // the moment that connection closes. The shared client reconnecting is
    // therefore a silent mass-delete of this panel's work, and a panel that
    // does not say so looks broken instead of correct.
    if (st.wasConnected && !isConnected && st.setsIssued > 0)
    {
        st.didLoseDrawings = true;
    }
    if (!st.wasConnected && isConnected)
    {
        st.setsIssued      = 0;
        st.forceRefresh    = true;
        st.forceStats      = true;
        st.hasEnableState  = false;
    }
    st.wasConnected = isConnected;
}

void AutoRefresh(app::App &a, DrawState &st)
{
    bool isDue      = st.forceRefresh;
    st.forceRefresh = false;
    if (st.isAutoRefresh && a.rpc.IsConnected())
    {
        st.refreshAccum += ImGui::GetIO().DeltaTime;
        const float period = (st.refreshHz > 0.1f) ? (1.0f / st.refreshHz) : 1.0f;
        if (st.refreshAccum >= period)
        {
            st.refreshAccum = 0.0f;
            isDue           = true;
        }
    }
    if (!isDue)
    {
        return;
    }
    RefreshList(a, st);
    RefreshStats(a, st);
    if (!st.hasEnableState)
    {
        RefreshEnable(a, st);
    }
}

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------

void DrawRefreshControls(DrawState &st)
{
    ImGui::PushItemWidth(ImGui::GetFontSize() * 7);
    const char *scopes[] = { "mine", "all" };
    int scopeIdx = st.isScopeAll ? 1 : 0;
    if (ImGui::Combo("scope", &scopeIdx, scopes, 2))
    {
        st.isScopeAll   = (scopeIdx == 1);
        st.forceRefresh = true;
    }
    ImGui::PopItemWidth();
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("\"all\" lists every connection's commands — this is "
                          "how you watch what a script is drawing. \"mine\" is "
                          "only what this debugger drew.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("auto", &st.isAutoRefresh);
    ImGui::SameLine();
    ImGui::PushItemWidth(ImGui::GetFontSize() * 8);
    ImGui::SliderFloat("Hz", &st.refreshHz, 0.5f, 20.0f, "%.1f");
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
    {
        st.forceRefresh = true;
    }
}

void DrawHeaderControls(app::App &a, DrawState &st)
{
    const bool isOff = a.rpc.IsConnected() && st.hasEnableState && !st.isEnabled;
    theme::StatusDot(isOff ? theme::kWarn
                           : (a.rpc.IsConnected() ? theme::kGood : theme::kTextDim),
                     a.rpc.IsConnected(), 0.4f);
    ImGui::SameLine();

    char countBuf[48];
    std::snprintf(countBuf, sizeof(countBuf), "%u / %u", st.total, kMaxDrawCmds);
    theme::HeroStat("commands in store", countBuf,
                    (st.total >= kMaxDrawCmds) ? theme::kBad : theme::kTextHi);

    ImGui::SameLine();
    ImGui::BeginGroup();
    if (theme::Toggle("##ddenable", &st.isEnabled))
    {
        ParamBuilder p;
        p.AddBool("enabled", st.isEnabled);
        std::vector<uint8_t> result;
        CallAndReport(a, st, "debug_draw_enable", p, result);
        st.hasEnableState = true;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(st.isEnabled ? "overlay on" : "overlay OFF");
    ImGui::EndGroup();

    ImGui::Spacing();
    DrawRefreshControls(st);
}

// Reading width is capped rather than left to the window: this panel's cards
// span the full docked width, and a banner running 1800px edge to edge is
// exhausting to read. ~45em is a serviceable proxy for 85 characters.
void Banner(ImU32 color, const char *text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 45.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void DrawBanners(app::App &a, DrawState &st)
{
    if (!a.rpc.IsConnected())
    {
        Banner(theme::kTextDim,
               "Not connected to the agent's RPC pipe. Attach to a client and "
               "this panel starts polling the store.");
        return;
    }
    if (st.didLoseDrawings)
    {
        Banner(theme::kWarn,
               "! The RPC connection cycled, so the agent dropped everything "
               "this panel had drawn. Drawings are owned by the connection, "
               "not by the debugger.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss"))
        {
            st.didLoseDrawings = false;
        }
    }
    if (st.isStoreUnknown)
    {
        Banner(theme::kBad,
               "! A batch failed at the envelope. Items before the failure were "
               "already applied and the count died with the call — the table "
               "below is a fresh read, not an assumption.");
    }
    if (!st.listError.empty())
    {
        Banner(theme::kBad, st.listError.c_str());
    }
}

void DrawHeaderCard(app::App &a, DrawState &st)
{
    if (!theme::BeginCard("dd.head", "DRAW STORE"))
    {
        theme::EndCard();
        return;
    }
    DrawHeaderControls(a, st);
    ImGui::Spacing();

    char gauge[64];
    std::snprintf(gauge, sizeof(gauge), "%s / %u text   %s / %u poly",
                  StatValue(st, "text_slots_used"), kMaxTextSlots,
                  StatValue(st, "poly_slots_used"), kMaxPolySlots);
    BeginKeyLineColumn("##ddgauges", 3.0f);
    theme::KeyLine("side slots", gauge);
    theme::KeyLine("dropped (cap hits)", StatValue(st, "dropped"));
    theme::KeyLine("resolve failures", StatValue(st, "resolve_failures"));
    ImGui::EndChild();

    ImGui::Spacing();
    DrawBanners(a, st);

    if (!st.lastCall.empty())
    {
        ImGui::Spacing();
        theme::Subheading(st.lastCall.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(st.lastColor));
        ImGui::TextWrapped("%s", st.lastResult.c_str());
        ImGui::PopStyleColor();
    }
    theme::EndCard();
}

// ---------------------------------------------------------------------------
// Store table
// ---------------------------------------------------------------------------

void DrawStoreRow(DrawState &st, int index)
{
    const DrawRow &row = st.rows[static_cast<size_t>(index)];
    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    if (ImGui::Selectable(row.key.c_str(), st.selectedRow == index,
                          ImGuiSelectableFlags_SpanAllColumns))
    {
        st.selectedRow = index;
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(row.kind.c_str());
    ImGui::TableSetColumnIndex(2);
    ImGui::TextUnformatted(row.space.c_str());
    ImGui::TableSetColumnIndex(3);
    ImGui::Text("%lld", static_cast<long long>(row.z));

    ImGui::TableSetColumnIndex(4);
    const ImVec2 p  = ImGui::GetCursorScreenPos();
    const float  sz = ImGui::GetFontSize() * 0.8f;
    ImGui::GetWindowDrawList()->AddRectFilled(
        p, ImVec2(p.x + sz, p.y + sz), ImColFromArgb(row.color), 2.0f);
    ImGui::Dummy(ImVec2(sz, sz));
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("0x%08X  (0xAARRGGBB)", row.color);
    }

    ImGui::TableSetColumnIndex(5);
    char buf[64];
    TtlText(row.ttlMs, buf, sizeof(buf));
    ImGui::TextUnformatted(buf);

    ImGui::TableSetColumnIndex(6);
    DrawStateCell(row);

    ImGui::TableSetColumnIndex(7);
    RectText(row, buf, sizeof(buf));
    ImGui::TextUnformatted(buf);

    ImGui::TableSetColumnIndex(8);
    ImGui::TextUnformatted(row.text.c_str());
}

void DrawStoreTable(DrawState &st)
{
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg
                                     | ImGuiTableFlags_Borders
                                     | ImGuiTableFlags_ScrollY
                                     | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##ddstore", 9, kFlags, ImVec2(0, 0)))
    {
        return;
    }
    const float em = ImGui::GetFontSize();
    ImGui::TableSetupColumn("key",   ImGuiTableColumnFlags_WidthFixed, em * 10);
    ImGui::TableSetupColumn("kind",  ImGuiTableColumnFlags_WidthFixed, em * 4.5f);
    ImGui::TableSetupColumn("space", ImGuiTableColumnFlags_WidthFixed, em * 3.5f);
    ImGui::TableSetupColumn("z",     ImGuiTableColumnFlags_WidthFixed, em * 2);
    ImGui::TableSetupColumn("col",   ImGuiTableColumnFlags_WidthFixed, em * 2);
    ImGui::TableSetupColumn("ttl left", ImGuiTableColumnFlags_WidthFixed, em * 4);
    ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed, em * 7);
    ImGui::TableSetupColumn("rect / geom", ImGuiTableColumnFlags_WidthFixed, em * 10);
    ImGui::TableSetupColumn("text",  ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    for (int i = 0; i < static_cast<int>(st.rows.size()); ++i)
    {
        DrawStoreRow(st, i);
    }
    ImGui::EndTable();
}

void DrawStoreActions(app::App &a, DrawState &st)
{
    const bool hasSelection = st.selectedRow >= 0
                           && st.selectedRow < static_cast<int>(st.rows.size());
    if (!hasSelection)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Clear selected") && hasSelection)
    {
        ParamBuilder p;
        p.AddStr("key", st.rows[static_cast<size_t>(st.selectedRow)].key.c_str());
        std::vector<uint8_t> result;
        CallAndReport(a, st, "debug_draw_clear", p, result);
        st.forceRefresh = true;
    }
    if (!hasSelection)
    {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered() && hasSelection)
    {
        ImGui::SetTooltip("debug_draw_clear only ever removes YOUR OWN "
                          "commands — selecting another connection's row and "
                          "clearing it reports removed: 0.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear all (mine)"))
    {
        ParamBuilder p;
        p.AddStr("scope", "mine");
        std::vector<uint8_t> result;
        CallAndReport(a, st, "debug_draw_clear_all", p, result);
        st.forceRefresh = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear all (every connection)"))
    {
        ParamBuilder p;
        p.AddStr("scope", "all");
        std::vector<uint8_t> result;
        CallAndReport(a, st, "debug_draw_clear_all", p, result);
        st.forceRefresh = true;
    }
}

void DrawStoreCard(app::App &a, DrawState &st)
{
    if (!theme::BeginCard("dd.store", "COMMANDS", theme::kAccent, true))
    {
        theme::EndCard();
        return;
    }
    DrawStoreActions(a, st);
    ImGui::Spacing();
    if (st.rows.empty())
    {
        theme::Subheading(a.rpc.IsConnected()
            ? "Nothing in the store. Draw something from the Compose tab, or "
              "switch scope to \"all\" to watch a script's commands appear."
            : "Attach to a client to read the store.");
        theme::EndCard();
        return;
    }
    DrawStoreTable(st);
    theme::EndCard();
}

// ---------------------------------------------------------------------------
// Compose tab
// ---------------------------------------------------------------------------

// Key length is measured in UTF-8 BYTES and text in UTF-16 UNITS — two
// different units for two fields that sit next to each other in the same form.
// Over-long text FAILS rather than truncating, so both counters warn before the
// call rather than after the rejection.
uint32_t Utf16Units(const char *utf8)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    return (n > 0) ? static_cast<uint32_t>(n - 1) : 0;
}

void DrawLimitCaption(uint32_t used, uint32_t cap, const char *unit)
{
    const bool isOver = used > cap;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(
        isOver ? theme::kBad : theme::kTextDim));
    ImGui::Text("%u / %u %s%s", used, cap, unit, isOver ? "  — will be REJECTED" : "");
    ImGui::PopStyleColor();
}

void DrawStylingForm(StylingForm &s)
{
    ImGui::ColorEdit4("color", s.colorRgba,
                      ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview);
    ImGui::SliderInt("thickness", &s.thickness, 1, 64);
    ImGui::Checkbox("filled", &s.isFilled);
    ImGui::SameLine();
    ImGui::PushItemWidth(ImGui::GetFontSize() * 8);
    ImGui::InputInt("z", &s.z);
    ImGui::PopItemWidth();
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Signed 16-bit. Outside -32768..32767 the call is "
                          "REJECTED, not clamped. Higher z paints later, so it "
                          "paints on top.");
    }
    ImGui::InputInt("ttl_ms", &s.ttlMs);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("0 means \"until replaced, cleared, or my connection "
                          "closes\" — not permanent. Omitting it would default "
                          "to 3000ms; this panel always sends it.");
    }
}

void DrawComposeGeometryForm(ComposeForm &f)
{
    const char *kind = kKindNames[f.kindIdx];
    if (std::strcmp(kind, "line") == 0)
    {
        ImGui::InputInt2("x1,y1", f.p1);
        ImGui::InputInt2("x2,y2", f.p2);
    }
    else if (std::strcmp(kind, "rect") == 0 || std::strcmp(kind, "ellipse") == 0)
    {
        ImGui::InputInt2("x,y", f.xy);
        ImGui::InputInt2("w,h", f.wh);
    }
    else if (std::strcmp(kind, "poly") == 0)
    {
        ImGui::InputText("points", f.points, sizeof(f.points));
        theme::Subheading("flat x,y pairs — 2 to 32 of them. Rejected in world "
                          "space (\"world space does not support poly\").");
        ImGui::Checkbox("closed", &f.isClosed);
    }
    else if (std::strcmp(kind, "text") == 0)
    {
        ImGui::InputInt2("x,y", f.xy);
    }
    else
    {
        ImGui::InputInt("iface", &f.iface);
        ImGui::InputInt("comp", &f.comp);
        theme::Subheading("No geometry: the agent stores (iface, comp) and "
                          "re-resolves the rect every tick, because the client "
                          "recomputes it on every layout pass.");
    }
}

void DrawCaptionForm(ComposeForm &f, bool isComponent)
{
    ImGui::Checkbox("send value+decimals instead of text", &f.useValue);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("There is no float on this wire. value:1234 "
                          "decimals:2 renders \"12.34\". text / label / value "
                          "are three spellings of one payload and exactly one "
                          "is legal per command.");
    }
    if (f.useValue)
    {
        ImGui::InputInt("value", &f.value);
        ImGui::SliderInt("decimals", &f.decimals, 0, 9);
    }
    else if (isComponent)
    {
        ImGui::InputText("label", f.label, sizeof(f.label));
        DrawLimitCaption(Utf16Units(f.label), kMaxTextUnits, "UTF-16 units");
        theme::Subheading("A component names its caption \"label\", not "
                          "\"text\" — sending text is rejected by name.");
    }
    else
    {
        ImGui::InputText("text", f.text, sizeof(f.text));
        DrawLimitCaption(Utf16Units(f.text), kMaxTextUnits, "UTF-16 units");
    }
    ImGui::Combo("font", &f.fontIdx, kFontNames, kFontCount);
}

void SendCompose(app::App &a, DrawState &st)
{
    ParamBuilder p;
    int32_t      points[64];
    BuildComposeParams(st.compose, st.compose.key, 0, p, points, 64);
    std::vector<uint8_t> result;
    CallAndReport(a, st, "debug_draw_set", p, result);
    ++st.setsIssued;
    st.forceRefresh = true;
}

// Key, kind and space — what the command IS, before any geometry or styling.
void DrawComposeIdentity(ComposeForm &f)
{
    ImGui::InputText("key", f.key, sizeof(f.key));
    DrawLimitCaption(static_cast<uint32_t>(std::strlen(f.key)), kMaxKeyBytes,
                     "UTF-8 bytes");
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Keys are an identity, not a handle: setting the same "
                          "key again REPLACES the command. Nothing to free.");
    }
    ImGui::Combo("kind", &f.kindIdx, kKindNames, kKindCount);
    ImGui::Checkbox("world space", &f.isWorld);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("World coordinates are fixed-point tile*256+subtile. "
                          "A world rect is a ground FOOTPRINT in tiles, not a "
                          "screen-sized billboard.");
    }
    if (!f.isWorld)
    {
        return;
    }
    ImGui::SliderInt("plane", &f.plane, 0, 3);
    theme::Subheading("Only world space takes a plane. A plane other than the "
                      "player's resolves \"unavailable\" rather than being "
                      "drawn wrong at the player's floor.");
}

void DrawComposeTab(app::App &a, DrawState &st)
{
    ComposeForm &f    = st.compose;
    const char  *kind = kKindNames[f.kindIdx];

    DrawComposeIdentity(f);
    theme::AccentRule();
    DrawComposeGeometryForm(f);
    const bool isCaptioned = (std::strcmp(kind, "text") == 0)
                          || (std::strcmp(kind, "component") == 0);
    if (isCaptioned)
    {
        theme::AccentRule();
        DrawCaptionForm(f, std::strcmp(kind, "component") == 0);
    }
    theme::AccentRule();
    DrawStylingForm(f.style);

    ImGui::Spacing();
    if (!a.rpc.IsConnected())
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Send debug_draw_set", ImVec2(-FLT_MIN, 0)))
    {
        SendCompose(a, st);
    }
    if (!a.rpc.IsConnected())
    {
        ImGui::EndDisabled();
    }
}

// ---------------------------------------------------------------------------
// Batch tab
// ---------------------------------------------------------------------------

void DrawBatchTab(app::App &a, DrawState &st)
{
    BatchForm &b = st.batch;
    theme::Subheading("Sends the Compose command N times, keys suffixed -0..N-1 "
                      "and each offset along x. At most 256 items per call.");
    ImGui::SliderInt("items", &b.itemCount, 1, 300);
    if (b.itemCount > static_cast<int>(kMaxBatchItems))
    {
        Banner(theme::kWarn,
               "Over 256: the handler returns BEFORE touching the store, so "
               "nothing is applied and the reply is an envelope error.");
    }
    ImGui::SliderInt("x stride", &b.strideX, 0, 64);

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Fault injection"))
    {
        ImGui::Checkbox("corrupt one item", &b.isCorrupting);
        ImGui::SliderInt("at index", &b.corruptIndex, 0, b.itemCount - 1);
        Banner(theme::kTextDim,
               "Writes an integer where the handler expects a map. Every item "
               "BEFORE it is already applied; reading stops there, the call "
               "errors, and the tally it had accumulated dies with it. This is "
               "the one wire behaviour a consumer is most likely to get wrong, "
               "and nothing else in the tree reproduces it on demand.");
    }

    ImGui::Spacing();
    if (!a.rpc.IsConnected())
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Send debug_draw_set_batch", ImVec2(-FLT_MIN, 0)))
    {
        SendBatch(a, st);
        st.forceRefresh = true;
    }
    if (!a.rpc.IsConnected())
    {
        ImGui::EndDisabled();
    }
    ImGui::Spacing();
    Banner(theme::kTextDim,
           "A per-item validation failure is NOT an envelope error: it answers "
           "a normal reply carrying count, dropped and the FIRST error string "
           "only. Read both.");
}

// ---------------------------------------------------------------------------
// Highlights tab
// ---------------------------------------------------------------------------

void DrawHighlightBody(HighlightForm &f)
{
    switch (f.tab)
    {
    case 0:
        ImGui::InputInt("iface", &f.iface);
        ImGui::InputInt("comp", &f.comp);
        theme::Subheading("Screen space by definition — its rect is re-read "
                          "from the live interface tree, so `project` is n/a "
                          "and no camera is involved.");
        break;
    case 1:
    {
        const char *modes[] = { "npc", "player", "self" };
        ImGui::Combo("target", &f.entityMode, modes, 3);
        if (f.entityMode != 2)
        {
            ImGui::InputInt("index", &f.entityIndex);
        }
        else
        {
            theme::Subheading("`self` carries no index: the local player's slot "
                              "is re-resolved every tick, so it survives a "
                              "world hop that would strand a stored index.");
        }
        ImGui::InputInt("w (tiles)", &f.entityW);
        ImGui::InputInt("h (tiles)", &f.entityH);
        ImGui::SliderInt("plane", &f.plane, 0, 3);
        theme::Subheading("An entity carries its own height, so entity "
                          "highlights are exact on slopes and stairs where a "
                          "raw world primitive is not. This is the recommended "
                          "path.");
        break;
    }
    case 2:
        ImGui::InputInt("x (tiles)", &f.tileX);
        ImGui::InputInt("y (tiles)", &f.tileY);
        ImGui::SliderInt("plane", &f.plane, 0, 3);
        theme::Subheading("Exactly one tile. w/h are REFUSED here — use "
                          "highlight_area — rather than silently dropped.");
        break;
    default:
        ImGui::InputInt("x (tiles)", &f.tileX);
        ImGui::InputInt("y (tiles)", &f.tileY);
        ImGui::InputInt("w (tiles)", &f.areaW);
        ImGui::InputInt("h (tiles)", &f.areaH);
        ImGui::SliderInt("plane", &f.plane, 0, 3);
        break;
    }
}

void DrawHighlightsTab(app::App &a, DrawState &st)
{
    HighlightForm &f = st.highlight;
    const char *tabs[] = { "component", "entity", "tile", "area" };
    ImGui::Combo("helper", &f.tab, tabs, 4);
    theme::AccentRule();
    DrawHighlightBody(f);

    theme::AccentRule();
    ImGui::InputText("key (blank = auto)", f.key, sizeof(f.key));
    char autoKey[64];
    FormatAutoKey(f, autoKey, sizeof(autoKey));
    char caption[128];
    std::snprintf(caption, sizeof(caption), "auto key: %s", autoKey);
    theme::KeyLine("generated", (f.key[0] == '\0') ? autoKey : f.key);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s\nThe format is contract: without it, a caller who "
                          "omitted `key` has no way to name the highlight again "
                          "in order to clear it. The reply echoes the key "
                          "actually used.", caption);
    }
    ImGui::InputText("label", f.label, sizeof(f.label));
    DrawLimitCaption(Utf16Units(f.label), kMaxTextUnits, "UTF-16 units");
    ImGui::Combo("font", &f.fontIdx, kFontNames, kFontCount);
    theme::AccentRule();
    DrawStylingForm(f.style);

    ImGui::Spacing();
    if (!a.rpc.IsConnected())
    {
        ImGui::BeginDisabled();
    }
    char label[64];
    std::snprintf(label, sizeof(label), "Send %s", HighlightMethod(f.tab));
    if (ImGui::Button(label, ImVec2(-FLT_MIN, 0)))
    {
        ParamBuilder p;
        BuildHighlightParams(f, p);
        std::vector<uint8_t> result;
        CallAndReport(a, st, HighlightMethod(f.tab), p, result);
        ++st.setsIssued;
        st.forceRefresh = true;
    }
    if (!a.rpc.IsConnected())
    {
        ImGui::EndDisabled();
    }
}

// ---------------------------------------------------------------------------
// Probe tab
// ---------------------------------------------------------------------------

// `key` and `x/y/w/h` are refused TOGETHER rather than ranked, so the form is
// one or the other and never both.
void DrawProbeRegionForm(ProbeForm &f)
{
    ImGui::Checkbox("address by key", &f.isByKey);
    if (f.isByKey)
    {
        ImGui::InputText("key", f.key, sizeof(f.key));
        ImGui::InputInt("inset", &f.inset);
        theme::Subheading("Probes wherever the named command's projection "
                          "actually put it. The key must name a WORLD command "
                          "this connection owns that is currently resolved — "
                          "anything else errors by name.");
        return;
    }
    ImGui::InputInt2("x,y", f.xy);
    ImGui::InputInt2("w,h", f.wh);
    theme::Subheading("Client-space pixels. w and h must be 1..4096.");
}

void SendProbe(app::App &a, DrawState &st)
{
    const ProbeForm &f = st.probe;
    ParamBuilder     p;
    if (f.isByKey)
    {
        p.AddStr("key", f.key);
        p.AddInt("inset", f.inset);
    }
    else
    {
        p.AddInt("x", f.xy[0]); p.AddInt("y", f.xy[1]);
        p.AddInt("w", f.wh[0]); p.AddInt("h", f.wh[1]);
    }
    p.AddInt("color", static_cast<int64_t>(ArgbFromFloats(f.colorRgba)));
    p.AddInt("source", f.sourceIdx);
    std::vector<uint8_t> result;
    CallAndReport(a, st, "debug_draw_probe_pixels", p, result);
}

void DrawProbeTab(app::App &a, DrawState &st)
{
    ProbeForm &f = st.probe;
    theme::Subheading("A verification surface, not a drawing one: it counts "
                      "pixels matching a colour inside the overlay's target "
                      "client rect.");
    DrawProbeRegionForm(f);
    ImGui::ColorEdit4("match colour", f.colorRgba,
                      ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview);
    const char *sources[] = { "0 - desktop screen", "1 - overlay surface" };
    ImGui::Combo("source", &f.sourceIdx, sources, 2);
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImGui::ColorConvertU32ToFloat4(theme::kTextDim));
    ImGui::TextWrapped(f.sourceIdx == 1
        ? "Source 1 samples the overlay's own surface: nothing can occlude it, "
          "and it also returns `exact` (a full 32-bit match including alpha)."
        : "Source 0 reads whatever is frontmost. `occluded: true` means "
          "UNKNOWN, never \"did not draw\" — gate on it before believing any "
          "count, including a partial one.");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    if (!a.rpc.IsConnected())
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Send debug_draw_probe_pixels", ImVec2(-FLT_MIN, 0)))
    {
        SendProbe(a, st);
    }
    if (!a.rpc.IsConnected())
    {
        ImGui::EndDisabled();
    }
}

// ---------------------------------------------------------------------------
// Stats + limits tabs
// ---------------------------------------------------------------------------

void DrawStatsTab(app::App &a, DrawState &st)
{
    if (ImGui::Button("Refresh stats"))
    {
        RefreshStats(a, st);
    }
    ImGui::Spacing();
    if (st.stats.empty())
    {
        theme::Subheading("No stats yet — attach and refresh.");
        return;
    }
    theme::Subheading("frames and backend_presents answer DIFFERENT questions; "
                      "a frame with an empty dirty region uploads nothing, so "
                      "they differ in normal operation. Do not check equality.");
    ImGui::Spacing();
    BeginKeyLineColumn("##ddstatcol", 0.0f);
    for (const auto &kv : st.stats)
    {
        theme::KeyLine(kv.first.c_str(), kv.second.c_str());
    }
    ImGui::EndChild();
}

// Signature matches the other tabs so the table below can hold it; neither
// parameter is used because every number here is a compile-time constant.
void DrawLimitsTab(app::App &, DrawState &)
{
    theme::Subheading("Every cap below is a hard error, not a silent "
                      "truncation — but how you are told depends on which call "
                      "you made.");
    ImGui::Spacing();
    BeginKeyLineColumn("##ddlimitcol", 0.0f);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%u", kMaxDrawCmds);
    theme::KeyLine("retained commands", buf);
    std::snprintf(buf, sizeof(buf), "%u", kMaxTextSlots);
    theme::KeyLine("text slots", buf);
    std::snprintf(buf, sizeof(buf), "%u", kMaxPolySlots);
    theme::KeyLine("polyline slots", buf);
    std::snprintf(buf, sizeof(buf), "%u", kMaxBatchItems);
    theme::KeyLine("items per batch", buf);
    std::snprintf(buf, sizeof(buf), "%u rows", kListPageMax);
    theme::KeyLine("list page limit", buf);
    theme::AccentRule();
    std::snprintf(buf, sizeof(buf), "%u UTF-8 bytes", kMaxKeyBytes);
    theme::KeyLine("key length", buf);
    std::snprintf(buf, sizeof(buf), "%u UTF-16 units", kMaxTextUnits);
    theme::KeyLine("text length", buf);
    theme::Subheading("Different units. Over-long text FAILS rather than "
                      "truncating.");
    theme::AccentRule();
    std::snprintf(buf, sizeof(buf), "+/- %d", kMaxScreenCoord);
    theme::KeyLine("screen coordinate", buf);
    std::snprintf(buf, sizeof(buf), "%d", kMaxScreenExtent);
    theme::KeyLine("screen extent", buf);
    std::snprintf(buf, sizeof(buf), "+/- %d  (%d tiles)", kMaxWorldCoord, kMaxWorldTile);
    theme::KeyLine("world coordinate", buf);
    theme::Subheading("TWO bounds, not interchangeable. The screen bound also "
                      "gates the coordinates a projection produces a tick "
                      "later, which is what makes a projected point safe to "
                      "hand back: a point behind the camera projects to "
                      "INT32_MIN and is refused rather than clamped.");
    theme::AccentRule();
    theme::KeyLine("a key longer than 47 bytes", "errors WITHOUT incrementing dropped");
    theme::Subheading("So does an out-of-range coordinate and a malformed "
                      "command: they are rejected before the store is touched. "
                      "Do not use the dropped counter to detect them.");
    ImGui::EndChild();
}

void DrawControlTabs(app::App &a, DrawState &st)
{
    if (!theme::BeginCard("dd.drive", "DRIVE", theme::kAccent, true))
    {
        theme::EndCard();
        return;
    }
    // A table of tabs rather than six near-identical Begin/End blocks — the
    // same shape App::panels uses for the panels themselves, and it keeps the
    // scrolling-child wrapper in exactly one place.
    struct DriveTab
    {
        const char *label;
        const char *childId;
        void      (*draw)(app::App &, DrawState &);
    };
    static constexpr DriveTab kTabs[] = {
        { "Compose",    "##ddcompose", &DrawComposeTab    },
        { "Batch",      "##ddbatch",   &DrawBatchTab      },
        { "Highlights", "##ddhl",      &DrawHighlightsTab },
        { "Probe",      "##ddprobe",   &DrawProbeTab      },
        { "Stats",      "##ddstats",   &DrawStatsTab      },
        { "Limits",     "##ddlimits",  &DrawLimitsTab     },
    };

    if (ImGui::BeginTabBar("##ddtabs"))
    {
        for (const DriveTab &tab : kTabs)
        {
            if (!ImGui::BeginTabItem(tab.label))
            {
                continue;
            }
            ImGui::BeginChild(tab.childId);
            tab.draw(a, st);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    theme::EndCard();
}

}   // namespace

void DrawDebugDraw(app::App &a)
{
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 74,
                                    ImGui::GetFontSize() * 42),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Debug Draw"))
    {
        ImGui::End();
        return;
    }

    static DrawState st;
    TrackConnection(a, st);
    AutoRefresh(a, st);

    DrawHeaderCard(a, st);
    ImGui::Spacing();

    const float leftWidth = ImGui::GetContentRegionAvail().x * 0.58f;
    ImGui::BeginChild("##ddleft", ImVec2(leftWidth, 0));
    DrawStoreCard(a, st);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##ddright", ImVec2(0, 0));
    DrawControlTabs(a, st);
    ImGui::EndChild();

    ImGui::End();
}

}   // namespace nxtdbg::panels
