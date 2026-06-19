#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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

    // Cheap id enumeration straight from the index reference tables (no decode
    // / blob fetch). typeName accepts the same names as GetJson plus "if".
    // Returns an ascending id list, or empty on miss / DLL too old to export
    // nxt_list_type_ids.
    std::vector<int> ListTypeIds(const char *typeName);

    // Software-renders an item's inventory icon to RGBA8888 (R,G,B,A row-major,
    // top-left origin; size == width*height*4). supersample is the SSAA factor
    // (0 picks the library default of 4). Returns false on miss / no model /
    // DLL too old. outRgba is cleared on failure.
    bool RenderItemIcon(int id, int width, int height, int supersample,
                        std::vector<uint8_t> &outRgba);

    // Decodes one frame of a sprite group to RGBA8888. outW/outH receive the
    // frame dimensions. Returns false on miss / out-of-range frame / old DLL.
    bool GetSpriteFrame(int id, int frameIndex, std::vector<uint8_t> &outRgba,
                        int &outWidth, int &outHeight);

private:
    HMODULE  dll_         = nullptr;
    void    *cacheHandle_ = nullptr;
    wchar_t  lastErr_[256]{};

    // Bound function pointers.
    using OpenLocalFn  = void *(*)(const char *);
    using CloseFn      = void  (*)(void *);
    using GetFn        = int   (*)(void *, const char *, int, char **, size_t *);
    using FreeFn       = void  (*)(void *);
    using LastErrFn    = const char *(*)(void);
    using ListIdsFn    = int   (*)(void *, const char *, int **, size_t *);
    using RenderIconFn = int   (*)(void *, int, int, int, int,
                                   uint8_t **, size_t *);
    using SpriteRgbaFn = int   (*)(void *, int, int, uint8_t **, size_t *,
                                   int32_t *, int32_t *, int32_t *, int32_t *);

    OpenLocalFn  openLocal_  = nullptr;
    CloseFn      close_      = nullptr;
    GetFn        get_        = nullptr;
    FreeFn       free_       = nullptr;
    LastErrFn    lastErrC_   = nullptr;
    ListIdsFn    listIds_    = nullptr;
    RenderIconFn renderIcon_ = nullptr;
    SpriteRgbaFn spriteRgba_ = nullptr;

    bool Bind();
};

}
