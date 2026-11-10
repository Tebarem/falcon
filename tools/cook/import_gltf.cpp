// glTF 2.0 import via cgltf: triangle primitives, metallic-roughness materials and the
// node hierarchy flattened into world-space instances.

#include "cook.h"
#include "import_anim.h"
#include "core/mesh_tangents.h"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unordered_map>

namespace fx::cook {

namespace {

const cgltf_accessor* findAttribute(const cgltf_primitive& prim, cgltf_attribute_type type)
{
    for (cgltf_size i = 0; i < prim.attributes_count; ++i)
        if (prim.attributes[i].type == type && prim.attributes[i].index == 0)
            return prim.attributes[i].data;
    return nullptr;
}

std::vector<float> unpack(const cgltf_accessor* acc, size_t components)
{
    std::vector<float> out(acc->count * components);
    cgltf_accessor_unpack_floats(acc, out.data(), out.size());
    return out;
}

// MikkTSpace tangents for primitives with UVs but no tangents (they used to get (1,0,0), wrong for
// any normal map not authored along +U). Corners of one vertex can disagree (mirrored UVs), so
// every corner becomes its own vertex here; buildMesh welds the identical ones back.
void computeTangents(SrcPrimitive& p)
{
    const fx::MeshAttribute pos{&p.vertices[0].px, sizeof(SrcVertex)};
    const fx::MeshAttribute nrm{&p.vertices[0].nx, sizeof(SrcVertex)};
    const fx::MeshAttribute uv{&p.vertices[0].u, sizeof(SrcVertex)};
    std::vector<float> tangents(p.indices.size() * 4);
    if (p.indices.empty() || !fx::generateTangents(p.indices.data(), p.indices.size(), pos, nrm, uv, tangents.data()))
        return;
    std::vector<SrcVertex> corners(p.indices.size());
    for (size_t c = 0; c < p.indices.size(); ++c) {
        SrcVertex v = p.vertices[p.indices[c]];
        v.tx = tangents[c * 4];
        v.ty = tangents[c * 4 + 1];
        v.tz = tangents[c * 4 + 2];
        v.tw = tangents[c * 4 + 3];
        corners[c] = v;
        p.indices[c] = uint32_t(c);
    }
    p.vertices = std::move(corners);
}

// Smooth normals for primitives that do not provide them.
void computeNormals(SrcPrimitive& p)
{
    for (auto& v : p.vertices)
        v.nx = v.ny = v.nz = 0.0f;
    for (size_t i = 0; i + 2 < p.indices.size(); i += 3) {
        SrcVertex& a = p.vertices[p.indices[i]];
        SrcVertex& b = p.vertices[p.indices[i + 1]];
        SrcVertex& c = p.vertices[p.indices[i + 2]];
        const float e1[3] = {b.px - a.px, b.py - a.py, b.pz - a.pz};
        const float e2[3] = {c.px - a.px, c.py - a.py, c.pz - a.pz};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        for (SrcVertex* v : {&a, &b, &c}) {
            v->nx += n[0];
            v->ny += n[1];
            v->nz += n[2];
        }
    }
    for (auto& v : p.vertices) {
        const float l = std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
        if (l > 0.0f) {
            v.nx /= l;
            v.ny /= l;
            v.nz /= l;
        } else {
            v.ny = 1.0f;
        }
    }
}

}  // namespace

bool importGltf(const std::string& path, SrcScene& scene, std::vector<std::string>& sourceFiles)
{
    cgltf_options options = {};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        std::fprintf(stderr, "falcon cook: cannot parse %s\n", path.c_str());
        return false;
    }
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        std::fprintf(stderr, "falcon cook: cannot load buffers of %s\n", path.c_str());
        cgltf_free(data);
        return false;
    }
    sourceFiles.push_back(path);
    const std::filesystem::path dir = std::filesystem::path(path).parent_path();
    for (cgltf_size i = 0; i < data->buffers_count; ++i)
        if (data->buffers[i].uri && std::strncmp(data->buffers[i].uri, "data:", 5) != 0)
            sourceFiles.push_back((dir / data->buffers[i].uri).string());

    // Images: referenced by path when external, copied when embedded (.glb / data URIs).
    for (cgltf_size i = 0; i < data->images_count; ++i) {
        const cgltf_image& img = data->images[i];
        SrcImage out;
        if (img.buffer_view) {
            const uint8_t* p = static_cast<const uint8_t*>(cgltf_buffer_view_data(img.buffer_view));
            if (p)
                out.bytes.assign(p, p + img.buffer_view->size);
        } else if (img.uri && std::strncmp(img.uri, "data:", 5) != 0) {
            std::string uri = img.uri;
            cgltf_decode_uri(uri.data());
            uri.resize(std::strlen(uri.c_str()));
            out.path = (dir / std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(uri.c_str())))).string();
            sourceFiles.push_back(out.path);
        }
        scene.images.push_back(std::move(out));
    }
    auto imageOf = [&](const cgltf_texture_view& v) -> int {
        return v.texture && v.texture->image ? int(cgltf_image_index(data, v.texture->image)) : -1;
    };

    // Materials (index 0 is the default for primitives without one).
    auto defaultMaterial = [] {
        fpak::MaterialGpu g = {};
        g.baseColor[0] = g.baseColor[1] = g.baseColor[2] = g.baseColor[3] = 1.0f;
        g.roughness = 1.0f;
        g.alphaCutoff = 0.5f;
        g.vtPageTable = fpak::kVtNoTexture;
        g.normalScale = 1.0f;
        g.occlusionStrength = 1.0f;
        return g;
    };
    scene.materials.push_back(defaultMaterial());
    scene.materialTextures.push_back({});
    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        const cgltf_material& m = data->materials[i];
        fpak::MaterialGpu g = defaultMaterial();
        SrcMaterialTextures t;
        const cgltf_pbr_metallic_roughness& pbr = m.pbr_metallic_roughness;
        if (m.has_pbr_metallic_roughness) {
            std::memcpy(g.baseColor, pbr.base_color_factor, sizeof(g.baseColor));
            g.metallic = pbr.metallic_factor;
            g.roughness = pbr.roughness_factor;
            t.baseColor = imageOf(pbr.base_color_texture);
            t.metallicRoughness = imageOf(pbr.metallic_roughness_texture);
        }
        t.normal = imageOf(m.normal_texture);
        t.occlusion = imageOf(m.occlusion_texture);
        if (t.normal >= 0)
            g.normalScale = m.normal_texture.scale;
        if (t.occlusion >= 0)
            g.occlusionStrength = m.occlusion_texture.scale;
        std::memcpy(g.emissive, m.emissive_factor, sizeof(m.emissive_factor));
        g.emissive[3] = m.has_emissive_strength ? m.emissive_strength.emissive_strength : 1.0f;
        g.alphaCutoff = m.alpha_cutoff;
        g.flags = (m.alpha_mode == cgltf_alpha_mode_mask ? fpak::kMaterialAlphaMask : 0) |
                  (m.alpha_mode == cgltf_alpha_mode_blend ? fpak::kMaterialAlphaBlend : 0) |
                  (m.double_sided ? fpak::kMaterialDoubleSided : 0);
        scene.materials.push_back(g);
        scene.materialTextures.push_back(t);
    }

    // Skeletons first: skinned primitives remap their joints to skeleton bones.
    const auto skinBindings = importSkeletons(data, scene);

    // KHR_texture_transform (texture tiling, e.g. Blender's Mapping node): the materials read
    // one UV set, so a material's transform is baked into its primitives' UVs (uv' = T R S uv).
    // Exact when the material's textures share one transform, as exporters write it.
    auto uvTransform = [](const cgltf_material* m) -> const cgltf_texture_transform* {
        if (!m)
            return nullptr;
        const cgltf_texture_view* views[] = {&m->pbr_metallic_roughness.base_color_texture,
                                             &m->pbr_metallic_roughness.metallic_roughness_texture, &m->normal_texture,
                                             &m->occlusion_texture, &m->emissive_texture};
        const cgltf_texture_transform* first = nullptr;
        for (const cgltf_texture_view* v : views) {
            if (!v->texture || !v->has_transform)
                continue;
            if (!first)
                first = &v->transform;
            else if (std::memcmp(first, &v->transform, sizeof(*first)) != 0)
                std::fprintf(stderr, "falcon cook: material '%s': textures with different UV transforms; using the first\n",
                             m->name ? m->name : "");
        }
        return first;
    };

    // Primitives, one SrcPrimitive per (mesh, primitive); instances reference them.
    std::unordered_map<const cgltf_mesh*, uint32_t> meshFirstPrim;
    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh& mesh = data->meshes[mi];
        meshFirstPrim[&mesh] = uint32_t(scene.primitives.size());
        for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
            const cgltf_primitive& prim = mesh.primitives[pi];
            SrcPrimitive out;
            const cgltf_accessor* pos = findAttribute(prim, cgltf_attribute_type_position);
            if (prim.type != cgltf_primitive_type_triangles || !pos) {
                scene.primitives.push_back(std::move(out));  // keep indexing stable; empty is skipped
                continue;
            }
            out.material = prim.material ? uint32_t(cgltf_material_index(data, prim.material) + 1) : 0;
            const size_t n = pos->count;
            out.vertices.resize(n);
            const std::vector<float> p = unpack(pos, 3);
            const cgltf_accessor* nrm = findAttribute(prim, cgltf_attribute_type_normal);
            const cgltf_accessor* uv = findAttribute(prim, cgltf_attribute_type_texcoord);
            const cgltf_accessor* tan = findAttribute(prim, cgltf_attribute_type_tangent);
            const std::vector<float> nv = nrm ? unpack(nrm, 3) : std::vector<float>();
            const std::vector<float> uvv = uv ? unpack(uv, 2) : std::vector<float>();
            const std::vector<float> tv = tan ? unpack(tan, 4) : std::vector<float>();
            const cgltf_texture_transform* xf = uvTransform(prim.material);
            const float xc = xf ? std::cos(xf->rotation) : 1.0f, xs = xf ? std::sin(xf->rotation) : 0.0f;
            for (size_t v = 0; v < n; ++v) {
                SrcVertex& o = out.vertices[v];
                o = {p[v * 3], p[v * 3 + 1], p[v * 3 + 2], 0, 1, 0, 0, 0, 1, 0, 0, 1};
                if (nrm) {
                    o.nx = nv[v * 3];
                    o.ny = nv[v * 3 + 1];
                    o.nz = nv[v * 3 + 2];
                }
                if (uv) {
                    o.u = uvv[v * 2];
                    o.v = uvv[v * 2 + 1];
                    if (xf) {
                        const float su = o.u * xf->scale[0], sv = o.v * xf->scale[1];
                        o.u = xc * su + xs * sv + xf->offset[0];
                        o.v = -xs * su + xc * sv + xf->offset[1];
                    }
                }
                if (tan) {
                    o.tx = tv[v * 4];
                    o.ty = tv[v * 4 + 1];
                    o.tz = tv[v * 4 + 2];
                    o.tw = tv[v * 4 + 3];
                }
            }
            if (prim.indices) {
                out.indices.resize(prim.indices->count);
                cgltf_accessor_unpack_indices(prim.indices, out.indices.data(), sizeof(uint32_t), out.indices.size());
            } else {
                out.indices.resize(n);
                for (size_t v = 0; v < n; ++v)
                    out.indices[v] = uint32_t(v);
            }
            out.indices.resize(out.indices.size() - out.indices.size() % 3);
            if (!nrm)
                computeNormals(out);
            if (auto it = skinBindings.find(&mesh); it != skinBindings.end())
                importSkinWeights(prim, it->second, out);
            if (!tan && uv)  // after the skin weights: it splits vertices per corner
                computeTangents(out);
            scene.primitives.push_back(std::move(out));
        }
    }

    // Punctual lights: world position and direction (-Z of the node) from the node hierarchy.
    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (!node.light)
            continue;
        const cgltf_light& l = *node.light;
        float world[16];
        cgltf_node_transform_world(&node, world);
        fpak::LightGpu g = {};
        g.positionRange[0] = world[12];
        g.positionRange[1] = world[13];
        g.positionRange[2] = world[14];
        g.positionRange[3] = l.range;
        float d[3] = {-world[8], -world[9], -world[10]};
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int a = 0; a < 3; ++a) {
            g.direction[a] = len > 0.0f ? d[a] / len : (a == 1 ? -1.0f : 0.0f);
            g.color[a] = l.color[a] * l.intensity;
        }
        g.color[3] = float(l.type == cgltf_light_type_spot          ? fpak::kLightSpot
                           : l.type == cgltf_light_type_directional ? fpak::kLightDirectional
                                                                    : fpak::kLightPoint);
        g.direction[3] = l.type == cgltf_light_type_spot ? std::cos(l.spot_outer_cone_angle) : -1.0f;
        g.params[0] = l.type == cgltf_light_type_spot ? std::cos(l.spot_inner_cone_angle) : -1.0f;
        if (l.intensity <= 0.0f)
            std::fprintf(stderr, "falcon cook: light '%s' has zero intensity\n", node.name ? node.name : "?");
        scene.lights.push_back(g);
    }

    // Instances: every node with a mesh, in node order (deterministic).
    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (!node.mesh)
            continue;
        float world[16];
        cgltf_node_transform_world(&node, world);
        // Skinned: placed at the skeleton's armature; the mesh node's own transform cancels.
        if (auto it = skinBindings.find(node.mesh); node.skin && it != skinBindings.end())
            std::memcpy(world, it->second.armature, sizeof(world));
        const uint32_t first = meshFirstPrim[node.mesh];
        for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi) {
            if (scene.primitives[first + pi].indices.empty())
                continue;
            SrcInstance inst;
            std::memcpy(inst.world, world, sizeof(world));
            inst.primitive = first + uint32_t(pi);
            inst.node = int32_t(ni);
            inst.name = node.name && *node.name             ? node.name
                        : node.mesh->name && *node.mesh->name ? node.mesh->name
                                                              : "Node " + std::to_string(ni);
            // Several primitives (one per material): tell them apart by material.
            if (node.mesh->primitives_count > 1) {
                const cgltf_material* mat = node.mesh->primitives[pi].material;
                inst.name += "/" + (mat && mat->name && *mat->name ? std::string(mat->name) : std::to_string(pi));
            }
            scene.instances.push_back(inst);
        }
    }

    // Cameras (perspective), for a model's own shots: where they are and their lens.
    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (!node.camera || node.camera->type != cgltf_camera_type_perspective)
            continue;
        SrcCamera c;
        c.name = node.name && *node.name ? node.name : node.camera->name && *node.camera->name ? node.camera->name : "Camera";
        c.yfov = node.camera->data.perspective.yfov;
        cgltf_node_transform_world(&node, c.world);
        c.node = int32_t(ni);
        scene.cameras.push_back(std::move(c));
    }

    importClips(data, scene, 30.0f);
    importNodeTracks(data, scene, 30.0f);

    cgltf_free(data);
    return true;
}

}  // namespace fx::cook
