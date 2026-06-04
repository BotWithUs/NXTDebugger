#include "RpcClient.h"

#include "log/Log.h"
#include "rpc/MsgPack.h"

#include <cstdio>
#include <cstring>

namespace nxtdbg::rpc
{

namespace
{

constexpr uint32_t kMaxFrameBytes = 4 * 1024 * 1024;   // matches NXTLibrary kMaxMsgSize

// Build \\.\pipe\BotWithUs_<pid> into out. Caller-allocated buffer is
// sized for the longest plausible name (~32 chars + NUL).
void BuildPipeName(wchar_t *out, size_t cap, DWORD pid)
{
    std::swprintf(out, cap, L"\\\\.\\pipe\\BotWithUs_%lu", pid);
}

// Wait for an overlapped event with a bounded timeout (0 means INFINITE).
// Returns true iff the op completed within the window.
bool WaitOverlappedTimed(HANDLE pipe, OVERLAPPED &ov, DWORD timeoutMs,
                         DWORD &bytesOut)
{
    DWORD waitMs = (timeoutMs == 0) ? INFINITE : timeoutMs;
    DWORD r = WaitForSingleObject(ov.hEvent, waitMs);
    if (r != WAIT_OBJECT_0)
    {
        CancelIoEx(pipe, &ov);
        DWORD scratch = 0;
        GetOverlappedResult(pipe, &ov, &scratch, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &ov, &bytesOut, FALSE) != FALSE;
}

bool ReadExactTimed(HANDLE pipe, void *buf, DWORD n, DWORD timeoutMs,
                    OVERLAPPED &ov)
{
    DWORD got = 0;
    while (got < n)
    {
        ResetEvent(ov.hEvent);
        DWORD chunk = 0;
        BOOL ok = ReadFile(pipe, static_cast<uint8_t *>(buf) + got,
                           n - got, &chunk, &ov);
        if (!ok)
        {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) return false;
            if (!WaitOverlappedTimed(pipe, ov, timeoutMs, chunk)) return false;
        }
        if (chunk == 0) return false;
        got += chunk;
    }
    return true;
}

bool WriteAllTimed(HANDLE pipe, const void *buf, DWORD n, DWORD timeoutMs,
                   OVERLAPPED &ov)
{
    DWORD sent = 0;
    while (sent < n)
    {
        ResetEvent(ov.hEvent);
        DWORD chunk = 0;
        BOOL ok = WriteFile(pipe, static_cast<const uint8_t *>(buf) + sent,
                            n - sent, &chunk, &ov);
        if (!ok)
        {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) return false;
            if (!WaitOverlappedTimed(pipe, ov, timeoutMs, chunk)) return false;
        }
        if (chunk == 0) return false;
        sent += chunk;
    }
    return true;
}

}

RpcClient::RpcClient() = default;

RpcClient::~RpcClient()
{
    Disconnect();
}

bool RpcClient::Connect(DWORD pid)
{
    Disconnect();

    wchar_t name[64];
    BuildPipeName(name, 64, pid);

    HANDLE p = CreateFileW(name,
                           GENERIC_READ | GENERIC_WRITE,
                           0,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED,
                           nullptr);
    if (p == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY)
        {
            lastStatus_ = CallStatus::Busy;
            std::swprintf(lastErr_, 256,
                          L"All %u pipe slots taken — close another client",
                          4u);
        }
        else
        {
            lastStatus_ = CallStatus::Disconnected;
            std::swprintf(lastErr_, 256,
                          L"CreateFileW(%s) failed: %lu", name, err);
        }
        log::LogWarn(lastErr_);
        return false;
    }

    // Byte mode matches the server (PIPE_TYPE_BYTE on the producer side).
    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(p, &mode, nullptr, nullptr);

    pipe_       = p;
    pid_        = pid;
    lastStatus_ = CallStatus::Ok;
    std::swprintf(lastErr_, 256, L"Connected to pipe pid=%lu", pid);
    log::LogInfo(lastErr_);
    lastErr_[0] = 0;
    return true;
}

void RpcClient::Disconnect()
{
    if (pipe_ != INVALID_HANDLE_VALUE)
    {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    pid_ = 0;
    lastStatus_ = CallStatus::Disconnected;
}

CallStatus RpcClient::Call(const char *method,
                           const uint8_t *paramsMp, uint32_t paramsLen,
                           std::vector<uint8_t> &resultOut,
                           DWORD timeoutMs)
{
    resultOut.clear();
    if (pipe_ == INVALID_HANDLE_VALUE)
    {
        lastStatus_ = CallStatus::Disconnected;
        std::swprintf(lastErr_, 256, L"Call(%hs): not connected", method);
        return lastStatus_;
    }

    // Encode the request envelope {id, method, params?} into a small heap
    // buffer. 1 KB headroom for the envelope keys + method name + id; the
    // params slice is appended verbatim.
    std::vector<uint8_t> reqBuf;
    reqBuf.resize(paramsLen + 1024);
    nxt::rpc::msgpack::Writer w(reqBuf.data() + 4,
                                static_cast<size_t>(reqBuf.size() - 4));
    const int64_t id = nextId_++;
    const uint32_t mapCount = (paramsLen > 0) ? 3 : 2;
    w.WriteMapHeader(mapCount);
    w.WriteCStr("id");        w.WriteInt(id);
    w.WriteCStr("method");    w.WriteCStr(method);
    if (paramsLen > 0)
    {
        w.WriteCStr("params");
        if (paramsLen > kMaxFrameBytes - w.BytesWritten() - 16)
        {
            lastStatus_ = CallStatus::ProtocolError;
            std::swprintf(lastErr_, 256, L"Call(%hs): params too large (%u bytes)",
                          method, paramsLen);
            return lastStatus_;
        }
        std::memcpy(reqBuf.data() + 4 + w.BytesWritten(), paramsMp, paramsLen);
        // BytesWritten() doesn't know about the raw splice; advance the
        // logical length manually for the frame-length prefix below.
    }
    const uint32_t bodyLen = static_cast<uint32_t>(w.BytesWritten()) + paramsLen;
    if (w.Overflowed())
    {
        lastStatus_ = CallStatus::ProtocolError;
        std::swprintf(lastErr_, 256, L"Call(%hs): envelope overflow", method);
        return lastStatus_;
    }

    // 4-byte LE length prefix.
    reqBuf[0] = static_cast<uint8_t>(bodyLen      );
    reqBuf[1] = static_cast<uint8_t>(bodyLen >> 8 );
    reqBuf[2] = static_cast<uint8_t>(bodyLen >> 16);
    reqBuf[3] = static_cast<uint8_t>(bodyLen >> 24);

    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
    {
        lastStatus_ = CallStatus::Disconnected;
        std::swprintf(lastErr_, 256, L"CreateEventW failed: %lu", GetLastError());
        return lastStatus_;
    }

    auto withEvent = [&](CallStatus s) -> CallStatus
    {
        CloseHandle(ov.hEvent);
        lastStatus_ = s;
        return s;
    };

    if (!WriteAllTimed(pipe_, reqBuf.data(), 4 + bodyLen, timeoutMs, ov))
    {
        DWORD err = GetLastError();
        std::swprintf(lastErr_, 256, L"Call(%hs): write failed (%lu)",
                      method, err);
        Disconnect();
        return withEvent(CallStatus::Disconnected);
    }

    uint32_t respLen = 0;
    if (!ReadExactTimed(pipe_, &respLen, 4, timeoutMs, ov))
    {
        std::swprintf(lastErr_, 256, L"Call(%hs): read len timed out / failed",
                      method);
        Disconnect();
        return withEvent(CallStatus::Timeout);
    }
    if (respLen == 0 || respLen > kMaxFrameBytes)
    {
        std::swprintf(lastErr_, 256, L"Call(%hs): bad response frame %u",
                      method, respLen);
        Disconnect();
        return withEvent(CallStatus::ProtocolError);
    }

    std::vector<uint8_t> respBuf;
    respBuf.resize(respLen);
    if (!ReadExactTimed(pipe_, respBuf.data(), respLen, timeoutMs, ov))
    {
        std::swprintf(lastErr_, 256, L"Call(%hs): read body timed out / failed",
                      method);
        Disconnect();
        return withEvent(CallStatus::Timeout);
    }

    // Parse {id, result} or {id, error}.
    nxt::rpc::msgpack::Reader r(respBuf.data(), respBuf.size());
    uint32_t mc;
    if (!r.ReadMapHeader(mc))
    {
        std::swprintf(lastErr_, 256, L"Call(%hs): reply not a map", method);
        return withEvent(CallStatus::ProtocolError);
    }
    int64_t replyId = 0;
    bool isError = false;
    const uint8_t *valuePtr = nullptr;
    uint32_t       valueLen = 0;
    for (uint32_t i = 0; i < mc; ++i)
    {
        const char *k = nullptr;
        uint32_t    kLen = 0;
        if (!r.ReadString(k, kLen))
        {
            std::swprintf(lastErr_, 256, L"Call(%hs): malformed reply key", method);
            return withEvent(CallStatus::ProtocolError);
        }
        if (nxt::rpc::msgpack::StrEq(k, kLen, "id"))
        {
            if (!r.ReadInt(replyId))
            {
                std::swprintf(lastErr_, 256, L"Call(%hs): bad id type", method);
                return withEvent(CallStatus::ProtocolError);
            }
        }
        else if (nxt::rpc::msgpack::StrEq(k, kLen, "result"))
        {
            valuePtr = respBuf.data() + r.Pos();
            valueLen = static_cast<uint32_t>(respBuf.size() - r.Pos());
            if (!r.SkipValue())
            {
                std::swprintf(lastErr_, 256, L"Call(%hs): bad result value", method);
                return withEvent(CallStatus::ProtocolError);
            }
            valueLen -= static_cast<uint32_t>(respBuf.size() - r.Pos());
        }
        else if (nxt::rpc::msgpack::StrEq(k, kLen, "error"))
        {
            isError = true;
            valuePtr = respBuf.data() + r.Pos();
            valueLen = static_cast<uint32_t>(respBuf.size() - r.Pos());
            if (!r.SkipValue())
            {
                std::swprintf(lastErr_, 256, L"Call(%hs): bad error value", method);
                return withEvent(CallStatus::ProtocolError);
            }
            valueLen -= static_cast<uint32_t>(respBuf.size() - r.Pos());
        }
        else
        {
            if (!r.SkipValue())
            {
                std::swprintf(lastErr_, 256, L"Call(%hs): bad extra key", method);
                return withEvent(CallStatus::ProtocolError);
            }
        }
    }

    if (replyId != id)
    {
        std::swprintf(lastErr_, 256,
                      L"Call(%hs): reply id %lld != expected %lld",
                      method, static_cast<long long>(replyId),
                      static_cast<long long>(id));
        return withEvent(CallStatus::ProtocolError);
    }

    if (valuePtr && valueLen > 0)
    {
        resultOut.assign(valuePtr, valuePtr + valueLen);
    }
    return withEvent(isError ? CallStatus::HandlerError : CallStatus::Ok);
}

}
