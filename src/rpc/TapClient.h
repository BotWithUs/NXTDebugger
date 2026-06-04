#pragma once
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nxtdbg::rpc
{

// Outcome of the most recent connect / subscribe attempt. Mirrors the shape
// of RpcClient::CallStatus closely so the UI can render both with the same
// status pill.
enum class TapStatus : uint8_t
{
    Disconnected,    // not connected to a pipe
    Connecting,      // pipe open + subscription in flight
    Subscribed,      // happy path: pipe + reader thread + at least one topic
    Busy,            // CreateFileW returned ERROR_PIPE_BUSY
    Error,           // see LastError() for a wide message
};

// Topic callback shape — fired on the reader thread, NOT the UI thread.
// `data` points into a frame buffer owned by the reader; copy if the
// callback needs to retain it past the call.
using TopicCallback = std::function<void(const uint8_t *data, uint32_t dataLen)>;

// A pipe client tuned for the push side of the broker. Distinct from
// RpcClient (which is the sync request-response one the console uses) so
// adding a reader thread + per-request demux didn't have to touch the
// working console code mid-Phase-3.
//
// One TapClient owns:
//   - one pipe HANDLE
//   - one background reader thread (started on Connect, joined on Disconnect)
//   - a topic → callback registry, locked by a mutex
//   - a writer mutex for outgoing subscribe / unsubscribe frames
//
// The reader thread does overlapped ReadFile, demuxes each frame by which
// keys are present in the msgpack map:
//   - `topic` + `data` → push: look up callback, invoke with raw data bytes
//   - `id` + (`result`|`error`) → reply: only used by Subscribe / Unsubscribe
//     (everything else flows over the separate RpcClient)
class TapClient
{
public:
    TapClient();
    TapClient(const TapClient &)            = delete;
    TapClient &operator=(const TapClient &) = delete;
    ~TapClient();

    // Open the pipe + start the reader thread. Returns true on success.
    // On false, call LastStatus() to distinguish Busy / Error.
    bool Connect(DWORD pid);

    // Cancel the reader thread, close the pipe, drop all subscriptions.
    // Safe to call from anywhere (including the destructor and the UI
    // thread). Idempotent.
    void Disconnect();

    bool      IsConnected() const noexcept { return pipe_ != INVALID_HANDLE_VALUE; }
    DWORD     Pid()         const noexcept { return pid_; }
    TapStatus LastStatus()  const noexcept { return lastStatus_; }
    const wchar_t *LastError() const noexcept { return lastErr_; }

    // Send {_debug.subscribe, topics: [topic]} to the agent and register
    // `cb` for inbound frames on that topic. The callback fires on the
    // reader thread. Subscribing the same topic twice replaces the prior
    // callback (and re-sends the subscribe frame, which the agent treats
    // as idempotent). Returns false on send failure or no connection.
    bool Subscribe(const std::string &topic, TopicCallback cb);

    // Send {_debug.unsubscribe, topics: [topic]} and drop the callback.
    // Best-effort — connection drops are tolerated.
    void Unsubscribe(const std::string &topic);

    // Diagnostic counter — number of push frames received since Connect.
    // Read across thread boundaries via atomic load. Used by the tap
    // panel's status row.
    uint64_t FramesReceived() const noexcept { return framesReceived_.load(); }

private:
    void ReaderLoop();
    void DispatchFrame(const uint8_t *frame, uint32_t len);
    bool SendSubscribeFrame(const char *method, const std::string &topic);

    HANDLE                                pipe_       = INVALID_HANDLE_VALUE;
    HANDLE                                stopEvent_  = nullptr;
    DWORD                                 pid_        = 0;
    TapStatus                             lastStatus_ = TapStatus::Disconnected;
    wchar_t                               lastErr_[256]{};

    std::thread                           reader_;
    std::atomic<bool>                     stop_{false};
    std::atomic<uint64_t>                 framesReceived_{0};

    std::mutex                            writeMx_;
    int64_t                               nextId_     = 1;

    std::mutex                            topicsMx_;
    std::vector<std::pair<std::string, TopicCallback>> topics_;
};

}
