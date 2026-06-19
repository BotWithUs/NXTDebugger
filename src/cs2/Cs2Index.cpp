#include "Cs2Index.h"

#include <Windows.h>

#include <cstring>
#include <fstream>

namespace nxtdbg::cs2
{

namespace
{

constexpr uint32_t kCacheMagic   = 0x52583243u;   // "C2XR"
constexpr uint32_t kCacheVersion = 1u;

using RefMap = std::unordered_map<int, std::vector<int>>;

struct AsmFile
{
    std::wstring path;
    int          scriptId;
};

// exe at <root>/NXTDebugger/build/<Config>/nxt_debugger.exe → strip the file
// name plus three directory levels to reach <root>, then the corpus path.
std::wstring DefaultAsmDir()
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        return {};
    }
    std::wstring p(exe, n);
    for (int i = 0; i < 4; ++i)
    {
        size_t slash = p.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
        {
            return {};
        }
        p.resize(slash);
    }
    return p + L"\\GameVal Information\\cs2_asm";
}

int ScriptIdFromName(const wchar_t *name)
{
    int  v   = 0;
    bool any = false;
    for (const wchar_t *p = name; *p && *p != L'.'; ++p)
    {
        if (*p >= L'0' && *p <= L'9')
        {
            v = v * 10 + static_cast<int>(*p - L'0');
            any = true;
        }
        else
        {
            break;
        }
    }
    return any ? v : -1;
}

bool ReadWholeFile(const std::wstring &path, std::string &out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        return false;
    }
    f.seekg(0, std::ios::end);
    std::streamoff len = f.tellg();
    if (len <= 0)
    {
        out.clear();
        return true;
    }
    f.seekg(0);
    out.resize(static_cast<size_t>(len));
    f.read(out.data(), len);
    return true;
}

void AddRef(RefMap &idx, int id, int scriptId)
{
    std::vector<int> &vec = idx[id];
    if (vec.empty() || vec.back() != scriptId)   // refs from one script are contiguous
    {
        vec.push_back(scriptId);
    }
}

bool ParseIntAt(const std::string &t, size_t p, int &out)
{
    int sign = 1;
    if (p < t.size() && t[p] == '-')
    {
        sign = -1;
        ++p;
    }
    long long val = 0;
    bool      any = false;
    while (p < t.size() && t[p] >= '0' && t[p] <= '9')
    {
        val = val * 10 + (t[p] - '0');
        if (val > 0x7fffffffLL)   // clamp: no config id exceeds INT_MAX, avoid overflow
        {
            val = 0x7fffffffLL;
        }
        ++p;
        any = true;
    }
    if (!any)
    {
        return false;
    }
    out = static_cast<int>(sign * val);
    return true;
}

void ScanAnchor(const std::string &t, const char *anchor, int scriptId, RefMap &idx)
{
    size_t alen = std::strlen(anchor);
    size_t pos  = 0;
    while ((pos = t.find(anchor, pos)) != std::string::npos)
    {
        int id = 0;
        if (ParseIntAt(t, pos + alen, id))
        {
            AddRef(idx, id, scriptId);
        }
        pos += alen;
    }
}

// varp(type=T, id=N, tail=0) — the id is not adjacent to the opcode name, so
// hop from each "varp(" to the "id=" before its closing paren.
void ScanVarp(const std::string &t, int scriptId, RefMap &idx)
{
    size_t pos = 0;
    while ((pos = t.find("varp(", pos)) != std::string::npos)
    {
        size_t idp   = t.find("id=", pos);
        size_t close = t.find(')', pos);
        if (idp != std::string::npos && (close == std::string::npos || idp < close))
        {
            int id = 0;
            if (ParseIntAt(t, idp + 3, id))
            {
                AddRef(idx, id, scriptId);
            }
        }
        pos += 5;
    }
}

bool Enumerate(const std::wstring &dir, std::vector<AsmFile> &out, uint64_t &totalBytes)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.asm").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            continue;
        }
        int sid = ScriptIdFromName(fd.cFileName);
        if (sid < 0)
        {
            continue;
        }
        totalBytes += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        out.push_back({ dir + L"\\" + fd.cFileName, sid });
    }
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

void ParseAll(const std::vector<AsmFile> &files, std::atomic<size_t> &filesDone,
              std::atomic<bool> &cancel, RefMap &vb, RefMap &vp, RefMap &cn)
{
    std::string text;
    size_t      done = 0;
    for (const auto &f : files)
    {
        if (cancel.load(std::memory_order_relaxed))
        {
            return;
        }
        if (ReadWholeFile(f.path, text))
        {
            ScanAnchor(text, "varbit(id=", f.scriptId, vb);
            ScanVarp(text, f.scriptId, vp);
            ScanAnchor(text, "const(type=0, int=", f.scriptId, cn);
        }
        filesDone.store(++done, std::memory_order_relaxed);
    }
}

std::wstring CachePath()
{
    wchar_t local[MAX_PATH];
    DWORD   n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(local) : L".";
    base += L"\\NXTDebugger";
    CreateDirectoryW(base.c_str(), nullptr);
    return base + L"\\cs2_xref.idx";
}

void WriteMap(std::ofstream &f, const RefMap &m)
{
    uint64_t n = m.size();
    f.write(reinterpret_cast<const char *>(&n), sizeof(n));
    for (const auto &kv : m)
    {
        int32_t  key = kv.first;
        uint64_t cnt = kv.second.size();
        f.write(reinterpret_cast<const char *>(&key), sizeof(key));
        f.write(reinterpret_cast<const char *>(&cnt), sizeof(cnt));
        f.write(reinterpret_cast<const char *>(kv.second.data()),
                static_cast<std::streamsize>(cnt * sizeof(int)));
    }
}

bool ReadMap(std::ifstream &f, RefMap &m)
{
    uint64_t n = 0;
    if (!f.read(reinterpret_cast<char *>(&n), sizeof(n)))
    {
        return false;
    }
    for (uint64_t i = 0; i < n; ++i)
    {
        int32_t  key = 0;
        uint64_t cnt = 0;
        if (!f.read(reinterpret_cast<char *>(&key), sizeof(key))
            || !f.read(reinterpret_cast<char *>(&cnt), sizeof(cnt)))
        {
            return false;
        }
        std::vector<int> v(static_cast<size_t>(cnt));
        if (cnt && !f.read(reinterpret_cast<char *>(v.data()),
                           static_cast<std::streamsize>(cnt * sizeof(int))))
        {
            return false;
        }
        m.emplace(key, std::move(v));
    }
    return true;
}

}

Cs2Index::Cs2Index()
{
    dir_ = DefaultAsmDir();
}

Cs2Index::~Cs2Index()
{
    cancel_.store(true, std::memory_order_relaxed);
    if (thread_.joinable())
    {
        thread_.join();
    }
}

void Cs2Index::BuildAsync(bool force)
{
    int st = status_.load(std::memory_order_acquire);
    if (st == static_cast<int>(Status::Building))
    {
        return;
    }
    if (st == static_cast<int>(Status::Ready) && !force)
    {
        return;
    }
    if (thread_.joinable())
    {
        thread_.join();   // a prior Ready/Error/Idle run
    }
    cancel_.store(false, std::memory_order_relaxed);
    filesDone_.store(0, std::memory_order_relaxed);
    filesTotal_.store(0, std::memory_order_relaxed);
    error_.clear();
    status_.store(static_cast<int>(Status::Building), std::memory_order_release);
    thread_ = std::thread([this, force] { Worker(force); });
}

void Cs2Index::Worker(bool force)
{
    std::vector<AsmFile> files;
    uint64_t             totalBytes = 0;
    if (!Enumerate(dir_, files, totalBytes) || files.empty())
    {
        error_ = L"No *.asm files under: " + dir_;
        status_.store(static_cast<int>(Status::Error), std::memory_order_release);
        return;
    }
    filesTotal_.store(files.size(), std::memory_order_relaxed);

    if (!force && LoadCache(files.size(), totalBytes))
    {
        scriptCount_.store(files.size(), std::memory_order_relaxed);
        filesDone_.store(files.size(), std::memory_order_relaxed);
        status_.store(static_cast<int>(Status::Ready), std::memory_order_release);
        return;
    }

    varbit_.clear();
    varp_.clear();
    const_.clear();
    ParseAll(files, filesDone_, cancel_, varbit_, varp_, const_);
    if (cancel_.load(std::memory_order_relaxed))
    {
        return;
    }
    scriptCount_ = files.size();
    SaveCache(files.size(), totalBytes);
    status_.store(static_cast<int>(Status::Ready), std::memory_order_release);
}

bool Cs2Index::LoadCache(uint64_t fileCount, uint64_t totalBytes)
{
    std::ifstream f(CachePath(), std::ios::binary);
    if (!f)
    {
        return false;
    }
    uint32_t magic = 0, ver = 0;
    uint64_t fc = 0, tb = 0;
    if (!f.read(reinterpret_cast<char *>(&magic), sizeof(magic))
        || !f.read(reinterpret_cast<char *>(&ver), sizeof(ver))
        || !f.read(reinterpret_cast<char *>(&fc), sizeof(fc))
        || !f.read(reinterpret_cast<char *>(&tb), sizeof(tb)))
    {
        return false;
    }
    if (magic != kCacheMagic || ver != kCacheVersion || fc != fileCount || tb != totalBytes)
    {
        return false;
    }
    varbit_.clear();
    varp_.clear();
    const_.clear();
    return ReadMap(f, varbit_) && ReadMap(f, varp_) && ReadMap(f, const_);
}

void Cs2Index::SaveCache(uint64_t fileCount, uint64_t totalBytes) const
{
    std::ofstream f(CachePath(), std::ios::binary | std::ios::trunc);
    if (!f)
    {
        return;
    }
    f.write(reinterpret_cast<const char *>(&kCacheMagic), sizeof(kCacheMagic));
    f.write(reinterpret_cast<const char *>(&kCacheVersion), sizeof(kCacheVersion));
    f.write(reinterpret_cast<const char *>(&fileCount), sizeof(fileCount));
    f.write(reinterpret_cast<const char *>(&totalBytes), sizeof(totalBytes));
    WriteMap(f, varbit_);
    WriteMap(f, varp_);
    WriteMap(f, const_);
}

const std::vector<int> *Cs2Index::Varbit(int id) const
{
    if (GetStatus() != Status::Ready)
    {
        return nullptr;
    }
    auto it = varbit_.find(id);
    return it == varbit_.end() ? nullptr : &it->second;
}

const std::vector<int> *Cs2Index::Varp(int id) const
{
    if (GetStatus() != Status::Ready)
    {
        return nullptr;
    }
    auto it = varp_.find(id);
    return it == varp_.end() ? nullptr : &it->second;
}

const std::vector<int> *Cs2Index::Const(int id) const
{
    if (GetStatus() != Status::Ready)
    {
        return nullptr;
    }
    auto it = const_.find(id);
    return it == const_.end() ? nullptr : &it->second;
}

}
