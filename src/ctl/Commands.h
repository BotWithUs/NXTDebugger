#pragma once
#include "ctl/JsonOut.h"
#include "rpc/RpcClient.h"

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

// Internal to the ctl module: the command handlers and the helpers they share.
// Everything here runs on the UI thread (see PipeServer.h for why).
namespace nxtdbg::app
{
struct App;
}

namespace nxtdbg::ctl
{

// A command fills `outResult` (an object merged into the response next to
// "ok": true) or sets `outError` and returns false.
using CommandFn = bool (*)(app::App &a, const JsonValue &args, JsonValue &outResult,
                           std::string &outError);

// --- argument helpers ------------------------------------------------------

bool        ArgInt(const JsonValue &args, const char *key, int64_t &outValue);
bool        ArgBool(const JsonValue &args, const char *key, bool fallback);
std::string ArgStr(const JsonValue &args, const char *key);

// Accepts `key: 5` or `key: [5, 6]`. Absent is fine (outIds left empty);
// a non-integer element is an error.
bool ArgIdList(const JsonValue &args, const char *key, std::vector<int32_t> &outIds,
               std::string &outError);

// --- agent access ----------------------------------------------------------

constexpr DWORD kCtlRpcTimeoutMs = 3000;

const char *RpcStatusName(rpc::CallStatus s);

// One call through the shared App::rpc, reply decoded to JSON. `params` may be
// nullptr for a no-param method. False with a readable error on any failure,
// including the agent answering {error: ...}.
bool CallRpc(app::App &a, const char *method, const JsonValue *params,
             JsonValue &outResult, std::string &outError,
             DWORD timeoutMs = kCtlRpcTimeoutMs);

// get_interface_tree(iface, comp) with every node enriched (index, hash,
// category_name, gameval, item_name). outNodes becomes a JSON array.
bool FetchTree(app::App &a, int32_t iface, int32_t rootComp, JsonValue &outNodes,
               std::string &outError);

// Batched int / string var reads. `method` is get_varps or get_varcs_int.
// outValues / outFound are index-aligned with ids.
bool ReadIntVars(app::App &a, const char *method, const std::vector<int32_t> &ids,
                 std::vector<int64_t> &outValues, std::vector<bool> &outFound,
                 std::string &outError);
bool ReadStrVars(app::App &a, const std::vector<int32_t> &ids,
                 std::vector<std::string> &outValues, std::vector<bool> &outFound,
                 std::string &outError);

// Varbit value = bits [lsb..msb] of its backing varp, via the cache definition.
// There is no agent read RPC for varbits (PROTOCOL.md 4.4). outSource names
// where each value came from: "varp" (decoded), "event" (last ring event, no
// cache) or "none".
bool ReadVarbits(app::App &a, const std::vector<int32_t> &ids,
                 std::vector<int64_t> &outValues, std::vector<std::string> &outSource,
                 std::string &outError);

// Opens the local cache at its default path if nothing has opened it yet.
// Retried at most every 30 s, so a missing NXTCache.dll costs one attempt.
bool EnsureCache(app::App &a);

std::string Utf8(const std::wstring &w);

// --- handlers ----------------------------------------------------------------

bool CmdTree   (app::App &, const JsonValue &, JsonValue &, std::string &);
bool CmdComp   (app::App &, const JsonValue &, JsonValue &, std::string &);
bool CmdVars   (app::App &, const JsonValue &, JsonValue &, std::string &);
bool CmdRpc    (app::App &, const JsonValue &, JsonValue &, std::string &);
bool CmdCache  (app::App &, const JsonValue &, JsonValue &, std::string &);
bool CmdGameval(app::App &, const JsonValue &, JsonValue &, std::string &);
bool CmdCs2    (app::App &, const JsonValue &, JsonValue &, std::string &);
bool CmdXref   (app::App &, const JsonValue &, JsonValue &, std::string &);
bool CmdWatch  (app::App &, const JsonValue &, JsonValue &, std::string &);
bool CmdUnwatch(app::App &, const JsonValue &, JsonValue &, std::string &);
bool CmdChanges(app::App &, const JsonValue &, JsonValue &, std::string &);
bool CmdEvents (app::App &, const JsonValue &, JsonValue &, std::string &);
bool CmdLayers (app::App &, const JsonValue &, JsonValue &, std::string &);

// --- per-frame work (Watch.cpp) ---------------------------------------------

void TrackAlways(app::App &a);
void PollWatches(app::App &a);

}
