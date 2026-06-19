#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace nxtdbg::cs2
{

// Inverted index over a directory of CS2 disassembly (*.asm) — the GameVal
// cs2_asm corpus. Maps a config id to the script ids that reference it. The
// build runs on a background thread (it touches only files, never the cache
// handle, so it can't collide with the UI thread's NXTCache calls).
//
// Varbit/varp refs are EXACT (typed `cs2_push_varbit` / `cs2_push_var`
// operands). The const index is the AMBIGUOUS integer-constant union — item N,
// npc N and sprite N are indistinguishable as `const(type=0, int=N)` operands —
// so it drives item/npc/loc/struct/enum/param/inv/seq xref as a superset.
class Cs2Index
{
public:
    enum class Status
    {
        Idle,
        Building,
        Ready,
        Error,
    };

    Cs2Index();
    Cs2Index(const Cs2Index &)            = delete;
    Cs2Index &operator=(const Cs2Index &) = delete;
    ~Cs2Index();

    Status GetStatus() const
    {
        return static_cast<Status>(status_.load(std::memory_order_acquire));
    }
    size_t FilesDone()  const { return filesDone_.load(std::memory_order_relaxed); }
    size_t FilesTotal() const { return filesTotal_.load(std::memory_order_relaxed); }
    size_t ScriptCount() const { return scriptCount_.load(std::memory_order_relaxed); }

    const std::wstring &Dir() const { return dir_; }
    void SetDir(const std::wstring &dir) { dir_ = dir; }
    const std::wstring &LastError() const { return error_; }

    // Spawn the build worker. No-op while Building or already Ready (unless
    // force, which rebuilds from scratch ignoring the on-disk cache).
    void BuildAsync(bool force);

    // Queries — valid only when GetStatus() == Ready. nullptr if unreferenced.
    const std::vector<int> *Varbit(int id) const;
    const std::vector<int> *Varp(int id) const;
    const std::vector<int> *Const(int id) const;

private:
    void Worker(bool force);
    bool LoadCache(uint64_t fileCount, uint64_t totalBytes);
    void SaveCache(uint64_t fileCount, uint64_t totalBytes) const;

    std::thread         thread_;
    std::atomic<int>    status_{0};
    std::atomic<size_t> filesDone_{0};
    std::atomic<size_t> filesTotal_{0};
    std::atomic<size_t> scriptCount_{0};
    std::atomic<bool>   cancel_{false};

    std::wstring dir_;
    std::wstring error_;

    std::unordered_map<int, std::vector<int>> varbit_;
    std::unordered_map<int, std::vector<int>> varp_;
    std::unordered_map<int, std::vector<int>> const_;
};

}
