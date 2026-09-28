// nxtdbgctl -- command-line client for NXTDebugger's control pipe.
//
//   nxtdbgctl [options] <cmd> [json-object | key=value ...]
//
// Sends one request line, prints the response line to stdout, exits 0 when the
// debugger answered {"ok": true}. `changes` / `events` with --follow stream one
// JSON object per line until interrupted. See README.md, "Control pipe".
#include "cache/Json.h"
#include "ctl/JsonOut.h"

#include <Windows.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

using nxtdbg::cache::JsonType;
using nxtdbg::cache::JsonValue;
namespace ctl = nxtdbg::ctl;

constexpr int  kExitOk       = 0;
constexpr int  kExitRejected = 1;   // the debugger answered ok:false
constexpr int  kExitPipe     = 2;   // no debugger / broken pipe
constexpr int  kExitUsage    = 3;
constexpr int  kFollowBatch  = 500;

constexpr wchar_t kAliasPipe[]  = L"\\\\.\\pipe\\nxtdebugger-ctl";
constexpr wchar_t kPipePrefix[] = L"nxtdebugger-ctl";

const char kUsage[] =
    "usage: nxtdbgctl [--pipe NAME | --pid DEBUGGER_PID] [--pretty] [--timeout MS]\n"
    "                 <cmd> [json-object | key=value ...]\n"
    "       nxtdbgctl [...] changes|events --follow [--interval MS] [since=N] [types=[..]]\n"
    "       nxtdbgctl --list\n"
    "\n"
    "Commands: status tree comp vars watch unwatch changes events rpc cache gameval\n"
    "          cs2 xref attach detach quit help   (nxtdbgctl help for arguments)\n"
    "key=value values are parsed as JSON when they parse, else taken as strings:\n"
    "  nxtdbgctl tree iface=1477\n"
    "  nxtdbgctl vars 'varp=[1,2]' varbit=5000\n"
    "Default pipe: $NXTDBGCTL_PIPE, else \\\\.\\pipe\\nxtdebugger-ctl (first debugger).\n"
    "Exit codes: 0 ok, 1 debugger said ok:false, 2 no debugger / pipe error, 3 usage.\n";

struct Options
{
    std::wstring pipe;
    bool         isPretty   = false;
    bool         isFollow   = false;
    bool         isList     = false;
    DWORD        timeoutMs  = 5000;
    DWORD        intervalMs = 100;
    std::string  cmd;
    JsonValue    args = ctl::JObj();
};

// RAII client end of the control pipe; synchronous, one line at a time.
class PipeClient
{
public:
    PipeClient() = default;
    PipeClient(const PipeClient &)            = delete;
    PipeClient &operator=(const PipeClient &) = delete;
    ~PipeClient()
    {
        if (pipe != INVALID_HANDLE_VALUE)
        {
            CloseHandle(pipe);
        }
    }

    bool Open(const std::wstring &name, DWORD timeoutMs);
    bool Exchange(const std::string &request, std::string &outResponse);

private:
    HANDLE      pipe = INVALID_HANDLE_VALUE;
    std::string pendingBytes;   // bytes read past the last newline
};

bool PipeClient::Open(const std::wstring &name, DWORD timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;)
    {
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
        {
            return true;
        }
        const DWORD err = GetLastError();
        const ULONGLONG now = GetTickCount64();
        if (err != ERROR_PIPE_BUSY || now >= deadline)
        {
            std::fprintf(stderr, "nxtdbgctl: cannot open %ls (error %lu)%s\n", name.c_str(), err,
                         err == ERROR_FILE_NOT_FOUND ? " -- is a debugger running with --ctl?"
                                                     : "");
            return false;
        }
        WaitNamedPipeW(name.c_str(), static_cast<DWORD>(deadline - now));
    }
}

bool PipeClient::Exchange(const std::string &request, std::string &outResponse)
{
    const std::string line  = request + "\n";
    DWORD             wrote = 0;
    if (!WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &wrote, nullptr)
        || wrote != line.size())
    {
        std::fprintf(stderr, "nxtdbgctl: write failed (error %lu)\n", GetLastError());
        return false;
    }
    char buf[65536];
    for (;;)
    {
        const size_t nl = pendingBytes.find('\n');
        if (nl != std::string::npos)
        {
            outResponse = pendingBytes.substr(0, nl);
            pendingBytes.erase(0, nl + 1);
            return true;
        }
        DWORD got = 0;
        if (!ReadFile(pipe, buf, sizeof(buf), &got, nullptr) || got == 0)
        {
            std::fprintf(stderr, "nxtdbgctl: debugger closed the pipe (error %lu)\n",
                         GetLastError());
            return false;
        }
        pendingBytes.append(buf, got);
    }
}

// --- output ----------------------------------------------------------------------

void WriteOut(const std::string &s)
{
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void Indent(std::string &out, int depth)
{
    out.append(size_t(depth) * 2, ' ');
}

void Pretty(const JsonValue &v, std::string &out, int depth)
{
    const bool isArray = v.type == JsonType::Array;
    const size_t n     = isArray ? v.arrVal.size() : v.objVal.size();
    if ((v.type != JsonType::Array && v.type != JsonType::Object) || n == 0)
    {
        ctl::WriteJson(v, out);
        return;
    }
    out.push_back(isArray ? '[' : '{');
    for (size_t i = 0; i < n; ++i)
    {
        out.append(i ? ",\n" : "\n");
        Indent(out, depth + 1);
        if (!isArray)
        {
            ctl::WriteJson(ctl::JStr(v.objVal[i].first), out);
            out.append(": ");
        }
        Pretty(isArray ? v.arrVal[i] : v.objVal[i].second, out, depth + 1);
    }
    out.push_back('\n');
    Indent(out, depth);
    out.push_back(isArray ? ']' : '}');
}

void PrintValue(const JsonValue &v, bool isPretty)
{
    std::string s;
    if (isPretty)
    {
        Pretty(v, s, 0);
    }
    else
    {
        ctl::WriteJson(v, s);
    }
    WriteOut(s);
}

// --- argument parsing -----------------------------------------------------------------

std::string Narrow(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

bool AddArgument(Options &o, const std::string &arg)
{
    if (!arg.empty() && arg.front() == '{')
    {
        JsonValue obj;
        if (!nxtdbg::cache::ParseJson(arg, obj) || !obj.IsObject())
        {
            std::fprintf(stderr, "nxtdbgctl: not a JSON object: %s\n", arg.c_str());
            return false;
        }
        for (auto &kv : obj.objVal)
        {
            ctl::Put(o.args, kv.first, std::move(kv.second));
        }
        return true;
    }
    const size_t eq = arg.find('=');
    if (eq == std::string::npos || eq == 0)
    {
        std::fprintf(stderr, "nxtdbgctl: expected key=value or a JSON object, got: %s\n",
                     arg.c_str());
        return false;
    }
    const std::string key = arg.substr(0, eq);
    const std::string raw = arg.substr(eq + 1);
    JsonValue         value;
    if (!nxtdbg::cache::ParseJson(raw, value))
    {
        value = ctl::JStr(raw);
    }
    ctl::Put(o.args, key, std::move(value));
    return true;
}

bool ParseFlag(Options &o, int argc, wchar_t **argv, int &i, bool &outWasFlag)
{
    outWasFlag = true;
    const std::wstring a = argv[i];
    auto next = [&](const wchar_t *flag) -> const wchar_t * {
        if (i + 1 >= argc)
        {
            std::fprintf(stderr, "nxtdbgctl: %ls needs a value\n", flag);
            return nullptr;
        }
        return argv[++i];
    };
    if (a == L"--pretty" || a == L"--follow" || a == L"-f" || a == L"--list")
    {
        o.isPretty = o.isPretty || a == L"--pretty";
        o.isFollow = o.isFollow || a == L"--follow" || a == L"-f";
        o.isList   = o.isList || a == L"--list";
        return true;
    }
    if (a != L"--pipe" && a != L"--pid" && a != L"--timeout" && a != L"--interval")
    {
        outWasFlag = false;
        return true;
    }
    const wchar_t *v = next(a.c_str());
    if (!v)
    {
        return false;
    }
    if (a == L"--pipe")
    {
        const bool isFull = std::wcsncmp(v, L"\\\\.\\pipe\\", 9) == 0;
        o.pipe = isFull ? std::wstring(v) : std::wstring(L"\\\\.\\pipe\\") + v;
    }
    else if (a == L"--pid")
    {
        o.pipe = std::wstring(L"\\\\.\\pipe\\nxtdebugger-ctl-") + v;
    }
    else
    {
        const DWORD ms = static_cast<DWORD>(std::wcstoul(v, nullptr, 10));
        (a == L"--timeout" ? o.timeoutMs : o.intervalMs) = ms;
    }
    return true;
}

bool ParseOptions(int argc, wchar_t **argv, Options &o)
{
    for (int i = 1; i < argc; ++i)
    {
        bool wasFlag = false;
        if (!ParseFlag(o, argc, argv, i, wasFlag))
        {
            return false;
        }
        if (wasFlag)
        {
            continue;
        }
        const std::string arg = Narrow(argv[i]);
        if (o.cmd.empty())
        {
            o.cmd = arg;
        }
        else if (!AddArgument(o, arg))
        {
            return false;
        }
    }
    if (o.pipe.empty())
    {
        wchar_t env[256];
        DWORD   n = GetEnvironmentVariableW(L"NXTDBGCTL_PIPE", env, 256);
        o.pipe    = (n > 0 && n < 256) ? std::wstring(env) : std::wstring(kAliasPipe);
    }
    return o.isList || !o.cmd.empty();
}

// --- modes ------------------------------------------------------------------------------

std::string BuildRequest(const Options &o, const JsonValue &args)
{
    JsonValue req = ctl::JObj();
    ctl::Put(req, "cmd", ctl::JStr(o.cmd));
    for (const auto &kv : args.objVal)
    {
        if (kv.first != "cmd")
        {
            ctl::Put(req, kv.first, kv.second);
        }
    }
    return ctl::ToJson(req);
}

bool IsOk(const JsonValue &resp)
{
    const JsonValue *ok = resp.Find("ok");
    return ok && ok->IsBool() && ok->boolVal;
}

int RunOnce(const Options &o)
{
    PipeClient client;
    std::string resp;
    if (!client.Open(o.pipe, o.timeoutMs) || !client.Exchange(BuildRequest(o, o.args), resp))
    {
        return kExitPipe;
    }
    JsonValue parsed;
    if (!nxtdbg::cache::ParseJson(resp, parsed))
    {
        WriteOut(resp);
        return kExitRejected;
    }
    if (o.isPretty)
    {
        PrintValue(parsed, true);
    }
    else
    {
        WriteOut(resp);
    }
    return IsOk(parsed) ? kExitOk : kExitRejected;
}

void SetArg(JsonValue &args, const char *key, JsonValue v)
{
    for (auto &kv : args.objVal)
    {
        if (kv.first == key)
        {
            kv.second = std::move(v);
            return;
        }
    }
    ctl::Put(args, key, std::move(v));
}

// Streams one item per line. Starts at `since` if given, else at the current
// head (only new entries). A `truncated` reply (the cursor fell behind the
// ring) is reported on stderr and streaming continues from what remains.
int RunFollow(const Options &o)
{
    const char *itemsKey = (o.cmd == "changes") ? "changes" : "events";
    PipeClient  client;
    if (!client.Open(o.pipe, o.timeoutMs))
    {
        return kExitPipe;
    }
    JsonValue args = o.args;
    if (!args.Find("since"))
    {
        JsonValue probe = args;
        SetArg(probe, "max", ctl::JInt(0));
        std::string resp;
        JsonValue   parsed;
        if (!client.Exchange(BuildRequest(o, probe), resp) || !nxtdbg::cache::ParseJson(resp, parsed))
        {
            return kExitPipe;
        }
        if (!IsOk(parsed))
        {
            WriteOut(resp);
            return kExitRejected;
        }
        SetArg(args, "since", ctl::JInt(parsed.Find("head") ? parsed.Find("head")->AsInt() : 0));
    }
    SetArg(args, "max", ctl::JInt(kFollowBatch));
    for (;;)
    {
        std::string resp;
        JsonValue   parsed;
        if (!client.Exchange(BuildRequest(o, args), resp) || !nxtdbg::cache::ParseJson(resp, parsed))
        {
            return kExitPipe;
        }
        if (!IsOk(parsed))
        {
            WriteOut(resp);
            return kExitRejected;
        }
        if (const JsonValue *t = parsed.Find("truncated"); t && t->AsBool())
        {
            std::fprintf(stderr, "nxtdbgctl: cursor fell behind the ring; some entries lost\n");
        }
        const JsonValue *items = parsed.Find(itemsKey);
        const size_t     count = items ? items->arrVal.size() : 0;
        for (size_t i = 0; i < count; ++i)
        {
            PrintValue(items->arrVal[i], o.isPretty);
        }
        SetArg(args, "since", ctl::JInt(parsed.Find("next") ? parsed.Find("next")->AsInt() : 0));
        if (count < static_cast<size_t>(kFollowBatch))
        {
            Sleep(o.intervalMs);
        }
    }
}

int RunList()
{
    WIN32_FIND_DATAW fd{};
    HANDLE           h = FindFirstFileW(L"\\\\.\\pipe\\*", &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        return kExitPipe;
    }
    do
    {
        if (std::wcsncmp(fd.cFileName, kPipePrefix, std::wcslen(kPipePrefix)) == 0)
        {
            std::printf("\\\\.\\pipe\\%ls\n", fd.cFileName);
        }
    }
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return kExitOk;
}

}   // namespace

int wmain(int argc, wchar_t **argv)
{
    _setmode(_fileno(stdout), _O_BINARY);   // LF-only lines, UTF-8 bytes verbatim
    Options o;
    if (!ParseOptions(argc, argv, o))
    {
        std::fputs(kUsage, stderr);
        return kExitUsage;
    }
    if (o.isList)
    {
        return RunList();
    }
    if (o.isFollow)
    {
        if (o.cmd != "changes" && o.cmd != "events")
        {
            std::fprintf(stderr, "nxtdbgctl: --follow works with changes and events only\n");
            return kExitUsage;
        }
        return RunFollow(o);
    }
    return RunOnce(o);
}
