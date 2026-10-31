// Wire tests: the open-interface decode (wire v23 openIfaceFlags), the
// absolute layout figures it moved, and the JSON parser's nesting cap (the
// control pipe hands that parser untrusted lines).
//
// No framework on purpose -- this target links nothing but the wire decode
// module and the JSON parser, so it builds on any clone. Each CHECK prints what it compared; the
// exit code is the number of failures, which is what ctest reads.

#include "cache/Json.h"
#include "wire/OpenIface.h"

// The same compile-time pins the debugger builds with: a layout that drifts
// from PROTOCOL.md fails this target too, before a single test runs.
#include "wire/ProtocolDocPins.h"

#include "ipc/SharedLayout.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace
{

int failures = 0;

void Check(bool isOk, const char *what, long long got, long long want)
{
    std::printf("%s  %-48s got %lld, want %lld\n", isOk ? "PASS" : "FAIL", what, got, want);
    if (!isOk)
    {
        ++failures;
    }
}

void CheckEq(const char *what, long long got, long long want)
{
    Check(got == want, what, got, want);
}

void CheckStr(const char *what, const char *got, const char *want)
{
    const bool isOk = std::strcmp(got, want) == 0;
    std::printf("%s  %-48s got \"%s\", want \"%s\"\n", isOk ? "PASS" : "FAIL", what, got, want);
    if (!isOk)
    {
        ++failures;
    }
}

// Runtime echo of the v24 figures, so the test log names the numbers the
// brief and PROTOCOL.md quote. The static_asserts in ProtocolDocPins.h are the
// gate; these make the result readable.
void TestLayoutPins()
{
    using nxt::ipc::Snapshot;
    CheckEq("kProtocolVersion", nxt::ipc::kProtocolVersion, 24);
    CheckEq("sizeof(Snapshot)", sizeof(Snapshot), 452800);
    CheckEq("offsetof(openIfaces)", offsetof(Snapshot, openIfaces), 361352);
    CheckEq("offsetof(openIfaceFlags)", offsetof(Snapshot, openIfaceFlags), 362376);
    CheckEq("sizeof(openIfaceFlags)", sizeof(Snapshot::openIfaceFlags), nxt::ipc::kOpenIfaceCap);
    CheckEq("offsetof(_padAfterOpenIfaces)", offsetof(Snapshot, _padAfterOpenIfaces), 362632);
    CheckEq("offsetof(groundItemCount)", offsetof(Snapshot, groundItemCount), 362636);
    CheckEq("offsetof(projectileCount)", offsetof(Snapshot, projectileCount), 379024);
    CheckEq("offsetof(gameCycle)", offsetof(Snapshot, gameCycle), 387220);
    CheckEq("offsetof(dynRegion)", offsetof(Snapshot, dynRegion), 387224);
    CheckEq("offsetof(dynChunks)", offsetof(Snapshot, dynChunks), 387264);
    CheckEq("kSnapshotOff1", nxt::ipc::kSnapshotOff1, 452864);
    CheckEq("kRingOff", nxt::ipc::kRingOff, 905664);
    CheckEq("kRegionSize", nxt::ipc::kRegionSize, 1036800);
    CheckEq("kOpenIfaceTypeMask", nxt::ipc::kOpenIfaceTypeMask, 0x07);
    CheckEq("kOpenIfaceFlagClientOpened", nxt::ipc::kOpenIfaceFlagClientOpened, 0x08);
    // v24 entity rows.
    CheckEq("offsetof(players)", offsetof(Snapshot, players), 49744);
    CheckEq("sizeof(PlayerEntry)", sizeof(nxt::ipc::PlayerEntry), 48);
    CheckEq("offsetof(PlayerEntry, name)", offsetof(nxt::ipc::PlayerEntry, name), 32);
    CheckEq("kFlagNameTruncated", nxt::ipc::kFlagNameTruncated, 0x02);
    CheckEq("sizeof(NpcEntry)", sizeof(nxt::ipc::NpcEntry), 48);
    CheckEq("offsetof(NpcEntry, activeTypeId)", offsetof(nxt::ipc::NpcEntry, activeTypeId), 40);
    CheckEq("offsetof(NpcEntry, combatLevel)", offsetof(nxt::ipc::NpcEntry, combatLevel), 44);
    CheckEq("sizeof(LocalPlayer)", sizeof(nxt::ipc::LocalPlayer), 560);
    CheckEq("offsetof(LocalPlayer, runEnergy)", offsetof(nxt::ipc::LocalPlayer, runEnergy), 552);
    CheckEq("offsetof(LocalPlayer, runWeight)", offsetof(nxt::ipc::LocalPlayer, runWeight), 556);
}

void CheckFlags(const char *name, uint8_t raw, int wantType, bool wantCs2, bool wantModal,
                const char *wantText)
{
    const nxtdbg::wire::OpenIfaceFlags f = nxtdbg::wire::DecodeOpenIfaceFlags(raw);
    char what[64];
    std::snprintf(what, sizeof(what), "%s 0x%02X type", name, raw);
    CheckEq(what, f.type, wantType);
    std::snprintf(what, sizeof(what), "%s 0x%02X isClientOpened", name, raw);
    CheckEq(what, f.isClientOpened, wantCs2);
    std::snprintf(what, sizeof(what), "%s 0x%02X IsModal", name, raw);
    CheckEq(what, f.IsModal(), wantModal);
    std::snprintf(what, sizeof(what), "%s 0x%02X reservedBits", name, raw);
    CheckEq(what, f.reservedBits, 0);
    char text[32];
    nxtdbg::wire::FormatOpenIfaceType(f, text, sizeof(text));
    std::snprintf(what, sizeof(what), "%s 0x%02X text", name, raw);
    CheckStr(what, text, wantText);
}

// The three types observed live on 950-1, the clientOpened bit, and the
// unknown sentinel. Every fixture byte has bits 4-7 clear, as the producer
// writes them.
void TestDecodeFlags()
{
    CheckFlags("bank modal", 0x00, 0, false, true, "modal");
    CheckFlags("hud overlay", 0x01, 1, false, false, "overlay");
    CheckFlags("cs2 child", 0x0B, 3, true, false, "child, cs2");
    CheckFlags("type 2", 0x02, 2, false, false, "type 2");
    CheckFlags("unknown", 0x07, 7, false, false, "unknown");
    CheckFlags("modal cs2", 0x08, 0, true, true, "modal, cs2");
}

// Reserved bits are split off, never folded into the type or the cs2 bit.
void TestReservedBitsIsolated()
{
    const nxtdbg::wire::OpenIfaceFlags f = nxtdbg::wire::DecodeOpenIfaceFlags(0xF1);
    CheckEq("0xF1 type ignores reserved", f.type, 1);
    CheckEq("0xF1 isClientOpened ignores reserved", f.isClientOpened, false);
    CheckEq("0xF1 reservedBits", f.reservedBits, 0x0F);
}

// A snapshot fixture read back through ReadOpenIfaces, the function the
// Interfaces panel calls. Bytes past the count are poisoned to prove they are
// not part of the list.
void TestReadFixture()
{
    auto snap = std::make_unique<nxt::ipc::Snapshot>();
    std::memset(snap.get(), 0xCD, sizeof(nxt::ipc::Snapshot));
    snap->openIfaceCount = 3;
    snap->openIfaceTotal = 3;
    const int32_t ids[] = {517, 1213, 1432};
    const uint8_t flags[] = {0x00, 0x01, 0x0B};
    std::memcpy(snap->openIfaces, ids, sizeof(ids));
    std::memcpy(snap->openIfaceFlags, flags, sizeof(flags));

    const nxtdbg::wire::OpenIfaceList list = nxtdbg::wire::ReadOpenIfaces(snap.get());
    CheckEq("fixture count", list.count, 3);
    CheckEq("fixture total", list.total, 3);
    const char *wantText[] = {"modal", "overlay", "child, cs2"};
    for (uint32_t i = 0; i < list.count; ++i)
    {
        char what[64];
        std::snprintf(what, sizeof(what), "fixture[%u] id", i);
        CheckEq(what, list.ids[i], ids[i]);
        char text[32];
        nxtdbg::wire::FormatOpenIfaceType(nxtdbg::wire::DecodeOpenIfaceFlags(list.flags[i]),
                                          text, sizeof(text));
        std::snprintf(what, sizeof(what), "fixture[%u] %d text", i, ids[i]);
        CheckStr(what, text, wantText[i]);
    }
    CheckEq("fixture flags alias the snapshot",
            list.flags == snap->openIfaceFlags, true);
}

void TestCountClamp()
{
    auto snap = std::make_unique<nxt::ipc::Snapshot>();
    snap->openIfaceCount = 0xFFFFFFFFu;
    const nxtdbg::wire::OpenIfaceList list = nxtdbg::wire::ReadOpenIfaces(snap.get());
    CheckEq("hostile count clamps to cap", list.count, nxt::ipc::kOpenIfaceCap);

    const nxtdbg::wire::OpenIfaceList none = nxtdbg::wire::ReadOpenIfaces(nullptr);
    CheckEq("null snapshot is empty", none.count, 0);
}

// '[' x depth, then ']' x depth -- or the object form, {"a":{"a":...0...}}.
std::string Nested(int depth, bool isObject)
{
    std::string s;
    for (int i = 0; i < depth; ++i)
    {
        s += isObject ? "{\"a\":" : "[";
    }
    s += "0";
    for (int i = 0; i < depth; ++i)
    {
        s += isObject ? "}" : "]";
    }
    return s;
}

long long Parses(const std::string &text)
{
    nxtdbg::cache::JsonValue v;
    return nxtdbg::cache::ParseJson(text, v) ? 1 : 0;
}

// Without the cap the 5000-deep lines (~10 KB) overflow the stack and the
// process dies here; the boundary pair fails rather than crashes.
void TestJsonDepthCap()
{
    const int cap = static_cast<int>(nxtdbg::cache::kMaxJsonNesting);
    CheckEq("json: cap is 64", cap, 64);
    CheckEq("json: array at cap parses", Parses(Nested(cap, false)), 1);
    CheckEq("json: object at cap parses", Parses(Nested(cap, true)), 1);
    CheckEq("json: array past cap refused", Parses(Nested(cap + 1, false)), 0);
    CheckEq("json: object past cap refused", Parses(Nested(cap + 1, true)), 0);
    CheckEq("json: 5000-deep array refused", Parses(Nested(5000, false)), 0);
    CheckEq("json: 5000-deep object refused", Parses(Nested(5000, true)), 0);

    // A refused parse leaves the documented Null, not a half-built tree.
    nxtdbg::cache::JsonValue v;
    nxtdbg::cache::ParseJson(Nested(5000, false), v);
    CheckEq("json: refused parse leaves Null", v.IsNull() ? 1 : 0, 1);
}

}

int main()
{
    TestLayoutPins();
    TestDecodeFlags();
    TestReservedBitsIsolated();
    TestReadFixture();
    TestCountClamp();
    TestJsonDepthCap();
    std::printf("%d failure(s)\n", failures);
    return failures;
}
