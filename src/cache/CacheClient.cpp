#include "CacheClient.h"

#include "log/Log.h"

#include <cstdio>

namespace nxtdbg::cache
{

namespace
{

std::string WideToUtf8(const std::wstring &w)
{
    if (w.empty())
    {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

}

CacheClient::~CacheClient()
{
    Close();
}

bool CacheClient::Bind()
{
    openLocal_ = reinterpret_cast<OpenLocalFn>(GetProcAddress(dll_, "nxt_cache_open_local"));
    close_     = reinterpret_cast<CloseFn>    (GetProcAddress(dll_, "nxt_cache_close"));
    get_       = reinterpret_cast<GetFn>      (GetProcAddress(dll_, "nxt_get_json"));
    free_      = reinterpret_cast<FreeFn>     (GetProcAddress(dll_, "nxt_free"));
    lastErrC_  = reinterpret_cast<LastErrFn>  (GetProcAddress(dll_, "nxt_last_error"));
    if (!openLocal_ || !close_ || !get_ || !free_)
    {
        swprintf_s(lastErr_, L"NXTCache.dll missing required exports");
        return false;
    }
    return true;
}

bool CacheClient::Open(const std::wstring &cacheDir)
{
    Close();

    dll_ = LoadLibraryW(L"NXTCache.dll");
    if (!dll_)
    {
        swprintf_s(lastErr_, L"LoadLibraryW(NXTCache.dll) failed: %lu (place it next to nxt_debugger.exe)",
                   GetLastError());
        log::LogWarn(lastErr_);
        return false;
    }
    if (!Bind())
    {
        log::LogError(lastErr_);
        FreeLibrary(dll_);
        dll_ = nullptr;
        return false;
    }

    std::string utf8 = WideToUtf8(cacheDir);
    cacheHandle_ = openLocal_(utf8.c_str());
    if (!cacheHandle_)
    {
        const char *err = lastErrC_ ? lastErrC_() : "unknown";
        swprintf_s(lastErr_, L"nxt_cache_open_local failed: %hs", err);
        log::LogError(lastErr_);
        FreeLibrary(dll_);
        dll_ = nullptr;
        return false;
    }
    swprintf_s(lastErr_, L"Opened cache at %s", cacheDir.c_str());
    log::LogInfo(lastErr_);
    lastErr_[0] = 0;
    return true;
}

void CacheClient::Close()
{
    if (cacheHandle_ && close_)
    {
        close_(cacheHandle_);
    }
    cacheHandle_ = nullptr;
    if (dll_)
    {
        FreeLibrary(dll_);
        dll_ = nullptr;
    }
    openLocal_ = nullptr;
    close_     = nullptr;
    get_       = nullptr;
    free_      = nullptr;
    lastErrC_  = nullptr;
}

std::string CacheClient::GetJson(const char *typeName, int id)
{
    if (!IsOpen() || !get_)
    {
        return {};
    }
    char  *jsonRaw = nullptr;
    size_t len     = 0;
    int    rc      = get_(cacheHandle_, typeName, id, &jsonRaw, &len);
    if (rc != 0 || !jsonRaw)
    {
        return {};
    }
    std::string out(jsonRaw, len);
    free_(jsonRaw);
    return out;
}

}
