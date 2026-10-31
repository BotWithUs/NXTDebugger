// Read-side control commands: interface trees, components, vars, the RPC
// passthrough, and the offline lookups (cache, gameval, CS2).
#include "ctl/Commands.h"
#include "ctl/Control.h"
#include "ctl/MsgPackJson.h"

#include "app/App.h"
#include "cache/Json.h"
#include "rpc/RpcClient.h"
#include "wire/CategoryName.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace nxtdbg::ctl
{

namespace
{

constexpr size_t    kIntVarBatch      = 256;   // agent kVarBatchCap
constexpr size_t    kStrVarBatch      = 64;    // agent kVarcStringBatchCap
constexpr ULONGLONG kCacheRetryMs     = 30000;
constexpr size_t    kGamevalMaxHits   = 200;
constexpr size_t    kXrefMaxScripts   = 2000;

constexpr const char kGamevalKinds[] =
    "item obj npc loc if interface component varp varbit varc struct enum param inv seq "
    "sprite graphic model dbrow quest";

// Query methods the `rpc` passthrough forwards. Everything else is refused,
// including get_script_handle (allocates agent state) and debug_draw_enable
// (writes when given `enabled`). Taken from PROTOCOL.md 4.4; a method missing
// here is refused even if the agent has it -- extend deliberately.
constexpr const char *kReadOnlyMethods[] = {
    "rpc.ping", "rpc.list_methods", "rpc.client_count",
    "get_game_cycle", "get_login_state",
    "get_action_queue_size", "get_action_history", "get_last_action_time",
    "are_actions_blocked",
    "get_auto_login", "get_token_refresher", "get_account_info", "get_current_world",
    "click_stats", "move_stats",
    "get_component", "get_components", "get_static_children", "get_dynamic_children",
    "get_interface_tree", "find_component_at",
    "get_varp", "get_varps", "get_varc_int", "get_varcs_int", "get_varc_string",
    "get_varcs_string", "get_obj_vars",
    "query_spot_anims", "query_world_map_elements", "_debug.entity_facing",
    "query_entities", "get_local_player", "query_inventories",
    "debug_draw_list", "debug_draw_stats", "debug_draw_probe_pixels",
};

bool IsReadOnlyMethod(const std::string &m)
{
    for (const char *allowed : kReadOnlyMethods)
    {
        if (m == allowed)
        {
            return true;
        }
    }
    return false;
}

int64_t IntField(const JsonValue &obj, const char *key, int64_t fallback)
{
    const JsonValue *v = obj.Find(key);
    return (v && v->type == JsonType::Int) ? v->intVal : fallback;
}

std::string ItemName(app::App &a, int32_t id, std::unordered_map<int32_t, std::string> &memo)
{
    auto it = memo.find(id);
    if (it != memo.end())
    {
        return it->second;
    }
    std::string     name;
    cache::JsonValue def;
    if (cache::ParseJson(a.cache.GetJson("item", id), def))
    {
        if (const cache::JsonValue *n = def.Find("name"))
        {
            name = n->AsString();
        }
    }
    memo.emplace(id, name);
    return name;
}

// index / hash first, then every field the agent sent, then the debugger's
// own resolutions. `index` < 0 omits it (single-component replies).
JsonValue EnrichNode(app::App &a, const JsonValue &node, int32_t index,
                     std::unordered_map<int32_t, std::string> &itemNames)
{
    JsonValue out = JObj();
    const auto iface = static_cast<int32_t>(IntField(node, "iface", -1));
    const auto comp  = static_cast<int32_t>(IntField(node, "comp", -1));
    if (index >= 0)
    {
        Put(out, "index", JInt(index));
    }
    if (iface >= 0 && comp >= 0)
    {
        Put(out, "hash", JInt((int64_t(iface) << 16) | (comp & 0xFFFF)));
    }
    for (const auto &kv : node.objVal)
    {
        out.objVal.push_back(kv);
    }
    const auto category = static_cast<int32_t>(IntField(node, "category", 0));
    Put(out, "category_name", JStr(wire::CategoryName(category)));
    Put(out, "gameval", JStr(a.gameval.ComponentName(iface, comp)));
    const auto item = static_cast<int32_t>(IntField(node, "item_id", -1));
    if (item >= 0 && a.cache.IsOpen())
    {
        std::string n = ItemName(a, item, itemNames);
        Put(out, "item_name", n.empty() ? JNull() : JStr(std::move(n)));
    }
    return out;
}

JsonValue IdsParam(const std::vector<int32_t> &ids, size_t begin, size_t end)
{
    JsonValue arr = JArr();
    for (size_t i = begin; i < end; ++i)
    {
        Push(arr, JInt(ids[i]));
    }
    JsonValue p = JObj();
    Put(p, "ids", std::move(arr));
    return p;
}

bool LoadVarbitDef(app::App &a, int32_t id, VarbitDef &outDef)
{
    auto it = a.ctl.varbitDefs.find(id);
    if (it != a.ctl.varbitDefs.end())
    {
        outDef = it->second;
        return outDef.isValid;
    }
    if (!EnsureCache(a))
    {
        return false;   // not memoised: a later cache open should still work
    }
    VarbitDef        def;
    cache::JsonValue j;
    if (cache::ParseJson(a.cache.GetJson("varbit", id), j))
    {
        const cache::JsonValue *varId  = j.Find("varId");
        const cache::JsonValue *domain = j.Find("domainType");
        // Domain 0 is the player domain (varps); anything else has no read path.
        if (varId && (!domain || domain->AsInt() == 0))
        {
            def.varp    = static_cast<int32_t>(varId->AsInt());
            def.lsb     = static_cast<int32_t>(j.Find("lsb") ? j.Find("lsb")->AsInt() : 0);
            def.msb     = static_cast<int32_t>(j.Find("msb") ? j.Find("msb")->AsInt() : 31);
            def.isValid = def.lsb >= 0 && def.msb >= def.lsb && def.msb < 32;
        }
    }
    a.ctl.varbitDefs.emplace(id, def);
    outDef = def;
    return def.isValid;
}

int64_t ExtractBits(int64_t varp, const VarbitDef &d)
{
    const int      width = d.msb - d.lsb + 1;
    const uint32_t mask  = (width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
    return static_cast<int64_t>((static_cast<uint32_t>(varp) >> d.lsb) & mask);
}

std::wstring Wide(const std::string &s)
{
    if (s.empty())
    {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(std::max(n, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

// Optional {dir}: repoint the shared CS2 corpus (the Cache Browser's "Build
// index" box does the same). Refused mid-build, because the index worker reads
// the directory while it runs; a finished index is rebuilt against the new dir
// so xref cannot answer from the old corpus.
bool ApplyCs2Dir(app::App &a, const JsonValue &args, std::string &outError)
{
    const std::wstring dir = Wide(ArgStr(args, "dir"));
    if (dir.empty() || dir == a.cs2.Dir())
    {
        return true;
    }
    using Status = cs2::Cs2Index::Status;
    const Status st = a.cs2.GetStatus();
    if (st == Status::Building)
    {
        outError = "the CS2 index is building from " + Utf8(a.cs2.Dir())
                 + "; retry the dir change when it finishes";
        return false;
    }
    a.cs2.SetDir(dir);
    if (st == Status::Ready)
    {
        a.cs2.BuildAsync(true);
    }
    return true;
}

bool ReadScriptFile(const std::wstring &path, std::string &out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

}   // namespace

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

const char *RpcStatusName(rpc::CallStatus s)
{
    switch (s)
    {
        case rpc::CallStatus::Ok:            return "ok";
        case rpc::CallStatus::Busy:          return "busy";
        case rpc::CallStatus::Disconnected:  return "disconnected";
        case rpc::CallStatus::Timeout:       return "timeout";
        case rpc::CallStatus::ProtocolError: return "protocol_error";
        case rpc::CallStatus::HandlerError:  return "handler_error";
    }
    return "?";
}

std::string Utf8(const std::wstring &w)
{
    if (w.empty())
    {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr,
                                nullptr);
    std::string out(size_t(std::max(n, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool ArgInt(const JsonValue &args, const char *key, int64_t &outValue)
{
    const JsonValue *v = args.Find(key);
    if (!v || !v->IsNumber())
    {
        return false;
    }
    outValue = v->AsInt();
    return true;
}

bool ArgBool(const JsonValue &args, const char *key, bool fallback)
{
    const JsonValue *v = args.Find(key);
    return (v && v->IsBool()) ? v->boolVal : fallback;
}

std::string ArgStr(const JsonValue &args, const char *key)
{
    const JsonValue *v = args.Find(key);
    return (v && v->IsString()) ? v->strVal : std::string();
}

bool ArgIdList(const JsonValue &args, const char *key, std::vector<int32_t> &outIds,
               std::string &outError)
{
    outIds.clear();
    const JsonValue *v = args.Find(key);
    if (!v || v->IsNull())
    {
        return true;
    }
    if (v->IsNumber())
    {
        outIds.push_back(static_cast<int32_t>(v->AsInt()));
        return true;
    }
    if (!v->IsArray())
    {
        outError = std::string("'") + key + "' must be an int or an array of ints";
        return false;
    }
    for (const JsonValue &e : v->arrVal)
    {
        if (!e.IsNumber())
        {
            outError = std::string("'") + key + "' must contain only ints";
            return false;
        }
        outIds.push_back(static_cast<int32_t>(e.AsInt()));
    }
    return true;
}

bool EnsureCache(app::App &a)
{
    if (a.cache.IsOpen())
    {
        return true;
    }
    const ULONGLONG now = GetTickCount64();
    if (a.ctl.lastCacheTry != 0 && now - a.ctl.lastCacheTry < kCacheRetryMs)
    {
        return false;
    }
    a.ctl.lastCacheTry = now;
    return a.cache.Open(cache::DefaultCacheDir());
}

bool CallRpc(app::App &a, const char *method, const JsonValue *params,
             JsonValue &outResult, std::string &outError, DWORD timeoutMs)
{
    if (!a.session.IsOpen())
    {
        outError = "not attached";
        return false;
    }
    if (!a.rpc.IsConnected())
    {
        outError = "rpc pipe not connected: " + Utf8(a.rpc.LastError());
        return false;
    }
    std::vector<uint8_t> mp;
    if (params && !JsonToMsgPack(*params, mp))
    {
        outError = "params too large to encode";
        return false;
    }
    std::vector<uint8_t> reply;
    const rpc::CallStatus st = a.rpc.Call(method, mp.empty() ? nullptr : mp.data(),
                                          static_cast<uint32_t>(mp.size()), reply, timeoutMs);
    if (st == rpc::CallStatus::HandlerError)
    {
        JsonValue e;
        MsgPackToJson(reply, e);
        outError = std::string(method) + ": agent error: " + (e.IsString() ? e.strVal : ToJson(e));
        return false;
    }
    if (st != rpc::CallStatus::Ok)
    {
        outError = std::string(method) + ": " + RpcStatusName(st) + " " + Utf8(a.rpc.LastError());
        return false;
    }
    if (reply.empty())
    {
        outResult = JNull();
        return true;
    }
    if (!MsgPackToJson(reply, outResult))
    {
        outError = std::string(method) + ": reply did not decode";
        return false;
    }
    return true;
}

bool FetchTree(app::App &a, int32_t iface, int32_t rootComp, JsonValue &outNodes,
               std::string &outError)
{
    JsonValue params = JObj();
    Put(params, "iface", JInt(iface));
    Put(params, "comp", JInt(rootComp));
    JsonValue reply;
    if (!CallRpc(a, "get_interface_tree", &params, reply, outError))
    {
        return false;
    }
    const JsonValue *nodes = reply.Find("nodes");
    if (!nodes || !nodes->IsArray())
    {
        outError = "get_interface_tree reply has no nodes array";
        return false;
    }
    std::unordered_map<int32_t, std::string> itemNames;
    outNodes = JArr();
    outNodes.arrVal.reserve(nodes->arrVal.size());
    for (size_t i = 0; i < nodes->arrVal.size(); ++i)
    {
        Push(outNodes, EnrichNode(a, nodes->arrVal[i], static_cast<int32_t>(i), itemNames));
    }
    return true;
}

bool ReadIntVars(app::App &a, const char *method, const std::vector<int32_t> &ids,
                 std::vector<int64_t> &outValues, std::vector<bool> &outFound,
                 std::string &outError)
{
    outValues.assign(ids.size(), 0);
    outFound.assign(ids.size(), false);
    for (size_t begin = 0; begin < ids.size(); begin += kIntVarBatch)
    {
        const size_t end    = std::min(ids.size(), begin + kIntVarBatch);
        JsonValue    params = IdsParam(ids, begin, end);
        JsonValue    reply;
        if (!CallRpc(a, method, &params, reply, outError))
        {
            return false;
        }
        const JsonValue *vals  = reply.Find("values");
        const JsonValue *found = reply.Find("found");
        for (size_t i = begin; vals && i < end && i - begin < vals->arrVal.size(); ++i)
        {
            outValues[i] = vals->arrVal[i - begin].AsInt();
            outFound[i]  = !found || (i - begin < found->arrVal.size()
                                      && found->arrVal[i - begin].AsBool());
        }
    }
    return true;
}

bool ReadStrVars(app::App &a, const std::vector<int32_t> &ids,
                 std::vector<std::string> &outValues, std::vector<bool> &outFound,
                 std::string &outError)
{
    outValues.assign(ids.size(), std::string());
    outFound.assign(ids.size(), false);
    for (size_t begin = 0; begin < ids.size(); begin += kStrVarBatch)
    {
        const size_t end    = std::min(ids.size(), begin + kStrVarBatch);
        JsonValue    params = IdsParam(ids, begin, end);
        JsonValue    reply;
        if (!CallRpc(a, "get_varcs_string", &params, reply, outError))
        {
            return false;
        }
        const JsonValue *vals  = reply.Find("values");
        const JsonValue *found = reply.Find("found");
        for (size_t i = begin; vals && i < end && i - begin < vals->arrVal.size(); ++i)
        {
            outValues[i] = vals->arrVal[i - begin].AsString();
            outFound[i]  = !found || (i - begin < found->arrVal.size()
                                      && found->arrVal[i - begin].AsBool());
        }
    }
    return true;
}

bool ReadVarbits(app::App &a, const std::vector<int32_t> &ids,
                 std::vector<int64_t> &outValues, std::vector<std::string> &outSource,
                 std::string &outError)
{
    outValues.assign(ids.size(), 0);
    outSource.assign(ids.size(), "none");
    std::vector<VarbitDef> defs(ids.size());
    std::vector<int32_t>   varps;
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (LoadVarbitDef(a, ids[i], defs[i]))
        {
            varps.push_back(defs[i].varp);
        }
    }
    std::vector<int64_t> varpValues;
    std::vector<bool>    varpFound;
    if (!varps.empty() && !ReadIntVars(a, "get_varps", varps, varpValues, varpFound, outError))
    {
        return false;
    }
    size_t k = 0;
    for (size_t i = 0; i < ids.size(); ++i)
    {
        if (defs[i].isValid)
        {
            if (varpFound[k])
            {
                outValues[i] = ExtractBits(varpValues[k], defs[i]);
                outSource[i] = "varp";
            }
            ++k;
            continue;
        }
        auto seen = a.ctl.varbitSeen.find(ids[i]);
        if (seen != a.ctl.varbitSeen.end())
        {
            outValues[i] = seen->second;
            outSource[i] = "event";
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

// tree {iface, comp?} -> {iface, iface_name, count, nodes: [...]}
bool CmdTree(app::App &a, const JsonValue &args, JsonValue &outResult, std::string &outError)
{
    int64_t iface = -1;
    int64_t comp  = 0;
    if (!ArgInt(args, "iface", iface) || iface < 0)
    {
        outError = "tree needs {iface}";
        return false;
    }
    ArgInt(args, "comp", comp);
    JsonValue nodes;
    if (!FetchTree(a, static_cast<int32_t>(iface), static_cast<int32_t>(comp), nodes, outError))
    {
        return false;
    }
    Put(outResult, "iface", JInt(iface));
    Put(outResult, "iface_name", JStr(a.gameval.NameForCacheType("if", int32_t(iface))));
    Put(outResult, "count", JInt(static_cast<int64_t>(nodes.arrVal.size())));
    Put(outResult, "nodes", std::move(nodes));
    return true;
}

// comp {iface, comp, sub?}. Without sub: get_component. With sub: the matching
// node of the interface tree (get_component cannot address a dynamic child).
bool CmdComp(app::App &a, const JsonValue &args, JsonValue &outResult, std::string &outError)
{
    int64_t iface = -1;
    int64_t comp  = -1;
    int64_t sub   = -1;
    if (!ArgInt(args, "iface", iface) || !ArgInt(args, "comp", comp))
    {
        outError = "comp needs {iface, comp}";
        return false;
    }
    if (ArgInt(args, "sub", sub) && sub >= 0)
    {
        JsonValue nodes;
        if (!FetchTree(a, static_cast<int32_t>(iface), 0, nodes, outError))
        {
            return false;
        }
        for (JsonValue &n : nodes.arrVal)
        {
            if (IntField(n, "comp", -2) == comp && IntField(n, "sub", -2) == sub)
            {
                Put(outResult, "component", std::move(n));
                return true;
            }
        }
        outError = "no such (comp, sub) in the interface tree";
        return false;
    }
    JsonValue params = JObj();
    Put(params, "iface", JInt(iface));
    Put(params, "comp", JInt(comp));
    JsonValue reply;
    if (!CallRpc(a, "get_component", &params, reply, outError))
    {
        return false;
    }
    if (IntField(reply, "iface", -1) < 0)
    {
        outError = "component not found (interface not open?)";
        return false;
    }
    std::unordered_map<int32_t, std::string> itemNames;
    Put(outResult, "component", EnrichNode(a, reply, -1, itemNames));
    return true;
}

// vars {varp:[..], varbit:[..], varc:[..], varcstr:[..]}
bool CmdVars(app::App &a, const JsonValue &args, JsonValue &outResult, std::string &outError)
{
    std::vector<int32_t> varps;
    std::vector<int32_t> varbits;
    std::vector<int32_t> varcs;
    std::vector<int32_t> varcstrs;
    if (!ArgIdList(args, "varp", varps, outError) || !ArgIdList(args, "varbit", varbits, outError)
        || !ArgIdList(args, "varc", varcs, outError)
        || !ArgIdList(args, "varcstr", varcstrs, outError))
    {
        return false;
    }
    std::vector<int64_t> iv;
    std::vector<bool>    found;
    auto emitInts = [&](const char *key, const char *gvType, const std::vector<int32_t> &ids) {
        JsonValue rows = JArr();
        for (size_t i = 0; i < ids.size(); ++i)
        {
            JsonValue row = JObj();
            Put(row, "id", JInt(ids[i]));
            Put(row, "value", JInt(iv[i]));
            Put(row, "found", JBool(found[i]));
            Put(row, "name", JStr(gvType ? a.gameval.NameForCacheType(gvType, ids[i]) : nullptr));
            Push(rows, std::move(row));
        }
        Put(outResult, key, std::move(rows));
    };
    if (!varps.empty())
    {
        if (!ReadIntVars(a, "get_varps", varps, iv, found, outError))
        {
            return false;
        }
        emitInts("varp", "varp", varps);
    }
    if (!varcs.empty())
    {
        if (!ReadIntVars(a, "get_varcs_int", varcs, iv, found, outError))
        {
            return false;
        }
        emitInts("varc", "varc", varcs);
    }
    if (!varbits.empty())
    {
        std::vector<std::string> source;
        if (!ReadVarbits(a, varbits, iv, source, outError))
        {
            return false;
        }
        JsonValue rows = JArr();
        for (size_t i = 0; i < varbits.size(); ++i)
        {
            JsonValue row = JObj();
            Put(row, "id", JInt(varbits[i]));
            Put(row, "value", source[i] == "none" ? JNull() : JInt(iv[i]));
            Put(row, "source", JStr(source[i]));
            Put(row, "name", JStr(a.gameval.NameForCacheType("varbit", varbits[i])));
            Push(rows, std::move(row));
        }
        Put(outResult, "varbit", std::move(rows));
    }
    if (!varcstrs.empty())
    {
        std::vector<std::string> sv;
        if (!ReadStrVars(a, varcstrs, sv, found, outError))
        {
            return false;
        }
        JsonValue rows = JArr();
        for (size_t i = 0; i < varcstrs.size(); ++i)
        {
            JsonValue row = JObj();
            Put(row, "id", JInt(varcstrs[i]));
            Put(row, "value", JStr(sv[i]));
            Put(row, "found", JBool(found[i]));
            Push(rows, std::move(row));
        }
        Put(outResult, "varcstr", std::move(rows));
    }
    return true;
}

// rpc {method, params?} -- allowlisted query methods only.
bool CmdRpc(app::App &a, const JsonValue &args, JsonValue &outResult, std::string &outError)
{
    const std::string method = ArgStr(args, "method");
    if (method.empty())
    {
        outError = "rpc needs {method}";
        return false;
    }
    if (!IsReadOnlyMethod(method))
    {
        outError = "refused: '" + method + "' is not on the read-only allowlist "
                   "(the debugger never issues action methods)";
        return false;
    }
    const JsonValue *params = args.Find("params");
    if (params && params->IsNull())
    {
        params = nullptr;
    }
    JsonValue reply;
    if (!CallRpc(a, method.c_str(), params, reply, outError))
    {
        return false;
    }
    Put(outResult, "method", JStr(method));
    Put(outResult, "result", std::move(reply));
    return true;
}

// cache {type, id} -> the NXTCache decoded definition (as JSON) + gameval name.
bool CmdCache(app::App &a, const JsonValue &args, JsonValue &outResult, std::string &outError)
{
    const std::string type = ArgStr(args, "type");
    int64_t           id   = -1;
    if (type.empty() || !ArgInt(args, "id", id))
    {
        outError = "cache needs {type, id}; type is one of npc item loc seq varbit enum "
                   "struct inv param quest";
        return false;
    }
    if (!EnsureCache(a))
    {
        outError = "cache not open: " + Utf8(a.cache.LastError());
        return false;
    }
    const std::string text = a.cache.GetJson(type.c_str(), static_cast<int>(id));
    Put(outResult, "type", JStr(type));
    Put(outResult, "id", JInt(id));
    Put(outResult, "gameval", JStr(a.gameval.NameForCacheType(type.c_str(), int32_t(id))));
    if (text.empty())
    {
        Put(outResult, "found", JBool(false));
        return true;
    }
    cache::JsonValue def;
    Put(outResult, "found", JBool(true));
    Put(outResult, "definition", cache::ParseJson(text, def) ? std::move(def) : JStr(text));
    return true;
}

// gameval {kind, id} -> name;  {kind, name} -> exact id;  {kind, search} -> matches.
// kind "component" takes id as the composite hash or {iface, comp}.
bool CmdGameval(app::App &a, const JsonValue &args, JsonValue &outResult, std::string &outError)
{
    const std::string kind = ArgStr(args, "kind");
    if (kind.empty())
    {
        outError = "gameval needs {kind} plus one of {id}, {name}, {search}";
        return false;
    }
    if (!gameval::GameVal::IsKnownType(kind.c_str()))
    {
        outError = "unknown gameval kind '" + kind + "'; one of: " + kGamevalKinds;
        return false;
    }
    Put(outResult, "kind", JStr(kind));
    int64_t id    = -1;
    int64_t iface = -1;
    int64_t comp  = -1;
    if (kind == "component" && ArgInt(args, "iface", iface) && ArgInt(args, "comp", comp))
    {
        id = (iface << 16) | (comp & 0xFFFF);
    }
    if (id >= 0 || ArgInt(args, "id", id))
    {
        const char *name = (kind == "component")
                               ? a.gameval.ComponentName(int32_t(id >> 16), int32_t(id & 0xFFFF))
                               : a.gameval.NameForCacheType(kind.c_str(), int32_t(id));
        Put(outResult, "id", JInt(id));
        Put(outResult, "name", JStr(name));
        return true;
    }
    const std::string name   = ArgStr(args, "name");
    const std::string needle = name.empty() ? ArgStr(args, "search") : name;
    if (needle.empty())
    {
        outError = "gameval needs one of {id}, {name}, {search}";
        return false;
    }
    int64_t maxHits = static_cast<int64_t>(kGamevalMaxHits);
    ArgInt(args, "max", maxHits);
    maxHits = std::clamp<int64_t>(maxHits, 0, static_cast<int64_t>(kGamevalMaxHits));
    std::vector<gameval::GameVal::Match> hits;
    const size_t total = a.gameval.Search(kind.c_str(), needle, !name.empty(),
                                          static_cast<size_t>(maxHits), hits);
    JsonValue    rows  = JArr();
    for (const auto &h : hits)
    {
        JsonValue row = JObj();
        Put(row, "id", JInt(h.id));
        Put(row, "name", JStr(h.name));
        if (kind == "component")
        {
            Put(row, "iface", JInt(h.id >> 16));
            Put(row, "comp", JInt(h.id & 0xFFFF));
        }
        Push(rows, std::move(row));
    }
    Put(outResult, "total", JInt(static_cast<int64_t>(total)));
    Put(outResult, "matches", std::move(rows));
    return true;
}

// cs2 {id, decompiled?} -> the CS2 script panel's source text.
bool CmdCs2(app::App &a, const JsonValue &args, JsonValue &outResult, std::string &outError)
{
    int64_t id = -1;
    if (!ArgInt(args, "id", id) || id < 0)
    {
        outError = "cs2 needs {id}";
        return false;
    }
    if (!ApplyCs2Dir(a, args, outError))
    {
        return false;
    }
    const bool        isDecompiled = ArgBool(args, "decompiled", false);
    const std::wstring path         = a.cs2.ScriptFile(static_cast<int>(id), isDecompiled);
    std::string        text;
    if (!ReadScriptFile(path, text))
    {
        outError = "could not open " + Utf8(path)
                 + " (point at the cs2_asm corpus with {dir} or --cs2-dir)";
        return false;
    }
    Put(outResult, "id", JInt(id));
    Put(outResult, "source", JStr(isDecompiled ? "decompiled" : "asm"));
    Put(outResult, "path", JStr(Utf8(path)));
    Put(outResult, "text", JStr(std::move(text)));
    return true;
}

// xref {kind: varbit|varp|const, id} -> referencing CS2 script ids. Starts the
// index build on first use; answers {status: "building"} until it is ready.
bool CmdXref(app::App &a, const JsonValue &args, JsonValue &outResult, std::string &outError)
{
    const std::string kind = ArgStr(args, "kind");
    int64_t           id   = -1;
    if (!ArgInt(args, "id", id) || (kind != "varbit" && kind != "varp" && kind != "const"))
    {
        outError = "xref needs {kind: varbit|varp|const, id}";
        return false;
    }
    if (!ApplyCs2Dir(a, args, outError))
    {
        return false;
    }
    using Status = cs2::Cs2Index::Status;
    const Status st = a.cs2.GetStatus();
    if (st == Status::Idle)
    {
        a.cs2.BuildAsync(false);
    }
    if (st != Status::Ready)
    {
        Put(outResult, "status", JStr(st == Status::Error ? "error" : "building"));
        Put(outResult, "files_done", JInt(static_cast<int64_t>(a.cs2.FilesDone())));
        Put(outResult, "files_total", JInt(static_cast<int64_t>(a.cs2.FilesTotal())));
        if (st == Status::Error)
        {
            Put(outResult, "error", JStr(Utf8(a.cs2.LastError())));
        }
        return true;
    }
    const std::vector<int> *hits = (kind == "varbit") ? a.cs2.Varbit(int(id))
                                   : (kind == "varp") ? a.cs2.Varp(int(id))
                                                      : a.cs2.Const(int(id));
    JsonValue scripts = JArr();
    const size_t n = hits ? std::min(hits->size(), kXrefMaxScripts) : 0;
    for (size_t i = 0; i < n; ++i)
    {
        Push(scripts, JInt((*hits)[i]));
    }
    Put(outResult, "status", JStr("ready"));
    Put(outResult, "total", JInt(hits ? static_cast<int64_t>(hits->size()) : 0));
    Put(outResult, "scripts", std::move(scripts));
    return true;
}

}
