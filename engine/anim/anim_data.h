#pragma once

// Read-only view of the .fpak Animation section (anim_format.h), used in place.

#include "anim/anim_format.h"

#include <cstdint>
#include <cstring>

namespace fx::anim {

class AnimData {
public:
    // `section` may be null/empty (scene without skeletons).
    bool init(const void* section, size_t size)
    {
        base_ = static_cast<const uint8_t*>(section);
        if (!base_ || size < sizeof(fmt::Header)) {
            base_ = nullptr;
            return true;
        }
        const auto* h = header();
        return std::memcmp(h->magic, fmt::kMagic, 4) == 0 && h->version == fmt::kVersion;
    }

    uint32_t skeletonCount() const { return base_ ? header()->skeletonCount : 0; }
    uint32_t clipCount() const { return base_ ? header()->clipCount : 0; }
    uint32_t graphCount() const { return base_ ? header()->graphCount : 0; }
    const fmt::Graph& graph(uint32_t i) const { return at<fmt::Graph>(header()->graphs)[i]; }
    const fmt::Skeleton& skeleton(uint32_t i) const { return at<fmt::Skeleton>(header()->skeletons)[i]; }
    const fmt::Bone* bones(const fmt::Skeleton& s) const { return at<fmt::Bone>(s.bones); }
    const fmt::Clip& clip(uint32_t i) const { return at<fmt::Clip>(header()->clips)[i]; }
    const char* name(uint32_t offset) const { return at<char>(header()->strings) + offset; }

    template <typename T>
    const T* at(uint32_t offset) const { return reinterpret_cast<const T*>(base_ + offset); }

    // Index of the clip with this name (or -1).
    int32_t findClip(const char* clipName) const
    {
        for (uint32_t i = 0; i < clipCount(); ++i)
            if (std::strcmp(name(clip(i).name), clipName) == 0)
                return int32_t(i);
        return -1;
    }

private:
    const fmt::Header* header() const { return reinterpret_cast<const fmt::Header*>(base_); }
    const uint8_t* base_ = nullptr;
};

}  // namespace fx::anim
