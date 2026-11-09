#pragma once
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The control pipe's transport. Owns the listener and per-connection threads;
// knows nothing about what a request means.
//
// Threading contract: pipe I/O runs on this class's own threads, but requests
// are NOT answered there. Each complete line is queued as a CtlRequest and the
// connection thread blocks on its future; the UI thread drains the queue once
// per frame (TakePending) and fulfils each promise. That keeps every request on
// the thread that owns App::rpc, the session and the panels' shared state, so
// nothing here races the single synchronous RPC client.
namespace nxtdbg::ctl
{

struct CtlRequest
{
    std::string              line;    // one request, newline stripped
    std::promise<std::string> reply;  // one response line, newline NOT included
};

// RAII over a Win32 HANDLE (INVALID_HANDLE_VALUE and nullptr both mean empty).
class OwnedHandle
{
public:
    OwnedHandle() = default;
    explicit OwnedHandle(HANDLE h) : handle(h) {}
    OwnedHandle(const OwnedHandle &)            = delete;
    OwnedHandle &operator=(const OwnedHandle &) = delete;
    OwnedHandle(OwnedHandle &&o) noexcept : handle(o.Release()) {}
    OwnedHandle &operator=(OwnedHandle &&o) noexcept;
    ~OwnedHandle() { Reset(); }

    HANDLE Get() const { return handle; }
    bool   IsValid() const { return handle != nullptr && handle != INVALID_HANDLE_VALUE; }
    HANDLE Release();
    void   Reset();

private:
    HANDLE handle = nullptr;
};

class PipeServer
{
public:
    PipeServer() = default;
    PipeServer(const PipeServer &)            = delete;
    PipeServer &operator=(const PipeServer &) = delete;
    ~PipeServer() { Stop(); }

    // Starts listening on \\.\pipe\nxtdebugger-ctl-<ownPid> and tries to claim
    // the alias \\.\pipe\nxtdebugger-ctl (first debugger instance wins). False
    // only if the per-pid pipe could not be created; losing the alias is not
    // an error. Idempotent while running.
    bool Start(DWORD ownPid);
    void Stop();

    bool IsRunning() const { return isRunning; }
    bool HasAlias()  const { return hasAlias.load(std::memory_order_acquire); }
    const std::wstring &PipeName()  const { return pipeName; }
    const std::wstring &AliasName() const { return aliasName; }
    uint32_t ConnectionCount();

    // UI thread: move every queued request into outRequests (appended).
    void TakePending(std::vector<std::shared_ptr<CtlRequest>> &outRequests);

    // Connection threads: queue one request. Returns the future to wait on.
    std::future<std::string> Enqueue(std::string line);

    HANDLE StopEvent() const { return stopEvent.Get(); }
    bool   ShouldStop() const { return isStopping.load(std::memory_order_acquire); }

    // Listener threads: hand a connected pipe instance to a new connection thread.
    void Adopt(OwnedHandle pipe);
    void SetHasAlias(bool v) { hasAlias.store(v, std::memory_order_release); }
    SECURITY_ATTRIBUTES *Security() { return securityReady ? &security : nullptr; }

private:
    struct Connection
    {
        std::shared_ptr<std::atomic<bool>> isDone;
        std::jthread                       thread;
    };

    bool BuildSecurity();
    void ReapFinished();

    bool                    isRunning = false;
    std::atomic<bool>       isStopping{false};
    std::atomic<bool>       hasAlias{false};
    std::wstring            pipeName;
    std::wstring            aliasName;
    OwnedHandle             stopEvent;

    SECURITY_ATTRIBUTES     security{};
    PSECURITY_DESCRIPTOR    descriptor = nullptr;   // LocalFree'd in Stop()
    bool                    securityReady = false;

    std::mutex                               queueMutex;
    std::vector<std::shared_ptr<CtlRequest>> queue;

    std::mutex              connMutex;
    std::list<Connection>   connections;

    // Declared last: destroyed (joined) first, before the state they use.
    std::jthread            listener;
    std::jthread            aliasListener;
};

}
