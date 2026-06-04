#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <string>

namespace nxtdbg::cache
{

// Wraps the NXTCache.dll C ABI via LoadLibraryW + GetProcAddress so panels
// can resolve id -> JSON without a link-time dep on NXTCache.lib.
class CacheClient
{
public:
    CacheClient() = default;
    CacheClient(const CacheClient &)            = delete;
    CacheClient &operator=(const CacheClient &) = delete;
    ~CacheClient();

    // Loads NXTCache.dll (searches the exe directory + PATH) and opens the
    // local cache at `cacheDir`. Returns true on success; LastError() carries
    // a wide message on failure.
    bool Open(const std::wstring &cacheDir);
    void Close();

    bool IsOpen() const { return cacheHandle_ != nullptr; }
    const wchar_t *LastError() const { return lastErr_; }

    // Resolves an id to a JSON string. typeName is one of: "npc", "item",
    // "loc", "seq", "varbit", "enum", "struct", "inv", "param", "quest".
    // Returns empty string on miss / not loaded.
    std::string GetJson(const char *typeName, int id);

private:
    HMODULE  dll_         = nullptr;
    void    *cacheHandle_ = nullptr;
    wchar_t  lastErr_[256]{};

    // Bound function pointers.
    using OpenLocalFn = void *(*)(const char *);
    using CloseFn     = void  (*)(void *);
    using GetFn       = int   (*)(void *, const char *, int, char **, size_t *);
    using FreeFn      = void  (*)(void *);
    using LastErrFn   = const char *(*)(void);

    OpenLocalFn openLocal_ = nullptr;
    CloseFn     close_     = nullptr;
    GetFn       get_       = nullptr;
    FreeFn      free_      = nullptr;
    LastErrFn   lastErrC_  = nullptr;

    bool Bind();
};

}
