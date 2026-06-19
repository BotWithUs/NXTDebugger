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
    // Optional exports — newer DLLs only. Absence degrades the browser (no id
    // list / no icons) but never blocks attach.
    listIds_    = reinterpret_cast<ListIdsFn>   (GetProcAddress(dll_, "nxt_list_type_ids"));
    renderIcon_ = reinterpret_cast<RenderIconFn>(GetProcAddress(dll_, "nxt_render_item_icon"));
    spriteRgba_ = reinterpret_cast<SpriteRgbaFn>(GetProcAddress(dll_, "nxt_get_sprite_frame_rgba"));
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
    openLocal_  = nullptr;
    close_      = nullptr;
    get_        = nullptr;
    free_       = nullptr;
    lastErrC_   = nullptr;
    listIds_    = nullptr;
    renderIcon_ = nullptr;
    spriteRgba_ = nullptr;
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

std::vector<int> CacheClient::ListTypeIds(const char *typeName)
{
    if (!IsOpen() || !listIds_)
    {
        return {};
    }
    int   *ids   = nullptr;
    size_t count = 0;
    int    rc    = listIds_(cacheHandle_, typeName, &ids, &count);
    if (rc != 0 || !ids)
    {
        return {};
    }
    std::vector<int> out(ids, ids + count);
    free_(ids);
    return out;
}

bool CacheClient::RenderItemIcon(int id, int width, int height, int supersample,
                                 std::vector<uint8_t> &outRgba)
{
    outRgba.clear();
    if (!IsOpen() || !renderIcon_)
    {
        return false;
    }
    uint8_t *rgba  = nullptr;
    size_t   count = 0;
    int      rc    = renderIcon_(cacheHandle_, id, width, height, supersample,
                                 &rgba, &count);
    if (rc != 0 || !rgba)
    {
        return false;
    }
    outRgba.assign(rgba, rgba + count);
    free_(rgba);
    return true;
}

bool CacheClient::GetSpriteFrame(int id, int frameIndex,
                                 std::vector<uint8_t> &outRgba,
                                 int &outWidth, int &outHeight)
{
    outRgba.clear();
    outWidth  = 0;
    outHeight = 0;
    if (!IsOpen() || !spriteRgba_)
    {
        return false;
    }
    uint8_t *rgba  = nullptr;
    size_t   count = 0;
    int32_t  w = 0, h = 0, ox = 0, oy = 0;
    int      rc = spriteRgba_(cacheHandle_, id, frameIndex, &rgba, &count,
                              &w, &h, &ox, &oy);
    if (rc != 0 || !rgba)
    {
        return false;
    }
    outRgba.assign(rgba, rgba + count);
    free_(rgba);
    outWidth  = w;
    outHeight = h;
    return true;
}

}
