// glTF skins and animations.
//
// A skin becomes a skeleton whose bone 0 is a synthetic root: the armature (model) space,
// i.e. the parent of the skin's root joints. Skinned instances are placed at the armature's
// world transform, so skinning (model-space joint transform x inverse bind) yields model-space
// positions; the glTF mesh node's own transform cancels out, as the spec defines.

#include "import_anim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <queue>

namespace fx::cook {

namespace {

using Mat4 = std::array<float, 16>;  // column-major

Mat4 identity()
{
    Mat4 m = {};
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    return m;
}

Mat4 mul(const Mat4& a, const Mat4& b)
{
    Mat4 r = {};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k)
                r[c * 4 + row] += a[k * 4 + row] * b[c * 4 + k];
    return r;
}

// Inverse of an affine transform (rotation/scale + translation).
Mat4 inverseAffine(const Mat4& m)
{
    const float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    const float s = det != 0.0f ? 1.0f / det : 0.0f;
    Mat4 r = identity();
    r[0] = (e * i - f * h) * s, r[4] = (c * h - b * i) * s, r[8] = (b * f - c * e) * s;
    r[1] = (f * g - d * i) * s, r[5] = (a * i - c * g) * s, r[9] = (c * d - a * f) * s;
    r[2] = (d * h - e * g) * s, r[6] = (b * g - a * h) * s, r[10] = (a * e - b * d) * s;
    for (int row = 0; row < 3; ++row)
        r[12 + row] = -(r[row] * m[12] + r[4 + row] * m[13] + r[8 + row] * m[14]);
    return r;
}

Mat4 fromTrs(const SrcTransform& t)
{
    const float x = t.r[0], y = t.r[1], z = t.r[2], w = t.r[3];
    Mat4 m = identity();
    m[0] = (1 - 2 * (y * y + z * z)) * t.s[0], m[1] = (2 * (x * y + z * w)) * t.s[0], m[2] = (2 * (x * z - y * w)) * t.s[0];
    m[4] = (2 * (x * y - z * w)) * t.s[1], m[5] = (1 - 2 * (x * x + z * z)) * t.s[1], m[6] = (2 * (y * z + x * w)) * t.s[1];
    m[8] = (2 * (x * z + y * w)) * t.s[2], m[9] = (2 * (y * z - x * w)) * t.s[2], m[10] = (1 - 2 * (x * x + y * y)) * t.s[2];
    m[12] = t.t[0], m[13] = t.t[1], m[14] = t.t[2];
    return m;
}

SrcTransform toTrs(const Mat4& m)
{
    SrcTransform t;
    t.t[0] = m[12], t.t[1] = m[13], t.t[2] = m[14];
    float c[3][3];
    for (int k = 0; k < 3; ++k) {
        t.s[k] = std::sqrt(m[k * 4] * m[k * 4] + m[k * 4 + 1] * m[k * 4 + 1] + m[k * 4 + 2] * m[k * 4 + 2]);
        for (int row = 0; row < 3; ++row)
            c[k][row] = t.s[k] > 0.0f ? m[k * 4 + row] / t.s[k] : 0.0f;
    }
    // Rotation matrix (columns c) -> quaternion.
    const float trace = c[0][0] + c[1][1] + c[2][2];
    float q[4];
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q[3] = 0.25f * s, q[0] = (c[1][2] - c[2][1]) / s, q[1] = (c[2][0] - c[0][2]) / s, q[2] = (c[0][1] - c[1][0]) / s;
    } else if (c[0][0] > c[1][1] && c[0][0] > c[2][2]) {
        const float s = std::sqrt(1.0f + c[0][0] - c[1][1] - c[2][2]) * 2.0f;
        q[3] = (c[1][2] - c[2][1]) / s, q[0] = 0.25f * s, q[1] = (c[1][0] + c[0][1]) / s, q[2] = (c[2][0] + c[0][2]) / s;
    } else if (c[1][1] > c[2][2]) {
        const float s = std::sqrt(1.0f + c[1][1] - c[0][0] - c[2][2]) * 2.0f;
        q[3] = (c[2][0] - c[0][2]) / s, q[0] = (c[1][0] + c[0][1]) / s, q[1] = 0.25f * s, q[2] = (c[2][1] + c[1][2]) / s;
    } else {
        const float s = std::sqrt(1.0f + c[2][2] - c[0][0] - c[1][1]) * 2.0f;
        q[3] = (c[0][1] - c[1][0]) / s, q[0] = (c[2][0] + c[0][2]) / s, q[1] = (c[2][1] + c[1][2]) / s, q[2] = 0.25f * s;
    }
    std::memcpy(t.r, q, sizeof(q));
    return t;
}

SrcTransform nodeLocal(const cgltf_node& n)
{
    if (n.has_matrix) {
        Mat4 m;
        std::memcpy(m.data(), n.matrix, sizeof(n.matrix));
        return toTrs(m);
    }
    SrcTransform t;
    if (n.has_translation)
        std::memcpy(t.t, n.translation, sizeof(t.t));
    if (n.has_rotation)
        std::memcpy(t.r, n.rotation, sizeof(t.r));
    if (n.has_scale)
        std::memcpy(t.s, n.scale, sizeof(t.s));
    return t;
}

Mat4 nodeWorld(const cgltf_node* n)
{
    Mat4 m = identity();
    if (n)
        cgltf_node_transform_world(n, m.data());
    return m;
}

void normalizeQuat(float* q)
{
    const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; ++i)
        q[i] = l > 0.0f ? q[i] / l : (i == 3 ? 1.0f : 0.0f);
}

void slerp(const float* a, const float* b, float t, float* out)
{
    float bb[4] = {b[0], b[1], b[2], b[3]};
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0.0f) {
        d = -d;
        for (float& v : bb)
            v = -v;
    }
    float wa = 1.0f - t, wb = t;
    if (d < 0.9995f) {
        const float theta = std::acos(d), s = std::sin(theta);
        wa = std::sin((1.0f - t) * theta) / s;
        wb = std::sin(t * theta) / s;
    }
    for (int i = 0; i < 4; ++i)
        out[i] = wa * a[i] + wb * bb[i];
    normalizeQuat(out);
}

// Samples one channel (n components) at time t, honoring LINEAR / STEP / CUBICSPLINE.
void sampleChannel(const cgltf_animation_sampler& s, int n, bool rotation, float t, float* out)
{
    const cgltf_size keys = s.input->count;
    std::vector<float> times(keys);
    cgltf_accessor_unpack_floats(s.input, times.data(), keys);
    const bool cubic = s.interpolation == cgltf_interpolation_type_cubic_spline;
    std::vector<float> values(s.output->count * n);
    cgltf_accessor_unpack_floats(s.output, values.data(), values.size());
    auto value = [&](cgltf_size k, int part) { return &values[((cubic ? k * 3 + part : k) * n)]; };  // part: 0 in, 1 v, 2 out

    if (keys == 0)
        return;
    if (t <= times[0] || keys == 1) {
        std::memcpy(out, value(0, 1), n * sizeof(float));
        return;
    }
    if (t >= times[keys - 1]) {
        std::memcpy(out, value(keys - 1, 1), n * sizeof(float));
        return;
    }
    const cgltf_size k = cgltf_size(std::upper_bound(times.begin(), times.end(), t) - times.begin()) - 1;
    const float dt = times[k + 1] - times[k];
    const float u = dt > 0.0f ? (t - times[k]) / dt : 0.0f;
    if (s.interpolation == cgltf_interpolation_type_step) {
        std::memcpy(out, value(k, 1), n * sizeof(float));
    } else if (cubic) {
        const float u2 = u * u, u3 = u2 * u;
        const float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
        for (int i = 0; i < n; ++i)
            out[i] = h00 * value(k, 1)[i] + h10 * dt * value(k, 2)[i] + h01 * value(k + 1, 1)[i] + h11 * dt * value(k + 1, 0)[i];
        if (rotation)
            normalizeQuat(out);
    } else if (rotation) {
        slerp(value(k, 1), value(k + 1, 1), u, out);
    } else {
        for (int i = 0; i < n; ++i)
            out[i] = value(k, 1)[i] + (value(k + 1, 1)[i] - value(k, 1)[i]) * u;
    }
}

// A channel unpacked once (sampleChannel unpacks on every call; node tracks sample each
// channel at every frame).
struct Channel {
    int path = 0;  // 0 translation, 1 rotation, 2 scale
    cgltf_interpolation_type interpolation = cgltf_interpolation_type_linear;
    std::vector<float> times, values;
};

void sample(const Channel& c, float t, float* out)
{
    const int n = c.path == 1 ? 4 : 3;
    const bool cubic = c.interpolation == cgltf_interpolation_type_cubic_spline;
    const size_t keys = c.times.size();
    auto value = [&](size_t k, int part) { return &c.values[(cubic ? k * 3 + part : k) * n]; };
    if (keys == 0)
        return;
    if (t <= c.times[0] || keys == 1) {
        std::memcpy(out, value(0, 1), n * sizeof(float));
        return;
    }
    if (t >= c.times[keys - 1]) {
        std::memcpy(out, value(keys - 1, 1), n * sizeof(float));
        return;
    }
    const size_t k = size_t(std::upper_bound(c.times.begin(), c.times.end(), t) - c.times.begin()) - 1;
    const float dt = c.times[k + 1] - c.times[k];
    const float u = dt > 0.0f ? (t - c.times[k]) / dt : 0.0f;
    if (c.interpolation == cgltf_interpolation_type_step) {
        std::memcpy(out, value(k, 1), n * sizeof(float));
    } else if (cubic) {
        const float u2 = u * u, u3 = u2 * u;
        const float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
        for (int i = 0; i < n; ++i)
            out[i] = h00 * value(k, 1)[i] + h10 * dt * value(k, 2)[i] + h01 * value(k + 1, 1)[i] + h11 * dt * value(k + 1, 0)[i];
        if (c.path == 1)
            normalizeQuat(out);
    } else if (c.path == 1) {
        slerp(value(k, 1), value(k + 1, 1), u, out);
    } else {
        for (int i = 0; i < n; ++i)
            out[i] = value(k, 1)[i] + (value(k + 1, 1)[i] - value(k, 1)[i]) * u;
    }
}

}  // namespace

void importNodeTracks(const cgltf_data* data, SrcScene& scene, float fps)
{
    const size_t count = data->nodes_count;
    const auto indexOf = [&](const cgltf_node* n) { return size_t(n - data->nodes); };
    std::vector<uint8_t> joint(count, 0);  // (skins' joints: their animation is a clip)
    for (cgltf_size s = 0; s < data->skins_count; ++s)
        for (cgltf_size j = 0; j < data->skins[s].joints_count; ++j)
            joint[indexOf(data->skins[s].joints[j])] = 1;
    std::vector<std::vector<Channel>> channels(count);
    float duration = 0.0f;
    for (cgltf_size a = 0; a < data->animations_count; ++a)
        for (cgltf_size c = 0; c < data->animations[a].channels_count; ++c) {
            const cgltf_animation_channel& ch = data->animations[a].channels[c];
            const int path = ch.target_path == cgltf_animation_path_type_translation ? 0
                             : ch.target_path == cgltf_animation_path_type_rotation  ? 1
                             : ch.target_path == cgltf_animation_path_type_scale     ? 2
                                                                                     : -1;
            if (!ch.target_node || path < 0 || joint[indexOf(ch.target_node)] || !ch.sampler->input || !ch.sampler->output)
                continue;
            Channel out;
            out.path = path;
            out.interpolation = ch.sampler->interpolation;
            out.times.resize(ch.sampler->input->count);
            cgltf_accessor_unpack_floats(ch.sampler->input, out.times.data(), out.times.size());
            out.values.resize(ch.sampler->output->count * (path == 1 ? 4 : 3));
            cgltf_accessor_unpack_floats(ch.sampler->output, out.values.data(), out.values.size());
            if (!out.times.empty())
                duration = std::max(duration, out.times.back());
            channels[indexOf(ch.target_node)].push_back(std::move(out));
        }
    // Nodes that move: animated themselves, or under one that is.
    std::vector<int8_t> moving(count, -1);
    const auto moves = [&](auto&& self, size_t n) -> bool {
        if (moving[n] < 0)
            moving[n] = !channels[n].empty() || (data->nodes[n].parent && self(self, indexOf(data->nodes[n].parent))) ? 1 : 0;
        return moving[n] == 1;
    };
    bool any = false;
    for (size_t n = 0; n < count; ++n)
        any = moves(moves, n) || any;
    if (!any)
        return;
    const uint32_t frames = uint32_t(std::lround(duration * fps)) + 1;
    scene.nodeFps = fps;
    scene.nodeFrames = frames;

    // Each moving node's world transform at every frame (parents first through the recursion).
    std::vector<Mat4> world(count);
    std::vector<uint32_t> at(count, ~0u);
    std::unordered_map<size_t, std::vector<float>> rows;  // node -> 12 floats per frame
    std::vector<size_t> targets;
    for (const SrcInstance& inst : scene.instances)
        if (inst.node >= 0 && moving[size_t(inst.node)] == 1 && scene.primitives[inst.primitive].skeleton < 0)
            targets.push_back(size_t(inst.node));
    for (const SrcCamera& c : scene.cameras)
        if (c.node >= 0 && moving[size_t(c.node)] == 1)
            targets.push_back(size_t(c.node));
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    for (uint32_t f = 0; f < frames; ++f) {
        const float t = float(f) / fps;
        const auto worldOf = [&](auto&& self, size_t n) -> const Mat4& {
            if (at[n] == f)
                return world[n];
            at[n] = f;
            const cgltf_node& node = data->nodes[n];
            if (moving[n] != 1) {
                world[n] = nodeWorld(&node);
                return world[n];
            }
            SrcTransform local = nodeLocal(node);
            for (const Channel& c : channels[n])
                sample(c, t, c.path == 0 ? local.t : c.path == 1 ? local.r : local.s);
            world[n] = node.parent ? mul(self(self, indexOf(node.parent)), fromTrs(local)) : fromTrs(local);
            return world[n];
        };
        for (size_t n : targets) {
            const Mat4& m = worldOf(worldOf, n);
            std::vector<float>& r = rows[n];
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 4; ++col)
                    r.push_back(m[col * 4 + row]);
        }
    }
    // Only the frames where it moves: held before the first change and after the last.
    std::unordered_map<size_t, std::pair<uint32_t, uint32_t>> kept;  // node -> data index, first frame
    for (size_t n : targets) {
        std::vector<float>& r = rows[n];
        const auto same = [&](uint32_t a, uint32_t b) {
            for (int i = 0; i < 12; ++i)
                if (std::fabs(r[a * 12 + i] - r[b * 12 + i]) > 1e-6f * (1.0f + std::fabs(r[b * 12 + i])))
                    return false;
            return true;
        };
        uint32_t first = 0, last = frames - 1;
        while (first < last && same(first, first + 1))
            ++first;
        while (last > first && same(last, last - 1))
            --last;
        if (first == last && frames > 1 && same(0, frames - 1))
            continue;  // (under a moving node, but not moving itself)
        std::vector<float> data(r.begin() + first * 12, r.begin() + (last + 1) * 12);
        kept[n] = {uint32_t(scene.nodeTrackData.size()), first};
        scene.nodeTrackData.push_back(std::move(data));
    }
    for (uint32_t i = 0; i < scene.instances.size(); ++i) {
        const SrcInstance& inst = scene.instances[i];
        if (inst.node < 0 || scene.primitives[inst.primitive].skeleton >= 0)
            continue;
        if (auto it = kept.find(size_t(inst.node)); it != kept.end())
            scene.nodeTracks.push_back({i, false, it->second.second, uint32_t(scene.nodeTrackData[it->second.first].size() / 12),
                                        it->second.first});
    }
    for (uint32_t i = 0; i < scene.cameras.size(); ++i)
        if (auto it = kept.find(size_t(scene.cameras[i].node)); scene.cameras[i].node >= 0 && it != kept.end())
            scene.nodeTracks.push_back({i, true, it->second.second, uint32_t(scene.nodeTrackData[it->second.first].size() / 12),
                                        it->second.first});
}

std::unordered_map<const cgltf_mesh*, MeshSkinBinding> importSkeletons(const cgltf_data* data, SrcScene& scene)
{
    std::unordered_map<const cgltf_skin*, std::pair<int32_t, std::vector<uint16_t>>> skins;
    std::unordered_map<const cgltf_skin*, Mat4> armatures;
    for (cgltf_size si = 0; si < data->skins_count; ++si) {
        const cgltf_skin& skin = data->skins[si];
        if (skin.joints_count == 0)
            continue;
        if (skin.joints_count > 255) {
            std::fprintf(stderr, "falcon cook: skin '%s' has %zu joints (max 255), skipped\n", skin.name ? skin.name : "?",
                         size_t(skin.joints_count));
            continue;
        }
        std::unordered_map<const cgltf_node*, uint32_t> jointIndex;
        for (cgltf_size j = 0; j < skin.joints_count; ++j)
            jointIndex[skin.joints[j]] = uint32_t(j);
        std::vector<Mat4> ibm(skin.joints_count, identity());
        if (skin.inverse_bind_matrices)
            for (cgltf_size j = 0; j < skin.joints_count; ++j)
                cgltf_accessor_read_float(skin.inverse_bind_matrices, j, ibm[j].data(), 16);

        // Roots: joints whose parent is not a joint. The armature is the first root's parent.
        std::vector<cgltf_size> roots;
        for (cgltf_size j = 0; j < skin.joints_count; ++j)
            if (!skin.joints[j]->parent || !jointIndex.count(skin.joints[j]->parent))
                roots.push_back(j);
        const cgltf_node* armatureNode = skin.joints[roots[0]]->parent;
        const Mat4 armature = nodeWorld(armatureNode);
        const Mat4 toModel = inverseAffine(armature);

        SrcSkeleton sk;
        sk.name = skin.name ? skin.name : "skeleton";
        sk.boneNames.push_back("root");
        sk.parents.push_back(-1);
        sk.reference.push_back({});
        sk.inverseBind.push_back(identity());
        sk.nodes.push_back(-1);
        // Breadth-first from the roots: parents always precede children.
        std::vector<uint16_t> jointToBone(skin.joints_count, 0);
        std::queue<std::pair<cgltf_size, int32_t>> open;
        for (cgltf_size r : roots)
            open.push({r, 0});
        while (!open.empty()) {
            const auto [j, parent] = open.front();
            open.pop();
            const cgltf_node* node = skin.joints[j];
            const uint16_t bone = uint16_t(sk.parents.size());
            jointToBone[j] = bone;
            sk.boneNames.push_back(node->name ? node->name : "bone" + std::to_string(bone));
            sk.parents.push_back(parent);
            // Roots are re-expressed relative to the armature (exact when it is their parent).
            sk.reference.push_back(parent == 0 && node->parent != armatureNode ? toTrs(mul(toModel, nodeWorld(node)))
                                                                                : nodeLocal(*node));
            sk.inverseBind.push_back(ibm[j]);
            sk.nodes.push_back(int32_t(cgltf_node_index(data, node)));
            for (cgltf_size c = 0; c < node->children_count; ++c)
                if (auto it = jointIndex.find(node->children[c]); it != jointIndex.end())
                    open.push({it->second, bone});
        }
        skins[&skin] = {int32_t(scene.skeletons.size()), jointToBone};
        armatures[&skin] = armature;
        scene.skeletons.push_back(std::move(sk));
    }

    std::unordered_map<const cgltf_mesh*, MeshSkinBinding> bindings;
    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (!node.mesh || !node.skin || bindings.count(node.mesh))
            continue;
        auto it = skins.find(node.skin);
        if (it == skins.end())
            continue;
        MeshSkinBinding b;
        b.skeleton = it->second.first;
        b.jointToBone = it->second.second;
        std::memcpy(b.armature, armatures[node.skin].data(), sizeof(b.armature));
        bindings[node.mesh] = std::move(b);
    }
    return bindings;
}

void importSkinWeights(const cgltf_primitive& prim, const MeshSkinBinding& binding, SrcPrimitive& out)
{
    const cgltf_accessor *joints = nullptr, *weights = nullptr;
    for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
        if (prim.attributes[i].index != 0)
            continue;
        if (prim.attributes[i].type == cgltf_attribute_type_joints)
            joints = prim.attributes[i].data;
        else if (prim.attributes[i].type == cgltf_attribute_type_weights)
            weights = prim.attributes[i].data;
    }
    if (!joints || !weights)
        return;
    out.skeleton = binding.skeleton;
    for (size_t v = 0; v < out.vertices.size(); ++v) {
        cgltf_uint j[4] = {};
        float w[4] = {};
        cgltf_accessor_read_uint(joints, v, j, 4);
        cgltf_accessor_read_float(weights, v, w, 4);
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k)
            sum += w[k] = std::max(w[k], 0.0f);
        SrcVertex& o = out.vertices[v];
        for (int k = 0; k < 4; ++k) {
            o.joints[k] = j[k] < binding.jointToBone.size() ? binding.jointToBone[j[k]] : 0;
            o.weights[k] = sum > 0.0f ? w[k] / sum : (k == 0 ? 1.0f : 0.0f);
        }
    }
}

void importClips(const cgltf_data* data, SrcScene& scene, float fps)
{
    // glTF node -> (skeleton, bone)
    std::unordered_map<int32_t, std::pair<uint32_t, uint32_t>> boneOf;
    for (uint32_t s = 0; s < scene.skeletons.size(); ++s)
        for (uint32_t b = 0; b < scene.skeletons[s].nodes.size(); ++b)
            if (scene.skeletons[s].nodes[b] >= 0)
                boneOf[scene.skeletons[s].nodes[b]] = {s, b};

    for (cgltf_size ai = 0; ai < data->animations_count; ++ai) {
        const cgltf_animation& anim = data->animations[ai];
        // Target skeleton: the one most of the channels animate.
        std::vector<uint32_t> votes(scene.skeletons.size(), 0);
        float duration = 0.0f;
        for (cgltf_size c = 0; c < anim.channels_count; ++c) {
            const cgltf_animation_channel& ch = anim.channels[c];
            if (!ch.target_node)
                continue;
            if (auto it = boneOf.find(int32_t(cgltf_node_index(data, ch.target_node))); it != boneOf.end())
                ++votes[it->second.first];
            float tmax = 0.0f;
            cgltf_accessor_read_float(ch.sampler->input, ch.sampler->input->count - 1, &tmax, 1);
            duration = std::max(duration, tmax);
        }
        const auto best = std::max_element(votes.begin(), votes.end());
        if (best == votes.end() || *best == 0)
            continue;
        const uint32_t skeleton = uint32_t(best - votes.begin());
        const SrcSkeleton& sk = scene.skeletons[skeleton];

        SrcClip clip;
        clip.name = anim.name ? anim.name : "clip" + std::to_string(ai);
        clip.skeleton = skeleton;
        clip.duration = duration;
        clip.frameCount = std::max(2u, uint32_t(std::lround(duration * fps)) + 1);
        const size_t bones = sk.parents.size();
        clip.frames.resize(clip.frameCount * bones);
        for (uint32_t f = 0; f < clip.frameCount; ++f) {
            const float t = duration * float(f) / float(clip.frameCount - 1);
            SrcTransform* pose = &clip.frames[f * bones];
            std::copy(sk.reference.begin(), sk.reference.end(), pose);
            for (cgltf_size c = 0; c < anim.channels_count; ++c) {
                const cgltf_animation_channel& ch = anim.channels[c];
                if (!ch.target_node)
                    continue;
                auto it = boneOf.find(int32_t(cgltf_node_index(data, ch.target_node)));
                if (it == boneOf.end() || it->second.first != skeleton)
                    continue;
                SrcTransform& out = pose[it->second.second];
                if (ch.target_path == cgltf_animation_path_type_translation)
                    sampleChannel(*ch.sampler, 3, false, t, out.t);
                else if (ch.target_path == cgltf_animation_path_type_rotation)
                    sampleChannel(*ch.sampler, 4, true, t, out.r);
                else if (ch.target_path == cgltf_animation_path_type_scale)
                    sampleChannel(*ch.sampler, 3, false, t, out.s);
            }
        }
        scene.clips.push_back(std::move(clip));
    }
}

}  // namespace fx::cook
