#pragma once
#include <cstdint>
#include <vector>

namespace nxtdbg::render
{

// Uploads an RGBA8888 buffer (size == width*height*4, top-left origin) to a new
// GL_TEXTURE_2D and returns its name, or 0 on failure. Must run on the thread
// that owns the GL context (the frame loop / panel Draw). Caller owns the
// texture and frees it with DeleteTexture. Cast the result to ImTextureID for
// ImGui::Image.
unsigned int UploadRgba(const uint8_t *rgba, int width, int height);

// Deletes a texture from UploadRgba. Safe to pass 0.
void DeleteTexture(unsigned int tex);

// Small LRU texture cache keyed by an integer id (e.g. item id), so re-visiting
// an entry doesn't re-upload. GL-context-thread only.
class TextureCache
{
public:
    TextureCache() = default;
    TextureCache(const TextureCache &)            = delete;
    TextureCache &operator=(const TextureCache &) = delete;
    ~TextureCache();

    // Cached texture for `id`, or 0 if absent. Touches LRU recency on a hit.
    unsigned int Get(int id);
    bool         Contains(int id) const;

    // Uploads an RGBA buffer for `id` (replacing any existing entry) and returns
    // the texture, or 0 on failure. Evicts the least-recently-used past the cap.
    unsigned int Put(int id, const uint8_t *rgba, int width, int height);

    void Clear();

private:
    struct Entry
    {
        int          id;
        unsigned int tex;
        uint64_t     used;
    };

    void EvictLru();

    std::vector<Entry> entries_;
    uint64_t           tick_ = 0;

    static constexpr size_t kCap = 256;
};

}
