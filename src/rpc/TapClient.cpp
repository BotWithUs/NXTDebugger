#include "TapClient.h"

#include "log/Log.h"
#include "rpc/MsgPack.h"

#include <cstdio>
#include <cstring>

namespace nxtdbg::rpc
{

namespace
{

constexpr uint32_t kMaxFrameBytes = 4 * 1024 * 1024;   // matches NXTLibrary kMaxMsgSize
constexpr DWORD    kSubscribeTimeoutMs = 1000;

void BuildPipeName(wchar_t *out, size_t cap, DWORD pid)
{
    std::swprintf(out, cap, L"\\\\.\\pipe\\BotWithUs_%lu", pid);
}

// Wait for an overlapped op or the stop event. Returns true iff the op
// completed (bytesOut populated). On stop the caller breaks out of its
// loop without touching the pipe further.
bool WaitOverlapped(HANDLE pipe, OVERLAPPED &ov, HANDLE stop, DWORD &bytesOut)
{
    HANDLE waits[2] = { ov.hEvent, stop };
    DWORD r = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (r == WAIT_OBJECT_0 + 1)
    {
        CancelIoEx(pipe, &ov);
        DWORD scratch = 0;
        GetOverlappedResult(pipe, &ov, &scratch, TRUE);
        return false;
    }
    if (r != WAIT_OBJECT_0) return false;
    return GetOverlappedResult(pipe, &ov, &bytesOut, FALSE) != FALSE;
}

bool ReadExact(HANDLE pipe, void *buf, DWORD n, HANDLE stop, OVERLAPPED &ov)
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
            if (!WaitOverlapped(pipe, ov, stop, chunk)) return false;
        }
        if (chunk == 0) return false;
        got += chunk;
    }
    return true;
}

bool WriteAll(HANDLE pipe, const void *buf, DWORD n, HANDLE stop, OVERLAPPED &ov)
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
            if (!WaitOverlapped(pipe, ov, stop, chunk)) return false;
        }
        if (chunk == 0) return false;
        sent += chunk;
    }
    return true;
}

// Parse a {topic, data} or {id, result|error} envelope. Sets `topic`,
// `topicLen`, `dataPtr`, `dataLen` for pushes (topic non-null). For
// replies sets `dataPtr` to the start of result/error value (we don't
// care which — this client only ever sends subscribe / unsubscribe, both
// of which return {ok: bool}; failures arrive as a string, success as a
// {ok: true} map, both readable).
struct ParsedFrame
{
    bool        isPush;
    const char *topic;
    uint32_t    topicLen;
    const uint8_t *dataPtr;
    uint32_t    dataLen;
};

ParsedFrame ParseFrame(const uint8_t *bytes, uint32_t len)
{
    ParsedFrame f{};
    nxt::rpc::msgpack::Reader r(bytes, len);
    uint32_t mc;
    if (!r.ReadMapHeader(mc)) return f;
    for (uint32_t i = 0; i < mc; ++i)
    {
        const char *k = nullptr;
        uint32_t kLen = 0;
        if (!r.ReadString(k, kLen)) return f;
        if (nxt::rpc::msgpack::StrEq(k, kLen, "topic"))
        {
            f.isPush = true;
            if (!r.ReadString(f.topic, f.topicLen)) return ParsedFrame{};
        }
        else if (nxt::rpc::msgpack::StrEq(k, kLen, "data"))
        {
            const size_t start = r.Pos();
            if (!r.SkipValue()) return ParsedFrame{};
            f.dataPtr = bytes + start;
            f.dataLen = static_cast<uint32_t>(r.Pos() - start);
        }
        else
        {
            // id / result / error / future keys — skip without recording.
            if (!r.SkipValue()) return ParsedFrame{};
        }
    }
    return f;
}

}

TapClient::TapClient() = default;

TapClient::~TapClient()
{
    Disconnect();
}

bool TapClient::Connect(DWORD pid)
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
            lastStatus_ = TapStatus::Busy;
            std::swprintf(lastErr_, 256,
                          L"Tap: all pipe slots taken (PID %lu)", pid);
        }
        else
        {
            lastStatus_ = TapStatus::Error;
            std::swprintf(lastErr_, 256,
                          L"Tap: CreateFileW(%s) failed: %lu", name, err);
        }
        log::LogWarn(lastErr_);
        return false;
    }

    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(p, &mode, nullptr, nullptr);

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_)
    {
        CloseHandle(p);
        lastStatus_ = TapStatus::Error;
        std::swprintf(lastErr_, 256, L"Tap: CreateEventW failed: %lu",
                      GetLastError());
        return false;
    }

    pipe_       = p;
    pid_        = pid;
    stop_.store(false);
    framesReceived_.store(0);
    lastStatus_ = TapStatus::Connecting;

    reader_ = std::thread([this] { ReaderLoop(); });
    std::swprintf(lastErr_, 256, L"Tap: connected to PID %lu", pid);
    log::LogInfo(lastErr_);
    lastErr_[0] = 0;
    return true;
}

void TapClient::Disconnect()
{
    if (!IsConnected() && !reader_.joinable())
    {
        // Clean state already.
        return;
    }

    stop_.store(true);
    if (stopEvent_) SetEvent(stopEvent_);

    // CancelIoEx ensures any in-flight ReadFile completes promptly so the
    // reader thread observes stop_ and exits.
    if (pipe_ != INVALID_HANDLE_VALUE) CancelIoEx(pipe_, nullptr);

    if (reader_.joinable()) reader_.join();

    if (pipe_ != INVALID_HANDLE_VALUE)
    {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    if (stopEvent_)
    {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }

    {
        std::lock_guard<std::mutex> lk(topicsMx_);
        topics_.clear();
    }
    pid_ = 0;
    lastStatus_ = TapStatus::Disconnected;
}

bool TapClient::SendSubscribeFrame(const char *method, const std::string &topic)
{
    if (pipe_ == INVALID_HANDLE_VALUE) return false;

    // {id, method, params: {topics: [topic]}}
    uint8_t buf[1024];
    nxt::rpc::msgpack::Writer w(buf + 4, sizeof(buf) - 4);
    const int64_t id = nextId_++;
    w.WriteMapHeader(3);
    w.WriteCStr("id");        w.WriteInt(id);
    w.WriteCStr("method");    w.WriteCStr(method);
    w.WriteCStr("params");
        w.WriteMapHeader(1);
        w.WriteCStr("topics");
            w.WriteArrayHeader(1);
            w.WriteString(topic.data(), static_cast<uint32_t>(topic.size()));
    if (w.Overflowed()) return false;
    const uint32_t bodyLen = static_cast<uint32_t>(w.BytesWritten());
    buf[0] = static_cast<uint8_t>(bodyLen      );
    buf[1] = static_cast<uint8_t>(bodyLen >> 8 );
    buf[2] = static_cast<uint8_t>(bodyLen >> 16);
    buf[3] = static_cast<uint8_t>(bodyLen >> 24);

    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    std::lock_guard<std::mutex> lk(writeMx_);
    bool ok = WriteAll(pipe_, buf, 4 + bodyLen, stopEvent_, ov);
    CloseHandle(ov.hEvent);
    return ok;
}

bool TapClient::Subscribe(const std::string &topic, TopicCallback cb)
{
    if (!IsConnected()) return false;

    {
        std::lock_guard<std::mutex> lk(topicsMx_);
        bool replaced = false;
        for (auto &kv : topics_)
        {
            if (kv.first == topic) { kv.second = std::move(cb); replaced = true; break; }
        }
        if (!replaced) topics_.emplace_back(topic, std::move(cb));
    }

    bool sent = SendSubscribeFrame("_debug.subscribe", topic);
    if (sent) lastStatus_ = TapStatus::Subscribed;
    return sent;
}

void TapClient::Unsubscribe(const std::string &topic)
{
    {
        std::lock_guard<std::mutex> lk(topicsMx_);
        for (auto it = topics_.begin(); it != topics_.end(); ++it)
        {
            if (it->first == topic) { topics_.erase(it); break; }
        }
    }
    (void)SendSubscribeFrame("_debug.unsubscribe", topic);
}

void TapClient::DispatchFrame(const uint8_t *frame, uint32_t len)
{
    ParsedFrame pf = ParseFrame(frame, len);
    if (!pf.isPush) return;   // reply frames are subscribe/unsubscribe acks; ignore

    framesReceived_.fetch_add(1, std::memory_order_relaxed);

    TopicCallback cb;
    {
        std::lock_guard<std::mutex> lk(topicsMx_);
        for (auto &kv : topics_)
        {
            if (kv.first.size() == pf.topicLen &&
                std::memcmp(kv.first.data(), pf.topic, pf.topicLen) == 0)
            {
                cb = kv.second;
                break;
            }
        }
    }
    if (cb) cb(pf.dataPtr, pf.dataLen);
}

void TapClient::ReaderLoop()
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return;

    std::vector<uint8_t> body;

    while (!stop_.load())
    {
        uint32_t len = 0;
        if (!ReadExact(pipe_, &len, 4, stopEvent_, ov)) break;
        if (len == 0 || len > kMaxFrameBytes)
        {
            log::LogWarn(L"Tap: bad frame length, disconnecting");
            break;
        }
        body.resize(len);
        if (!ReadExact(pipe_, body.data(), len, stopEvent_, ov)) break;
        DispatchFrame(body.data(), len);
    }

    CloseHandle(ov.hEvent);
}

}
