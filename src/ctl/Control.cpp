// Control pipe entry points: lifecycle, per-frame pump, request dispatch and
// the session-level commands (status, attach, detach, quit, help).
#include "ctl/Control.h"
#include "ctl/Commands.h"

#include "app/App.h"
#include "attach/ProcessList.h"
#include "log/Log.h"
#include "wire/SnapshotReader.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace nxtdbg::ctl
{

namespace
{

constexpr ULONGLONG kAttachRetryMs = 3000;
constexpr ULONGLONG kQuitDelayMs   = 250;   // lets the `quit` reply reach the client
constexpr int32_t   kGameStateInGame = 30;

bool CmdStatus(app::App &, const JsonValue &, JsonValue &, std::string &);
bool CmdAttach(app::App &, const JsonValue &, JsonValue &, std::string &);
bool CmdDetach(app::App &, const JsonValue &, JsonValue &, std::string &);
bool CmdQuit  (app::App &, const JsonValue &, JsonValue &, std::string &);
bool CmdHelp  (app::App &, const JsonValue &, JsonValue &, std::string &);
bool CmdScene (app::App &, const JsonValue &, JsonValue &, std::string &);

struct CommandEntry
{
    const char *name;
    CommandFn   fn;
    const char *usage;
};

constexpr CommandEntry kCommands[] = {
    { "status",  CmdStatus,  "status" },
    { "scene",   CmdScene,   "scene  (self, npcs, projectiles from the snapshot)" },
    { "tree",    CmdTree,    "tree {iface, comp?}" },
    { "comp",    CmdComp,    "comp {iface, comp, sub?}" },
    { "vars",    CmdVars,    "vars {varp?, varbit?, varc?, varcstr?}  (int or [ints])" },
    { "watch",   CmdWatch,   "watch {varp?, varbit?, varc?, varcstr?, iface?, var_interval_ms?, "
                             "iface_interval_ms?, geometry?, replace?}" },
    { "unwatch", CmdUnwatch, "unwatch {varp?, varbit?, varc?, varcstr?, iface?} | {all: true}" },
    { "changes", CmdChanges, "changes {since?, max?, types?}" },
    { "events",  CmdEvents,  "events {since?, max?, types?}" },
    { "rpc",     CmdRpc,     "rpc {method, params?}  (read-only allowlist)" },
    { "cache",   CmdCache,   "cache {type, id}" },
    { "gameval", CmdGameval, "gameval {kind, id | name | search, max?}  (kind component: id or iface+comp)" },
    { "cs2",     CmdCs2,     "cs2 {id, decompiled?, dir?}" },
    { "xref",    CmdXref,    "xref {kind: varbit|varp|const, id, dir?}" },
    { "layers",  CmdLayers,  "layers [get|set|retry] {<layer>: on|off, <layer>.color|labels|max_distance|"
                             "max_count|filter, refresh_ms, label_budget, iface}  "
                             "(layers: npc player self loc ground)" },
    { "attach",  CmdAttach,  "attach {pid} | {auto: true}" },
    { "detach",  CmdDetach,  "detach" },
    { "quit",    CmdQuit,    "quit  (closes this debugger instance)" },
    { "help",    CmdHelp,    "help" },
};

const CommandEntry *FindCommand(const std::string &name)
{
    for (const CommandEntry &c : kCommands)
    {
        if (name == c.name)
        {
            return &c;
        }
    }
    return nullptr;
}

JsonValue ErrorResponse(const JsonValue *id, const std::string &message)
{
    JsonValue r = JObj();
    Put(r, "ok", JBool(false));
    if (id)
    {
        Put(r, "req_id", *id);
    }
    Put(r, "error", JStr(message));
    return r;
}

// Request shape: {"cmd": "<name>", "req_id"?: any, ...args} or {"cmd", "args": {...}}.
std::string HandleLine(app::App &a, const std::string &line)
{
    JsonValue req;
    if (!cache::ParseJson(line, req) || !req.IsObject())
    {
        return ToJson(ErrorResponse(nullptr, "request must be one JSON object per line"));
    }
    const JsonValue *id  = req.Find("req_id");
    const std::string cmd = ArgStr(req, "cmd");
    const CommandEntry *entry = FindCommand(cmd);
    if (!entry)
    {
        return ToJson(ErrorResponse(id, "unknown cmd '" + cmd + "' (try: help)"));
    }
    const JsonValue *nested = req.Find("args");
    const JsonValue &args   = (nested && nested->IsObject()) ? *nested : req;
    JsonValue   result = JObj();
    std::string error;
    if (!entry->fn(a, args, result, error))
    {
        return ToJson(ErrorResponse(id, error));
    }
    JsonValue resp = JObj();
    Put(resp, "ok", JBool(true));
    if (id)
    {
        Put(resp, "req_id", *id);
    }
    Put(resp, "cmd", JStr(cmd));
    resp.objVal.insert(resp.objVal.end(), std::make_move_iterator(result.objVal.begin()),
                       std::make_move_iterator(result.objVal.end()));
    return ToJson(resp);
}

bool OpenSession(app::App &a, DWORD pid)
{
    if (!a.session.Open(pid))
    {
        a.ctl.attachStatus = Utf8(a.session.LastError());
        return false;
    }
    a.events.Reset();
    a.ctl.isAttachPending = false;
    a.ctl.attachStatus.clear();
    return true;
}

// --attach auto binds only to an unambiguous target: exactly one rs2client
// publishing an agent snapshot. Zero or several -> keep retrying and say why.
void TryAttachAuto(app::App &a)
{
    std::vector<attach::ProcessEntry> agents = attach::ListAgentProcesses();
    if (agents.size() == 1)
    {
        OpenSession(a, agents.front().pid);
        return;
    }
    if (agents.empty())
    {
        a.ctl.attachStatus = "waiting: no rs2client with the agent loaded";
        return;
    }
    std::string pids;
    for (const attach::ProcessEntry &p : agents)
    {
        pids += (pids.empty() ? "" : ", ") + std::to_string(p.pid);
    }
    a.ctl.attachStatus = "ambiguous: " + std::to_string(agents.size())
                       + " clients carry the agent (" + pids + "); use --attach <pid>";
}

void TryAttach(app::App &a)
{
    ControlState &c = a.ctl;
    if (!c.isAttachPending || a.session.IsOpen())
    {
        c.isAttachPending = c.isAttachPending && !a.session.IsOpen();
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (c.lastAttachTry != 0 && now - c.lastAttachTry < kAttachRetryMs)
    {
        return;
    }
    c.lastAttachTry = now;
    if (c.options.attachMode == AttachMode::Pid)
    {
        OpenSession(a, c.options.attachPid);
    }
    else if (c.options.attachMode == AttachMode::Auto)
    {
        TryAttachAuto(a);
    }
}

void PutSnapshot(app::App &a, JsonValue &out)
{
    const nxt::ipc::Snapshot *snap = wire::CurrentSnapshot(a.session);
    if (!snap)
    {
        return;
    }
    const int32_t gameState = snap->gameState;
    Put(out, "game_state", JInt(gameState));
    Put(out, "server_tick", JInt(snap->serverTick));
    Put(out, "game_cycle", JInt(snap->gameCycle));
    Put(out, "publish_seq", JInt(static_cast<int64_t>(snap->publishSeq)));
    const int32_t root = snap->rootIfaceId;
    Put(out, "root_iface", JInt(root));
    if (gameState == kGameStateInGame)
    {
        const nxt::ipc::LocalPlayer &p = snap->self;
        JsonValue player = JObj();
        Put(player, "server_index", JInt(p.serverIndex));
        Put(player, "tile_x", JInt(p.tileX));
        Put(player, "tile_y", JInt(p.tileY));
        Put(player, "plane", JInt(p.plane));
        Put(player, "combat_level", JInt(p.combatLevel));
        Put(player, "animation", JInt(p.animationId));
        Put(player, "orientation", JInt(p.orientation));
        Put(player, "target_index", JInt(p.targetIndex));
        Put(player, "flags", JInt(p.flags));
        Put(out, "player", std::move(player));
    }
    else
    {
        Put(out, "player", JNull());
    }
    const uint32_t count = std::min<uint32_t>(snap->openIfaceCount, 64);
    JsonValue      open  = JArr();
    for (uint32_t i = 0; i < count; ++i)
    {
        const int32_t id = snap->openIfaces[i];
        JsonValue     e  = JObj();
        Put(e, "id", JInt(id));
        Put(e, "name", JStr(a.gameval.NameForCacheType("if", id)));
        Push(open, std::move(e));
    }
    Put(out, "open_ifaces", std::move(open));
}

JsonValue SceneSelf(const nxt::ipc::LocalPlayer &p)
{
    constexpr int32_t kConstitution = 3;
    constexpr int32_t kPrayer       = 5;
    JsonValue self = JObj();
    Put(self, "x", JInt(p.tileX));
    Put(self, "y", JInt(p.tileY));
    Put(self, "plane", JInt(p.plane));
    Put(self, "anim", JInt(p.animationId));
    Put(self, "spot_anim", JInt(p.spotAnimId));
    Put(self, "target", JInt(p.targetIndex));
    const uint32_t skills = std::min<uint32_t>(p.skillCount, nxt::ipc::kSkillCap);
    for (uint32_t i = 0; i < skills; ++i)
    {
        const nxt::ipc::SkillEntry &k = p.skills[i];
        if (k.typeId == kConstitution)
        {
            Put(self, "constitution", JInt(k.boostedLevel));
        }
        else if (k.typeId == kPrayer)
        {
            Put(self, "prayer", JInt(k.boostedLevel));
        }
    }
    return self;
}

JsonValue SceneNpcs(app::App &a, const nxt::ipc::Snapshot &s)
{
    JsonValue      npcs  = JArr();
    const uint32_t count = std::min<uint32_t>(s.npcCount, nxt::ipc::kNpcCap);
    for (uint32_t i = 0; i < count; ++i)
    {
        const nxt::ipc::NpcEntry &n    = s.npcs[i];
        const char               *name = a.gameval.NameForCacheType("npc", n.typeId);
        JsonValue e = JObj();
        Put(e, "index", JInt(n.serverIndex));
        Put(e, "type", JInt(n.typeId));
        Put(e, "name", JStr(name ? name : ""));
        Put(e, "x", JInt(n.tileX));
        Put(e, "y", JInt(n.tileY));
        Put(e, "hp", JInt(n.hp));
        Put(e, "max_hp", JInt(n.maxHp));
        Put(e, "anim", JInt(n.animationId));
        Put(e, "spot_anim", JInt(n.spotAnimId));
        Put(e, "following", JInt(n.followingIndex));
        Push(npcs, std::move(e));
    }
    return npcs;
}

JsonValue SceneProjectiles(const nxt::ipc::Snapshot &s)
{
    JsonValue      out   = JArr();
    const uint32_t count = std::min<uint32_t>(s.projectileCount, nxt::ipc::kProjectileCap);
    for (uint32_t i = 0; i < count; ++i)
    {
        const nxt::ipc::ProjectileEntry &p = s.projectiles[i];
        JsonValue e = JObj();
        Put(e, "gfx", JInt(p.projectileId));
        Put(e, "src", JInt(p.sourceIndex));
        Put(e, "dst", JInt(p.targetIndex));
        Put(e, "from_x", JInt(p.startTileX));
        Put(e, "from_y", JInt(p.startTileY));
        Put(e, "to_x", JInt(p.endTileX));
        Put(e, "to_y", JInt(p.endTileY));
        Put(e, "start", JInt(p.startCycle));
        Put(e, "end", JInt(p.endCycle));
        Push(out, std::move(e));
    }
    return out;
}

// Read-only fight logging: the snapshot's self, NPC and projectile blocks.
bool CmdScene(app::App &a, const JsonValue &, JsonValue &out, std::string &err)
{
    const nxt::ipc::Snapshot *snap = wire::CurrentSnapshot(a.session);
    if (!snap)
    {
        err = "not attached";
        return false;
    }
    Put(out, "server_tick", JInt(snap->serverTick));
    Put(out, "game_cycle", JInt(snap->gameCycle));
    if (snap->gameState != kGameStateInGame)
    {
        Put(out, "self", JNull());
        return true;
    }
    Put(out, "self", SceneSelf(snap->self));
    Put(out, "npcs", SceneNpcs(a, *snap));
    Put(out, "projectiles", SceneProjectiles(*snap));
    return true;
}

bool CmdStatus(app::App &a, const JsonValue &, JsonValue &out, std::string &)
{
    const nxt::ipc::SharedHeader *h = a.session.Header();
    Put(out, "attached", JBool(h != nullptr));
    Put(out, "pid", h ? JInt(a.session.Pid()) : JNull());
    Put(out, "debugger_pid", JInt(GetCurrentProcessId()));
    Put(out, "protocol_version", h ? JInt(h->version) : JNull());
    Put(out, "debugger_protocol", JInt(nxt::ipc::kProtocolVersion));
    if (!h)
    {
        const std::string err = Utf8(a.session.LastError());
        Put(out, "attach_error", err.empty() ? JNull() : JStr(err));
    }
    Put(out, "attach_pending", JBool(a.ctl.isAttachPending));
    Put(out, "attach_status", a.ctl.attachStatus.empty() ? JNull() : JStr(a.ctl.attachStatus));
    PutSnapshot(a, out);

    JsonValue rpcInfo = JObj();
    Put(rpcInfo, "connected", JBool(a.rpc.IsConnected()));
    Put(rpcInfo, "status", JStr(RpcStatusName(a.rpc.LastStatus())));
    const std::string rpcErr = Utf8(a.rpc.LastError());
    Put(rpcInfo, "error", rpcErr.empty() ? JNull() : JStr(rpcErr));
    Put(out, "rpc", std::move(rpcInfo));

    JsonValue logs = JObj();
    Put(logs, "events_head", JInt(static_cast<int64_t>(a.ctl.events.Head())));
    Put(logs, "changes_head", JInt(static_cast<int64_t>(a.ctl.changes.Head())));
    Put(logs, "ring_drops", JInt(static_cast<int64_t>(a.events.DropCount())));
    Put(logs, "paused", JBool(a.paused));
    Put(out, "logs", std::move(logs));

    Put(out, "cache_open", JBool(a.cache.IsOpen()));
    JsonValue pipe = JObj();
    Put(pipe, "name", JStr(Utf8(a.ctl.pipe.PipeName())));
    Put(pipe, "alias", a.ctl.pipe.HasAlias() ? JStr(Utf8(a.ctl.pipe.AliasName())) : JNull());
    Put(pipe, "connections", JInt(a.ctl.pipe.ConnectionCount()));
    Put(out, "ctl", std::move(pipe));
    return true;
}

// attach {pid} | {auto: true}. Binds the debugger's observer session -- the
// same thing the Attach panel's button does; nothing is sent to the game.
bool CmdAttach(app::App &a, const JsonValue &args, JsonValue &out, std::string &outError)
{
    int64_t pid = 0;
    if (ArgInt(args, "pid", pid) && pid > 0)
    {
        a.ctl.options.attachMode = AttachMode::Pid;
        a.ctl.options.attachPid  = static_cast<DWORD>(pid);
    }
    else if (ArgBool(args, "auto", false))
    {
        a.ctl.options.attachMode = AttachMode::Auto;
    }
    else
    {
        outError = "attach needs {pid} or {auto: true}";
        return false;
    }
    a.session.Close();
    a.events.Reset();
    a.ctl.isAttachPending = true;
    a.ctl.lastAttachTry   = 0;
    TryAttach(a);
    Put(out, "attached", JBool(a.session.IsOpen()));
    Put(out, "pid", a.session.IsOpen() ? JInt(a.session.Pid()) : JNull());
    Put(out, "attach_status", a.ctl.attachStatus.empty() ? JNull() : JStr(a.ctl.attachStatus));
    return true;
}

bool CmdDetach(app::App &a, const JsonValue &, JsonValue &out, std::string &)
{
    a.ctl.isAttachPending = false;
    a.session.Close();
    a.events.Reset();
    Put(out, "attached", JBool(false));
    return true;
}

bool CmdQuit(app::App &a, const JsonValue &, JsonValue &out, std::string &)
{
    a.ctl.quitAt = GetTickCount64() + kQuitDelayMs;
    Put(out, "closing", JBool(true));
    Put(out, "debugger_pid", JInt(GetCurrentProcessId()));
    return true;
}

bool CmdHelp(app::App &, const JsonValue &, JsonValue &out, std::string &)
{
    JsonValue list = JArr();
    for (const CommandEntry &c : kCommands)
    {
        Push(list, JStr(c.usage));
    }
    Put(out, "commands", std::move(list));
    Put(out, "request", JStr("one JSON object per line: {\"cmd\": \"<name>\", \"req_id\"?: any, "
                             "...args} or {\"cmd\", \"args\": {...}}"));
    return true;
}

}   // namespace

void StartControl(app::App &a, const LaunchOptions &options)
{
    ControlState &c = a.ctl;
    c.options         = options;
    c.startTick       = GetTickCount64();
    c.isAttachPending = options.attachMode != AttachMode::None;
    if (!options.cs2Dir.empty())
    {
        a.cs2.SetDir(options.cs2Dir);   // before anything can start an index build
    }
    if (options.isCtlEnabled)
    {
        c.pipe.Start(GetCurrentProcessId());
    }
}

void StopControl(app::App &a)
{
    a.ctl.pipe.Stop();
}

void PumpControl(app::App &a)
{
    TryAttach(a);
    TrackAlways(a);
    PollWatches(a);

    std::vector<std::shared_ptr<CtlRequest>> pending;
    a.ctl.pipe.TakePending(pending);
    for (const std::shared_ptr<CtlRequest> &req : pending)
    {
        req->reply.set_value(HandleLine(a, req->line));
    }

    if (a.ctl.quitAt != 0 && GetTickCount64() >= a.ctl.quitAt)
    {
        a.ctl.quitAt = 0;
        log::LogInfo(L"ctl: quit requested over the control pipe");
        PostMessageW(a.mainWindow, WM_CLOSE, 0, 0);
    }
}

}
