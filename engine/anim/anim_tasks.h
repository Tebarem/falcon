#pragma once

// Pose task system, ported from Esoterica (MIT, Engine/Animation/TaskSystem). Graph nodes
// register tasks while they update; afterwards the list runs forward once. Each task has at
// most two dependencies (always earlier in the list) and writes one pooled pose buffer, which
// a consumer either takes over or reads and releases. The last task's pose is the result.

#include "anim/anim_core.h"

#include <cstdint>
#include <vector>

namespace fx::anim {

class TaskSystem {
public:
    static constexpr int8_t kInvalid = -1;

    void init(const Skeleton* skeleton);
    void reset();  // start of a graph evaluation

    int8_t reference();
    int8_t zero();
    int8_t sample(const Clip* clip, float percent);
    int8_t blend(int8_t source, int8_t target, float weight, BlendMode mode = BlendMode::Normal, int16_t mask = -1);
    // A bone mask for a blend of this evaluation (weights per bone, copied): its index.
    int16_t mask(const float* weights);

    // Instant transitions discard the source's tasks.
    int8_t marker() const { return int8_t(tasks_.size()); }
    void rollback(int8_t marker) { tasks_.resize(size_t(marker)); }

    // Runs the list; returns the final (parent-space, non-additive) pose: `result`'s (default: the
    // last task's), or the reference pose when nothing was registered.
    const Pose& execute(int8_t result = kInvalid);

private:
    enum class Type : uint8_t { Reference, Zero, Sample, Blend };
    struct Task {
        Type type;
        BlendMode mode = BlendMode::Normal;
        int8_t deps[2] = {kInvalid, kInvalid};
        const Clip* clip = nullptr;
        float value = 0.0f;  // sample time / blend weight
        int8_t buffer = kInvalid;
        int16_t mask = -1;   // blend: per-bone weights (masks_)
    };

    int8_t add(const Task& t);
    int8_t acquire();
    void release(int8_t buffer) { free_[size_t(buffer)] = true; }

    const Skeleton* skeleton_ = nullptr;
    std::vector<Task> tasks_;
    std::vector<std::vector<float>> masks_;
    size_t masksUsed_ = 0;
    std::vector<Pose> buffers_;
    std::vector<bool> free_;
    Pose result_;
};

}  // namespace fx::anim
