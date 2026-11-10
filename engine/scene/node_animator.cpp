#include "scene/node_animator.h"

#include "scene/scene.h"

#include <cmath>
#include <cstring>

namespace fx {

void NodeAnimator::update(Scene& scene, float dt)
{
    time_ += dt;
    flecs::world& world = scene.world();
    for (const Link& l : links_) {
        const ScenePak::NodeTrack& t = *l.track;
        // The loop's frame, then this track's (held before its first and after its last).
        const float loop = float(std::max(t.loop, 1u));
        const float frame = std::fmod(time_ * fps_, loop) - float(t.first);
        const float clamped = std::fmin(std::fmax(frame, 0.0f), float(t.count - 1));
        const uint32_t a = uint32_t(clamped), b = std::min(a + 1, t.count - 1);
        const float u = clamped - float(a);
        float rows[12];
        for (int i = 0; i < 12; ++i)
            rows[i] = t.rows[a * 12 + i] + (t.rows[b * 12 + i] - t.rows[a * 12 + i]) * u;
        const flecs::entity e(world, l.entity);
        if (!e.is_alive())
            continue;
        if (scene.parentOf(e)) {
            LocalTransform local;
            std::memcpy(local.rows, rows, sizeof(rows));
            e.set<LocalTransform>(local);
        } else {
            scene.setWorld(e, reinterpret_cast<const float(*)[4]>(rows));
        }
    }
}

}  // namespace fx
