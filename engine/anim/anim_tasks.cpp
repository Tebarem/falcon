// Ported from Esoterica (MIT): TaskSystem, TaskPosePool and the default/sample/blend tasks.

#include "anim/anim_tasks.h"

namespace fx::anim {

void TaskSystem::init(const Skeleton* skeleton)
{
    skeleton_ = skeleton;
    result_.reset(skeleton);
    tasks_.reserve(32);
}

void TaskSystem::reset()
{
    tasks_.clear();
    masksUsed_ = 0;
    std::fill(free_.begin(), free_.end(), true);
}

int8_t TaskSystem::add(const Task& t)
{
    if (tasks_.size() >= 127)
        return kInvalid;  // int8 task indices, as in Esoterica
    tasks_.push_back(t);
    return int8_t(tasks_.size() - 1);
}

int8_t TaskSystem::reference() { return add({Type::Reference}); }
int8_t TaskSystem::zero() { return add({Type::Zero}); }

int8_t TaskSystem::sample(const Clip* clip, float percent)
{
    Task t{Type::Sample};
    t.clip = clip;
    t.value = percent;
    return add(t);
}

int8_t TaskSystem::blend(int8_t source, int8_t target, float weight, BlendMode mode, int16_t mask)
{
    if (source == kInvalid || target == kInvalid)
        return source == kInvalid ? target : source;
    Task t{Type::Blend, mode, {source, target}};
    t.value = weight;
    t.mask = mask;
    return add(t);
}

int16_t TaskSystem::mask(const float* weights)
{
    if (!weights || !skeleton_ || masksUsed_ >= 32767)
        return -1;
    if (masks_.size() <= masksUsed_)
        masks_.emplace_back();
    masks_[masksUsed_].assign(weights, weights + skeleton_->boneCount());
    return int16_t(masksUsed_++);
}

int8_t TaskSystem::acquire()
{
    for (size_t i = 0; i < free_.size(); ++i)
        if (free_[i]) {
            free_[i] = false;
            return int8_t(i);
        }
    buffers_.emplace_back(skeleton_);
    free_.push_back(false);
    return int8_t(buffers_.size() - 1);
}

const Pose& TaskSystem::execute(int8_t result)
{
    if (tasks_.empty()) {
        result_.setToReference();
        return result_;
    }
    for (Task& t : tasks_) {
        switch (t.type) {
        case Type::Reference:
            t.buffer = acquire();
            buffers_[t.buffer].setToReference();
            break;
        case Type::Zero:
            t.buffer = acquire();
            buffers_[t.buffer].setToZero();
            break;
        case Type::Sample:
            t.buffer = acquire();
            t.clip->samplePose(t.value, buffers_[t.buffer]);
            break;
        case Type::Blend: {
            // Takes over the source's buffer and blends the target into it in place.
            Task& source = tasks_[t.deps[0]];
            Task& target = tasks_[t.deps[1]];
            t.buffer = source.buffer;
            blendPoses(buffers_[source.buffer], buffers_[target.buffer], t.value, t.mode,
                       t.mask >= 0 ? masks_[size_t(t.mask)].data() : nullptr, buffers_[t.buffer]);
            release(target.buffer);
            break;
        }
        }
    }
    const Task& last = result >= 0 && size_t(result) < tasks_.size() ? tasks_[size_t(result)] : tasks_.back();
    result_ = buffers_[last.buffer];
    if (result_.isAdditive())
        applyAdditiveToReference(result_);
    return result_;
}

}  // namespace fx::anim
