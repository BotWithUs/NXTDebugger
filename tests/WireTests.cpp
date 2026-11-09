// Wire tests: the open-interface decode (wire v23 openIfaceFlags), the
// absolute layout figures it moved, and the JSON parser's nesting cap (the
// control pipe hands that parser untrusted lines).
//
// No framework on purpose -- this target links nothing but the wire decode
// module and the JSON parser, so it builds on any clone. Each CHECK prints what it compared; the
// exit code is the number of failures, which is what ctest reads.

#include "cache/Json.h"
#include "ctl/MsgPackJson.h"
#include "ctl/SelfJson.h"
#include "rpc/DrawParams.h"
#include "rpc/IntText.h"
#include "wire/LocalPlayer.h"
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
#include <vector>

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

// Runtime echo of the v25 figures, so the test log names the numbers the
// brief and PROTOCOL.md quote. The static_asserts in ProtocolDocPins.h are the
// gate; these make the result readable.
void TestLayoutPins()
{
    using nxt::ipc::Snapshot;
    CheckEq("kProtocolVersion", nxt::ipc::kProtocolVersion, 25);
    CheckEq("sizeof(Snapshot)", sizeof(Snapshot), 469192);
    CheckEq("offsetof(openIfaces)", offsetof(Snapshot, openIfaces), 377744);
    CheckEq("offsetof(openIfaceFlags)", offsetof(Snapshot, openIfaceFlags), 378768);
    CheckEq("sizeof(openIfaceFlags)", sizeof(Snapshot::openIfaceFlags), nxt::ipc::kOpenIfaceCap);
    CheckEq("offsetof(_padAfterOpenIfaces)", offsetof(Snapshot, _padAfterOpenIfaces), 379024);
    CheckEq("offsetof(groundItemCount)", offsetof(Snapshot, groundItemCount), 379028);
    CheckEq("offsetof(projectileCount)", offsetof(Snapshot, projectileCount), 395416);
    CheckEq("offsetof(gameCycle)", offsetof(Snapshot, gameCycle), 403612);
    CheckEq("offsetof(dynRegion)", offsetof(Snapshot, dynRegion), 403616);
    CheckEq("offsetof(dynChunks)", offsetof(Snapshot, dynChunks), 403656);
    CheckEq("kSnapshotOff1", nxt::ipc::kSnapshotOff1, 469312);
    CheckEq("kRingOff", nxt::ipc::kRingOff, 938560);
    CheckEq("kRegionSize", nxt::ipc::kRegionSize, 1069696);
    CheckEq("kOpenIfaceTypeMask", nxt::ipc::kOpenIfaceTypeMask, 0x07);
    CheckEq("kOpenIfaceFlagClientOpened", nxt::ipc::kOpenIfaceFlagClientOpened, 0x08);
    // v24 entity rows.
    CheckEq("offsetof(players)", offsetof(Snapshot, players), 49752);
    CheckEq("sizeof(PlayerEntry)", sizeof(nxt::ipc::PlayerEntry), 56);
    CheckEq("offsetof(PlayerEntry, name)", offsetof(nxt::ipc::PlayerEntry, name), 32);
    CheckEq("kFlagNameTruncated", nxt::ipc::kFlagNameTruncated, 0x02);
    CheckEq("sizeof(NpcEntry)", sizeof(nxt::ipc::NpcEntry), 48);
    CheckEq("offsetof(NpcEntry, activeTypeId)", offsetof(nxt::ipc::NpcEntry, activeTypeId), 40);
    CheckEq("offsetof(NpcEntry, combatLevel)", offsetof(nxt::ipc::NpcEntry, combatLevel), 44);
    CheckEq("sizeof(LocalPlayer)", sizeof(nxt::ipc::LocalPlayer), 568);
    CheckEq("offsetof(LocalPlayer, runEnergy)", offsetof(nxt::ipc::LocalPlayer, runEnergy), 552);
    CheckEq("offsetof(LocalPlayer, runWeight)", offsetof(nxt::ipc::LocalPlayer, runWeight), 556);
    // v25 server tile.
    CheckEq("offsetof(PlayerEntry, serverTileX)", offsetof(nxt::ipc::PlayerEntry, serverTileX), 48);
    CheckEq("offsetof(PlayerEntry, serverTileY)", offsetof(nxt::ipc::PlayerEntry, serverTileY), 50);
    CheckEq("offsetof(PlayerEntry, serverPlane)", offsetof(nxt::ipc::PlayerEntry, serverPlane), 52);
    CheckEq("offsetof(LocalPlayer, serverTileX)", offsetof(nxt::ipc::LocalPlayer, serverTileX), 560);
    CheckEq("offsetof(LocalPlayer, serverTileY)", offsetof(nxt::ipc::LocalPlayer, serverTileY), 562);
    CheckEq("offsetof(LocalPlayer, serverPlane)", offsetof(nxt::ipc::LocalPlayer, serverPlane), 564);
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

// One frame shape for the Player panel. selfFill is what self.serverIndex and the server
// tile hold: -1 on a current agent, 0 on one before the pre-publish fix.
void CheckPanel(const char *what, int32_t gameState, int32_t ownIndex, int32_t selfFill,
                nxtdbg::wire::PlayerPanelState want)
{
    auto snap = std::make_unique<nxt::ipc::Snapshot>();
    snap->gameState        = gameState;
    snap->ownIndex         = ownIndex;
    snap->self.serverIndex = selfFill;
    snap->self.serverTileX = static_cast<int16_t>(selfFill);
    CheckEq(what, static_cast<long long>(nxtdbg::wire::ClassifyPlayerPanel(snap.get())),
            static_cast<long long>(want));
}

// The regression: self.serverIndex is -1 at the lobby since the agent's pre-publish fix,
// and the panel read that as "not attached". Attachment and the world are separate.
void TestPlayerPanelState()
{
    using nxtdbg::wire::PlayerPanelState;
    constexpr int32_t kLobby = 20;
    constexpr int32_t kWorld = 30;
    CheckEq("no session: not attached",
            static_cast<long long>(nxtdbg::wire::ClassifyPlayerPanel(nullptr)),
            static_cast<long long>(PlayerPanelState::NotAttached));
    CheckPanel("lobby, current agent", kLobby, -1, -1, PlayerPanelState::NoLocalPlayer);
    CheckPanel("lobby, older agent", kLobby, -1, 0, PlayerPanelState::NoLocalPlayer);
    CheckPanel("no-client frame", 0, -1, -1, PlayerPanelState::NoLocalPlayer);
    CheckPanel("all-zero frame, older agent", 0, 0, 0, PlayerPanelState::NoLocalPlayer);
    CheckPanel("state 30, player not resolved", kWorld, -1, -1, PlayerPanelState::NoLocalPlayer);
    CheckPanel("state 30, not resolved, older agent", kWorld, -1, 0, PlayerPanelState::NoLocalPlayer);
    CheckPanel("in world", kWorld, 123, 123, PlayerPanelState::InWorld);
    CheckPanel("in world at index 0", kWorld, 0, 0, PlayerPanelState::InWorld);
}

// One frame shape for the control pipe's self gate. A self is reported only with a
// local player, so neither command can put a self at tile 0,0 on the wire.
void CheckCtlSelf(const char *what, int32_t gameState, int32_t ownIndex, int32_t selfFill,
                  bool wantSelf)
{
    auto snap = std::make_unique<nxt::ipc::Snapshot>();
    snap->gameState        = gameState;
    snap->ownIndex         = ownIndex;
    snap->self.serverIndex = selfFill;
    snap->self.tileX       = wantSelf ? 3222 : 0;
    const nxtdbg::ctl::JsonValue player = nxtdbg::ctl::StatusPlayerJson(*snap);
    const nxtdbg::ctl::JsonValue self   = nxtdbg::ctl::SceneSelfJson(*snap);
    std::printf("[ctl] %s\n", what);
    CheckEq("  status player present", player.IsObject() ? 1 : 0, wantSelf ? 1 : 0);
    CheckEq("  status player null otherwise", player.IsNull() ? 1 : 0, wantSelf ? 0 : 1);
    CheckEq("  scene self present", self.IsObject() ? 1 : 0, wantSelf ? 1 : 0);
    CheckEq("  scene self null otherwise", self.IsNull() ? 1 : 0, wantSelf ? 0 : 1);
}

void TestCtlSelfGate()
{
    constexpr int32_t kLobby = 20;
    constexpr int32_t kWorld = 30;
    CheckCtlSelf("state 30, player not resolved", kWorld, -1, -1, false);
    CheckCtlSelf("state 30, not resolved, older agent", kWorld, -1, 0, false);
    CheckCtlSelf("lobby, current agent", kLobby, -1, -1, false);
    CheckCtlSelf("lobby, older agent", kLobby, -1, 0, false);
    CheckCtlSelf("all-zero frame, older agent", 0, 0, 0, false);
    CheckCtlSelf("in world", kWorld, 123, 123, true);
    CheckCtlSelf("in world at index 0", kWorld, 0, 0, true);
}

void CheckIntText(const uint8_t *bytes, size_t size, const char *want, size_t wantPos,
                  const char *what)
{
    nxt::rpc::msgpack::Reader reader(bytes, size);
    const std::string text = nxtdbg::rpc::ReadIntText(reader);
    const bool isOk = text == want && reader.Pos() == wantPos;
    std::printf("%s  %-48s got %s at %zu, want %s at %zu\n", isOk ? "PASS" : "FAIL", what,
                text.c_str(), reader.Pos(), want, wantPos);
    if (!isOk)
    {
        ++failures;
    }
}

const uint8_t kUInt64Max[] = {0xCF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const uint8_t kFirstHigh[] = {0xCF, 0x80, 0, 0, 0, 0, 0, 0, 0};
const uint8_t kInt64Top[]  = {0xCF, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const uint8_t kInt64Min[]  = {0xD3, 0x80, 0, 0, 0, 0, 0, 0, 0};

// The generic renderers' integer case, across the range the codec's ReadInt refuses.
void TestIntText()
{
    const uint8_t minusOne[] = {0xFF};
    const uint8_t cut[]      = {0xCF, 0xFF, 0xFF};
    CheckIntText(kUInt64Max, sizeof(kUInt64Max), "18446744073709551615", 9, "uint64 max printed");
    CheckIntText(kFirstHigh, sizeof(kFirstHigh), "9223372036854775808", 9, "2^63 printed");
    CheckIntText(kInt64Top, sizeof(kInt64Top), "9223372036854775807", 9, "INT64_MAX in uint64 form");
    CheckIntText(kInt64Min, sizeof(kInt64Min), "-9223372036854775808", 9, "int64 min");
    CheckIntText(minusOne, sizeof(minusOne), "-1", 1, "-1 stays -1");
    CheckIntText(cut, sizeof(cut), "?", 0, "truncated uint64 is not a number");
}

// A production renderer: what follows a high value must still decode.
void TestValueToText()
{
    const uint8_t array[] = {0x92, 0xCF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07};
    nxt::rpc::msgpack::Reader reader(array, sizeof(array));
    const std::string text = nxtdbg::rpc::ValueToText(reader);
    const bool isOk = text.find("18446744073709551615") != std::string::npos &&
                      text.find('7', text.find("18446744073709551615") + 20) != std::string::npos &&
                      reader.AtEnd();
    std::printf("%s  %-48s got %s\n", isOk ? "PASS" : "FAIL", "ValueToText walks past a high value",
                text.c_str());
    if (!isOk)
    {
        ++failures;
    }
}

// nxtdbgctl's generic reply bridge: one high value must not fail the whole reply.
void TestMsgPackJsonRange()
{
    using nxtdbg::cache::JsonType;
    using nxtdbg::ctl::MsgPackToJson;
    nxtdbg::cache::JsonValue value;
    const bool isMaxOk = MsgPackToJson(kUInt64Max, sizeof(kUInt64Max), value);
    Check(isMaxOk && value.type == JsonType::UInt, "bridge takes uint64 max as UInt", isMaxOk, 1);
    const bool isTextOk = value.AsString() == "18446744073709551615";
    Check(isTextOk, "bridge keeps uint64 max's value", isTextOk, 1);
    Check(value.AsInt(-5) == -5, "AsInt on a high value returns the default", value.AsInt(-5), -5);
    const bool isDoubleDefault = value.AsDouble(-5.0) == -5.0;
    Check(isDoubleDefault, "AsDouble on a high value returns the default", isDoubleDefault, 1);
    const bool isWritten = nxtdbg::ctl::ToJson(value) == "18446744073709551615";
    Check(isWritten, "WriteJson prints a high value exactly", isWritten, 1);
    std::vector<uint8_t> back;
    const bool isEncoded = nxtdbg::ctl::JsonToMsgPack(value, back);
    const bool isSame = isEncoded && back.size() == sizeof(kUInt64Max) &&
                        std::memcmp(back.data(), kUInt64Max, back.size()) == 0;
    Check(isSame, "uint64 max re-encodes byte-identical", isSame, 1);
    const bool isTopOk = MsgPackToJson(kInt64Top, sizeof(kInt64Top), value) &&
                         value.type == JsonType::Int && value.intVal == INT64_MAX;
    Check(isTopOk, "INT64_MAX in uint64 form stays Int", isTopOk, 1);
    const bool isMinOk = MsgPackToJson(kInt64Min, sizeof(kInt64Min), value) &&
                         value.type == JsonType::Int && value.intVal == INT64_MIN;
    Check(isMinOk, "int64 min is Int", isMinOk, 1);
    const auto top = nxtdbg::ctl::JUInt(static_cast<uint64_t>(INT64_MAX));
    const bool isCanonical = top.type == JsonType::Int && top.intVal == INT64_MAX;
    Check(isCanonical, "JUInt(INT64_MAX) is Int", isCanonical, 1);
}

int main()
{
    TestIntText();
    TestValueToText();
    TestMsgPackJsonRange();
    TestLayoutPins();
    TestCtlSelfGate();
    TestPlayerPanelState();
    TestDecodeFlags();
    TestReservedBitsIsolated();
    TestReadFixture();
    TestCountClamp();
    TestJsonDepthCap();
    std::printf("%d failure(s)\n", failures);
    return failures;
}
