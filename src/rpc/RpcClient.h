#pragma once
#include <Windows.h>
#include <cstdint>
#include <vector>

namespace nxtdbg::rpc
{

// Outcome of a single Call(). Distinguishes the cases the UI cares about:
//   Ok            — response arrived, result bytes populated
//   Busy          — CreateFile got ERROR_PIPE_BUSY (every slot taken)
//   Disconnected  — pipe broke mid-call OR Connect was never made
//   Timeout       — caller's timeout elapsed before the response did
//   ProtocolError — reply parsed but the envelope was malformed
//   HandlerError  — agent replied with {id, error: "..."} instead of result
enum class CallStatus : uint8_t
{
    Ok,
    Busy,
    Disconnected,
    Timeout,
    ProtocolError,
    HandlerError,
};

// RAII over a single client-side pipe HANDLE. NOT thread-safe by design — a
// single console panel issues one Call at a time on the UI thread, with
// overlapped I/O so a long handler doesn't freeze the frame loop. The
// integration shape is "issue Call, poll-or-wait, render result" rather
// than a background worker queue.
class RpcClient
{
public:
    RpcClient();
    RpcClient(const RpcClient &)            = delete;
    RpcClient &operator=(const RpcClient &) = delete;
    ~RpcClient();

    // Open \\.\pipe\BotWithUs_<pid>. Returns true on success. On false call
    // LastStatus() — Busy means every slot taken, Disconnected means the
    // agent isn't there or the open failed for another reason.
    bool Connect(DWORD pid);
    void Disconnect();

    bool  IsConnected() const { return pipe_ != INVALID_HANDLE_VALUE; }
    DWORD Pid()         const { return pid_; }

    // Last connect / call status — drives the status pill in the UI.
    CallStatus     LastStatus()  const { return lastStatus_; }
    const wchar_t *LastError()   const { return lastErr_; }

    // Issue one synchronous RPC. paramsMp is the already-encoded msgpack
    // value for the "params" slot (typically a map); pass an empty span for
    // no-param methods. resultOut receives the raw msgpack bytes of the
    // "result" value on Ok, or the error-string bytes on HandlerError.
    // timeoutMs of 0 means "wait forever (or until disconnect)".
    CallStatus Call(const char           *method,
                    const uint8_t        *paramsMp,
                    uint32_t              paramsLen,
                    std::vector<uint8_t> &resultOut,
                    DWORD                 timeoutMs);

private:
    HANDLE     pipe_       = INVALID_HANDLE_VALUE;
    DWORD      pid_        = 0;
    int64_t    nextId_     = 1;
    CallStatus lastStatus_ = CallStatus::Disconnected;
    wchar_t    lastErr_[256]{};
};

}
