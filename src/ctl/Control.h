#pragma once
#include "ctl/JsonOut.h"
#include "ctl/PipeServer.h"

#include <Windows.h>

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Programmatic control surface: a line-delimited JSON server on a named pipe
// that exposes what the debugger sees (session status, interface trees, vars,
// the event tail, read-only RPC) to an automation client -- nxtdbgctl.exe.
//
// Strictly an observer, like the rest of the debugger: no command queues a game
// action, and the `rpc` passthrough only forwards an explicit allowlist of
// query methods. Every command runs on the UI thread (see PipeServer.h).
namespace nxtdbg::app
{
struct App;
}

namespace nxtdbg::wire
{
struct EventRecord;
}

namespace nxtdbg::ctl
{

enum class AttachMode : uint8_t
{
    None,
    Auto,   // the single rs2client that publishes an agent snapshot
    Pid,
};

struct LaunchOptions
{
    AttachMode attachMode       = AttachMode::None;
    DWORD      attachPid        = 0;
    bool         isCtlEnabled     = false;            // --ctl: the pipe is opt-in
    int          showCommand      = SW_SHOWDEFAULT;   // SW_SHOWMINNOACTIVE / SW_HIDE
    bool         shouldSaveLayout = true;             // false under --hidden
    std::wstring cs2Dir;                              // --cs2-dir; empty keeps the default
};

// Parses the process command line. False (with a message) on an unknown flag
// or a malformed value; the caller shows it and exits.
bool ParseLaunchOptions(const wchar_t *commandLine, LaunchOptions &outOptions,
                        std::wstring &outError);

// ---------------------------------------------------------------------------
// Watches and the change log
// ---------------------------------------------------------------------------

struct IntWatch
{
    int32_t id      = 0;
    bool    isKnown = false;
    int64_t value   = 0;
};

struct StrWatch
{
    int32_t     id      = 0;
    bool        isKnown = false;
    std::string value;
};

// Per-interface baseline: the diffed fields of every component, keyed on
// (comp, sub). Only the fields the watch diffs are stored, not whole nodes.
struct IfaceWatch
{
    int32_t                                          iface   = -1;
    bool                                             isKnown = false;
    std::map<std::pair<int32_t, int32_t>, JsonValue> comps;
};

struct WatchSet
{
    std::vector<IntWatch>   varps;
    std::vector<IntWatch>   varbits;
    std::vector<IntWatch>   varcs;
    std::vector<StrWatch>   varcstrs;
    std::vector<IfaceWatch> ifaces;
    uint32_t                varIntervalMs    = 100;
    uint32_t                ifaceIntervalMs  = 300;
    bool                    isGeometryDiffed = false;
    ULONGLONG               lastVarPoll      = 0;
    ULONGLONG               lastIfacePoll    = 0;
};

// Bounded log with a monotonic cursor. `seq` starts at 1 and never repeats for
// the life of the debugger process, across detach / re-attach, so a client's
// cursor stays meaningful. A reader whose cursor fell behind the oldest
// retained entry is told so (`truncated`) instead of silently skipping.
struct LogRing
{
    struct Entry
    {
        uint64_t  seq;
        JsonValue body;
    };
    std::deque<Entry> entries;
    uint64_t          nextSeq  = 1;
    size_t            capacity = 8192;

    void     Add(JsonValue body);
    uint64_t Head() const { return nextSeq - 1; }
};

// Always-on trackers: open-interface set, game state and attach changes are
// logged whether or not anything is watched.
struct Tracker
{
    DWORD                lastPid       = 0;
    bool                 hasOpen       = false;
    std::vector<int32_t> lastOpen;
    int32_t              lastGameState = INT32_MIN;
};

struct VarbitDef
{
    int32_t varp  = -1;
    int32_t lsb   = 0;
    int32_t msb   = 0;
    bool    isValid = false;
};

struct ControlState
{
    PipeServer    pipe;
    LaunchOptions options;
    WatchSet      watches;
    LogRing       changes;
    LogRing       events;
    Tracker       tracker;
    ULONGLONG     startTick       = 0;
    bool          isAttachPending = false;   // --attach / `attach` not yet bound
    ULONGLONG     lastAttachTry   = 0;
    ULONGLONG     quitAt          = 0;       // `quit`: close the window at this tick
    ULONGLONG     lastCacheTry    = 0;       // EnsureCache throttle
    std::string   attachStatus;          // why --attach has not bound yet (UTF-8)
    std::unordered_map<int32_t, VarbitDef> varbitDefs;   // cache-decoded, memoised
    std::unordered_map<int32_t, int64_t>   varbitSeen;   // last value off the event ring
};

// Lifecycle. StartControl records the options and opens the pipe when enabled.
void StartControl(app::App &a, const LaunchOptions &options);
void StopControl(app::App &a);

// UI thread, once per frame (App::Update): auto-attach retry, always-on
// trackers, watch polling, then answer every queued pipe request.
void PumpControl(app::App &a);

// UI thread, from App's event drain: every freshly drained ring event.
void RecordEvents(app::App &a, const std::vector<wire::EventRecord> &fresh);

}
