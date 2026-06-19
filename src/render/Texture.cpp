#include "Texture.h"

#include <Windows.h>
#include <gl/GL.h>

// GL_CLAMP_TO_EDGE is a GL 1.2 enum absent from the Windows <gl/GL.h> (which
// only declares GL 1.1), but the runtime 3.3-core driver honours it. Define the
// literal so we get edge clamping rather than 1.1's border-bleeding GL_CLAMP.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace nxtdbg::render
{

unsigned int UploadRgba(const uint8_t *rgba, int width, int height)
{
    if (!rgba || width <= 0 || height <= 0)
    {
        return 0;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (tex == 0)
    {
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

void DeleteTexture(unsigned int tex)
{
    if (tex != 0)
    {
        GLuint t = tex;
        glDeleteTextures(1, &t);
    }
}

TextureCache::~TextureCache()
{
    Clear();
}

unsigned int TextureCache::Get(int id)
{
    for (auto &e : entries_)
    {
        if (e.id == id)
        {
            e.used = ++tick_;
            return e.tex;
        }
    }
    return 0;
}

bool TextureCache::Contains(int id) const
{
    for (const auto &e : entries_)
    {
        if (e.id == id)
        {
            return true;
        }
    }
    return false;
}

unsigned int TextureCache::Put(int id, const uint8_t *rgba, int width, int height)
{
    unsigned int tex = UploadRgba(rgba, width, height);
    if (tex == 0)
    {
        return 0;
    }
    for (auto &e : entries_)
    {
        if (e.id == id)
        {
            DeleteTexture(e.tex);
            e.tex  = tex;
            e.used = ++tick_;
            return tex;
        }
    }
    if (entries_.size() >= kCap)
    {
        EvictLru();
    }
    entries_.push_back({ id, tex, ++tick_ });
    return tex;
}

void TextureCache::Clear()
{
    for (auto &e : entries_)
    {
        DeleteTexture(e.tex);
    }
    entries_.clear();
}

void TextureCache::EvictLru()
{
    if (entries_.empty())
    {
        return;
    }
    size_t lru = 0;
    for (size_t i = 1; i < entries_.size(); ++i)
    {
        if (entries_[i].used < entries_[lru].used)
        {
            lru = i;
        }
    }
    DeleteTexture(entries_[lru].tex);
    entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(lru));
}

}
