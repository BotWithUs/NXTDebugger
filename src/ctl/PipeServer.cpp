#include "PipeServer.h"

#include "log/Log.h"

#include <sddl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <utility>
#include <vector>

namespace nxtdbg::ctl
{

namespace
{

constexpr DWORD  kPipeBufferBytes = 64 * 1024;
constexpr size_t kMaxLineBytes    = 1024 * 1024;
constexpr auto   kReplyTimeout    = std::chrono::seconds(30);
constexpr auto   kReplyPollSlice  = std::chrono::milliseconds(50);

OwnedHandle CreateInstance(PipeServer &srv, const std::wstring &name, bool isFirst)
{
    DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
    if (isFirst)
    {
        openMode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
    }
    HANDLE h = CreateNamedPipeW(name.c_str(), openMode,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
                                    | PIPE_REJECT_REMOTE_CLIENTS,
                                PIPE_UNLIMITED_INSTANCES, kPipeBufferBytes, kPipeBufferBytes,
                                0, srv.Security());
    return OwnedHandle(h);
}

// One overlapped read or write that gives up when the server's stop event
// fires. False on stop, error, or a zero-byte read (client went away).
bool DoIo(HANDLE pipe, HANDLE stopEvent, bool isWrite, void *buf, DWORD len, DWORD &outGot)
{
    outGot = 0;
    OwnedHandle ev(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!ev.IsValid())
    {
        return false;
    }
    OVERLAPPED ov{};
    ov.hEvent = ev.Get();
    BOOL ok   = isWrite ? WriteFile(pipe, buf, len, nullptr, &ov)
                        : ReadFile(pipe, buf, len, nullptr, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING)
    {
        return false;
    }
    HANDLE waits[2] = { ev.Get(), stopEvent };
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0)
    {
        CancelIoEx(pipe, &ov);
        DWORD scratch = 0;
        GetOverlappedResult(pipe, &ov, &scratch, TRUE);
        return false;
    }
    if (!GetOverlappedResult(pipe, &ov, &outGot, FALSE))
    {
        return false;
    }
    return outGot > 0;
}

bool WriteAll(HANDLE pipe, HANDLE stopEvent, const std::string &data)
{
    size_t sent = 0;
    while (sent < data.size())
    {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - sent, 1u << 20));
        DWORD got   = 0;
        if (!DoIo(pipe, stopEvent, true, const_cast<char *>(data.data() + sent), chunk, got))
        {
            return false;
        }
        sent += got;
    }
    return true;
}

bool WaitForClient(PipeServer &srv, HANDLE pipe)
{
    OwnedHandle ev(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!ev.IsValid())
    {
        return false;
    }
    OVERLAPPED ov{};
    ov.hEvent = ev.Get();
    if (ConnectNamedPipe(pipe, &ov))
    {
        return true;
    }
    DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED)
    {
        return true;
    }
    if (err != ERROR_IO_PENDING)
    {
        return false;
    }
    HANDLE waits[2] = { ev.Get(), srv.StopEvent() };
    DWORD  scratch  = 0;
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0)
    {
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, &scratch, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &ov, &scratch, FALSE) != FALSE;
}

std::string ErrorLine(const char *msg)
{
    return std::string("{\"ok\":false,\"error\":\"") + msg + "\"}";
}

std::string AwaitReply(PipeServer &srv, std::string line)
{
    std::future<std::string> fut   = srv.Enqueue(std::move(line));
    const auto               start = std::chrono::steady_clock::now();
    while (fut.wait_for(kReplyPollSlice) != std::future_status::ready)
    {
        if (srv.ShouldStop())
        {
            return ErrorLine("debugger is shutting down");
        }
        if (std::chrono::steady_clock::now() - start > kReplyTimeout)
        {
            return ErrorLine("timed out waiting for the debugger UI thread");
        }
    }
    return fut.get();
}

// Split accumulated bytes into lines and answer each. False when the client
// should be dropped (write failed, or an over-long line).
bool ServeLines(PipeServer &srv, HANDLE pipe, std::string &acc)
{
    size_t nl = acc.find('\n');
    while (nl != std::string::npos)
    {
        std::string line = acc.substr(0, nl);
        acc.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (!line.empty())
        {
            std::string resp = AwaitReply(srv, std::move(line));
            resp.push_back('\n');
            if (!WriteAll(pipe, srv.StopEvent(), resp))
            {
                return false;
            }
        }
        nl = acc.find('\n');
    }
    if (acc.size() > kMaxLineBytes)
    {
        WriteAll(pipe, srv.StopEvent(), ErrorLine("request line too long") + "\n");
        return false;
    }
    return true;
}

void ServeConnection(std::stop_token, PipeServer *srv, OwnedHandle pipe,
                     std::shared_ptr<std::atomic<bool>> isDone)
{
    std::string acc;
    char        buf[8192];
    while (!srv->ShouldStop())
    {
        DWORD got = 0;
        if (!DoIo(pipe.Get(), srv->StopEvent(), false, buf, sizeof(buf), got))
        {
            break;
        }
        acc.append(buf, got);
        if (!ServeLines(*srv, pipe.Get(), acc))
        {
            break;
        }
    }
    DisconnectNamedPipe(pipe.Get());
    pipe.Reset();
    isDone->store(true, std::memory_order_release);
}

// Accept loop for one pipe name. `listening` is the already-created first
// instance (Start creates it synchronously so it can report failure). The next
// instance is created BEFORE the connected one is handed off, so the name never
// has zero instances -- that is what keeps the alias owned by this process.
void Listen(std::stop_token, PipeServer *srv, std::wstring name, OwnedHandle listening)
{
    while (!srv->ShouldStop())
    {
        if (!listening.IsValid())
        {
            listening = CreateInstance(*srv, name, false);
            if (!listening.IsValid())
            {
                if (WaitForSingleObject(srv->StopEvent(), 500) == WAIT_OBJECT_0)
                {
                    return;
                }
                continue;
            }
        }
        if (!WaitForClient(*srv, listening.Get()))
        {
            listening.Reset();
            continue;
        }
        OwnedHandle next = CreateInstance(*srv, name, false);
        srv->Adopt(std::move(listening));
        listening = std::move(next);
    }
}

}

OwnedHandle &OwnedHandle::operator=(OwnedHandle &&o) noexcept
{
    if (this != &o)
    {
        Reset();
        handle = o.Release();
    }
    return *this;
}

HANDLE OwnedHandle::Release()
{
    HANDLE h = handle;
    handle   = nullptr;
    return h;
}

void OwnedHandle::Reset()
{
    if (IsValid())
    {
        CloseHandle(handle);
    }
    handle = nullptr;
}

// DACL granting GENERIC_ALL to the current user's SID and nobody else;
// protected (P) so nothing is inherited. Remote clients are refused separately
// by PIPE_REJECT_REMOTE_CLIENTS.
bool PipeServer::BuildSecurity()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        return false;
    }
    OwnedHandle tokenOwner(token);
    DWORD       need = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &need);
    std::vector<uint8_t> info(need);
    if (need == 0 || !GetTokenInformation(token, TokenUser, info.data(), need, &need))
    {
        return false;
    }
    const auto *user   = reinterpret_cast<const TOKEN_USER *>(info.data());
    LPWSTR      sidStr = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sidStr))
    {
        return false;
    }
    std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sidStr) + L")";
    LocalFree(sidStr);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
    {
        descriptor = nullptr;
        return false;
    }
    security.nLength              = sizeof(security);
    security.lpSecurityDescriptor = descriptor;
    security.bInheritHandle       = FALSE;
    securityReady                 = true;
    return true;
}

bool PipeServer::Start(DWORD ownPid)
{
    if (isRunning)
    {
        return true;
    }
    if (!BuildSecurity())
    {
        // Refuse rather than fall back to the default DACL: a pipe that answers
        // anyone on the box is not what --ctl promises.
        log::LogError(L"ctl: could not build the current-user pipe ACL; control pipe disabled");
        return false;
    }
    wchar_t name[80];
    std::swprintf(name, 80, L"\\\\.\\pipe\\nxtdebugger-ctl-%lu", ownPid);
    pipeName  = name;
    aliasName = L"\\\\.\\pipe\\nxtdebugger-ctl";

    stopEvent = OwnedHandle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    OwnedHandle primary = CreateInstance(*this, pipeName, true);
    if (!stopEvent.IsValid() || !primary.IsValid())
    {
        wchar_t msg[160];
        std::swprintf(msg, 160, L"ctl: CreateNamedPipeW(%s) failed: %lu",
                      pipeName.c_str(), GetLastError());
        log::LogError(msg);
        return false;
    }
    OwnedHandle alias = CreateInstance(*this, aliasName, true);
    SetHasAlias(alias.IsValid());

    isStopping.store(false, std::memory_order_release);
    listener = std::jthread(Listen, this, pipeName, std::move(primary));
    if (alias.IsValid())
    {
        aliasListener = std::jthread(Listen, this, aliasName, std::move(alias));
    }
    isRunning = true;

    wchar_t msg[200];
    std::swprintf(msg, 200, L"ctl: listening on %s%s", pipeName.c_str(),
                  HasAlias() ? L" (+ alias \\\\.\\pipe\\nxtdebugger-ctl)"
                             : L" (alias held by another instance)");
    log::LogInfo(msg);
    return true;
}

void PipeServer::Stop()
{
    if (isRunning)
    {
        isStopping.store(true, std::memory_order_release);
        SetEvent(stopEvent.Get());
        listener      = std::jthread();
        aliasListener = std::jthread();
        std::list<Connection> doomed;
        {
            std::lock_guard lock(connMutex);
            doomed.swap(connections);
        }
        doomed.clear();   // joins every connection thread
        {
            std::lock_guard lock(queueMutex);
            queue.clear();
        }
        stopEvent.Reset();
        isRunning = false;
        SetHasAlias(false);
    }
    if (descriptor)
    {
        LocalFree(descriptor);
        descriptor = nullptr;
    }
    securityReady = false;
}

uint32_t PipeServer::ConnectionCount()
{
    std::lock_guard lock(connMutex);
    uint32_t        n = 0;
    for (const Connection &c : connections)
    {
        if (!c.isDone->load(std::memory_order_acquire))
        {
            ++n;
        }
    }
    return n;
}

void PipeServer::TakePending(std::vector<std::shared_ptr<CtlRequest>> &outRequests)
{
    std::lock_guard lock(queueMutex);
    for (auto &r : queue)
    {
        outRequests.push_back(std::move(r));
    }
    queue.clear();
}

std::future<std::string> PipeServer::Enqueue(std::string line)
{
    auto req  = std::make_shared<CtlRequest>();
    req->line = std::move(line);
    std::future<std::string> fut = req->reply.get_future();
    std::lock_guard          lock(queueMutex);
    queue.push_back(std::move(req));
    return fut;
}

void PipeServer::ReapFinished()
{
    std::list<Connection> finished;
    {
        std::lock_guard lock(connMutex);
        for (auto it = connections.begin(); it != connections.end();)
        {
            auto next = std::next(it);
            if (it->isDone->load(std::memory_order_acquire))
            {
                finished.splice(finished.end(), connections, it);
            }
            it = next;
        }
    }
    // Joined outside the lock; each has already returned.
}

void PipeServer::Adopt(OwnedHandle pipe)
{
    ReapFinished();
    auto isDone = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard lock(connMutex);
    connections.push_back(Connection{
        isDone, std::jthread(ServeConnection, this, std::move(pipe), isDone) });
}

}
