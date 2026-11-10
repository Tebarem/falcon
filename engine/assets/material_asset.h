#pragma once

// Material assets (.fmat): a material made in the editor. A project's scenes are cooked with
// every one of them (falcon cook --material x.fmat: its textures go into the virtual texture);
// its factors (colours, metallic, roughness, emission, alpha) are read again whenever a scene
// loads and edited live, so only its textures need a cook.
//
//   {"format": 1, "name": "Rock", "baseColor": [r, g, b, a], "metallic": 0, "roughness": 0.5,
//    "emissive": [r, g, b], "emissiveStrength": 1, "normalScale": 1, "occlusionStrength": 1,
//    "alpha": "opaque" | "mask" | "blend", "alphaCutoff": 0.5, "doubleSided": false, "size": [w, h], "parallax": 0.02,
//    "textures": {"baseColor": "Assets/rock.png", "normal": ..., "metallicRoughness": ..., "occlusion": ...},
//    "acoustic": {"preset": "rock", "absorption": [l, m, h], "scattering": s, "transmission": [l, m, h]},
//    "surface": {"preset": "rock", "impact": "Assets/Audio/Hit.fsound", "scrape": "", "roll": "", "hardness": h, ...}}
//
// Colours are linear; texture paths are relative to the project (a library's material: to its
// library, "@falcon/..." or "@library/...", project/asset_library.h).

#include "assets/acoustic_material.h"
#include "assets/fpak_format.h"
#include "assets/surface_sound.h"

#include <string>

namespace fx {

struct MaterialAsset {
    static constexpr const char* kTextureSlots[4] = {"baseColor", "normal", "metallicRoughness", "occlusion"};

    std::string name;
    float baseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float metallic = 0.0f, roughness = 0.5f;
    // The roughness the roughness map's black gives (its white gives `roughness`): above 0, a
    // material comes out rougher than its map, its variation kept (0: the map times `roughness`).
    // On the GPU: vtSize bits 24..31, in 255ths.
    float roughnessMin = 0.0f;
    float emissive[3] = {0.0f, 0.0f, 0.0f};
    float emissiveStrength = 1.0f;
    float normalScale = 1.0f, occlusionStrength = 1.0f;
    std::string alpha = "opaque";  // opaque | mask | blend
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
    std::string textures[4];  // by kTextureSlots; empty: none
    // How much of the world one repeat of its textures covers (metres, width x height; 0: not said):
    // level-mesh faces with world or box UVs take it as their UV scale when it is put on them.
    float size[2] = {0.0f, 0.0f};
    // (R4) A project's surface shader (its path: "Shaders/Toon.slang"; "": none) and its values.
    std::string shader;
    float params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // (T2) Refraction, for "alpha": "blend": how far what is seen through it is bent (the screen
    // offset for a surface at 45 degrees, scaled by 1 - 1/ior), 0: none. Rough ones blur it.
    float refraction = 0.0f, ior = 1.5f;
    // (V1) A surface shader's vertex function: how far it moves vertices at most (metres: culling)
    // and the camera distance it runs within (0: any; past it the surface stays still).
    float vertexBound = 1.0f, vertexDistance = 0.0f;
    // Height displacement: cooked into the geometry of the meshes that use the material
    // (tools/cook/build_displace.cpp). The height map's grey 0..1, minus `heightCenter`, times
    // `displacement` metres, along the surface's normal; triangles subdivided to `displacementDetail`
    // metres (0: about one texel of the map).
    std::string height;  // ("textures": {"height": ...})
    float displacement = 0.0f, heightCenter = 0.0f, displacementDetail = 0.0f;
    // Parallax occlusion: how deep (metres) the height map's black lies under the surface, drawn by
    // stepping the view ray through it (no geometry; 0: none). A factor, read live: only the height
    // map needs a cook (into the ORM pages' alpha). With displacement too, it draws over the displaced
    // geometry (relief finer than its triangles). On the GPU: the material's flags bits 16..31, a half.
    float parallax = 0.0f;
    bool displaced() const { return !height.empty() && displacement != 0.0f; }
    // How it treats sound (assets/acoustic_material.h): heard live, not cooked.
    std::string acousticPreset = "generic";  // "" when the values are its own
    AcousticParams acoustic;
    // What it sounds like hit, scraped or rolled on (assets/surface_sound.h): heard live too.
    std::string surfacePreset = "generic";
    SurfaceSound surface;
};

bool readMaterialAsset(const std::string& path, MaterialAsset& out, std::string& error);
bool writeMaterialAsset(const std::string& path, const MaterialAsset& m, std::string& error);
// The factors into a GPU material (its textures, the VT page table, stay as cooked).
void applyMaterialFactors(const MaterialAsset& m, fpak::MaterialGpu& gpu);
// What a cook needs (its textures, by path): when this changes, scenes must be cooked again.
std::string materialCookKey(const MaterialAsset& m);

}  // namespace fx
