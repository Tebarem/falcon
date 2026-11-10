# Blender (background) -> glTF for `falcon cook`. Run by scripts/blend-to-fpak.ps1:
#   blender -b -Y scene.blend --python scripts/blender_export.py -- out.gltf
# (-Y: the .blend's own Python scripts never run.)
#
# What it exports: the visible objects with their modifiers applied, Geometry Nodes instances
# as glTF nodes that share meshes (the engine instances them), lights, and textures as files.
# What glTF can't say directly is fixed up in the written file:
# - Cut-out materials (a Transparent BSDF mixed in, the usual foliage setup, or a Principled
#   BSDF with its Alpha connected): alphaMode MASK, cutoff 0.5. The color texture's alpha is
#   the mask.
# - Sun strength: Blender's strength in W/m^2 carries over 1:1 as the engine's sun irradiance
#   (its default sun is about 3.2). The exporter's RAW mode writes 1 for any sun.
# - Roughness remapped by a (linear) Map Range between its texture and the Principled BSDF: the
#   exporter writes the raw texture, so the material gets a remapped copy of it (the generated
#   open world's ground: 0.46 raw, about 0.9 as rendered; raw, the terrain glared white).
# Not exported: procedural textures, displacement, and Geometry Nodes behaviour (the scatter is
# baked as it is in the file).

import json
import os
import sys

import bpy
import numpy as np

out = sys.argv[sys.argv.index("--") + 1]


# (Not blend_method: Blender 4.2+ reports HASHED for every material.)
def cut_out(material):
    tree = material.node_tree if material.use_nodes else None
    if not tree:
        return False
    for n in tree.nodes:
        if n.type == "BSDF_TRANSPARENT":
            return True
        if n.type == "BSDF_PRINCIPLED" and n.inputs["Alpha"].is_linked:
            return True
    return False


masked = {m.name for m in bpy.data.materials if m.users and cut_out(m)}


# A linear Map Range feeding a Principled BSDF's Roughness: (from min, from max, to min, to max, clamp).
def roughness_remap(material):
    tree = material.node_tree if material.use_nodes else None
    for n in tree.nodes if tree else []:
        if n.type != "BSDF_PRINCIPLED" or not n.inputs["Roughness"].is_linked:
            continue
        r = n.inputs["Roughness"].links[0].from_node
        if r.type == "MAP_RANGE" and r.data_type == "FLOAT" and r.interpolation_type == "LINEAR":
            v = lambda name: r.inputs[name].default_value
            return (v("From Min"), v("From Max"), v("To Min"), v("To Max"), r.clamp)
    return None


remaps = {m.name: roughness_remap(m) for m in bpy.data.materials if m.users and roughness_remap(m)}

bpy.ops.export_scene.gltf(
    filepath=out,
    export_format="GLTF_SEPARATE",
    use_visible=True,            # instance sources hidden in the viewport stay out
    export_apply=True,           # modifiers (Geometry Nodes) evaluated
    export_gn_mesh=True,         # Geometry Nodes instances as nodes sharing meshes
    export_gpu_instances=False,  # (EXT_mesh_gpu_instancing: the cooker reads plain nodes)
    export_lights=True,
    export_cameras=False,
    export_import_convert_lighting_mode="RAW",
    export_image_format="AUTO",
)

with open(out, encoding="utf-8") as f:
    gltf = json.load(f)
for m in gltf.get("materials", []):
    if m.get("name") in masked:
        m["alphaMode"] = "MASK"
        m["alphaCutoff"] = 0.5
# Remapped roughness: a copy of the material's metallic-roughness texture with G (roughness) mapped.
folder = os.path.dirname(out)
for m in gltf.get("materials", []):
    remap = remaps.get(m.get("name"))
    view = m.get("pbrMetallicRoughness", {}).get("metallicRoughnessTexture")
    if not remap or view is None:
        continue
    src = gltf["textures"][view["index"]]
    uri = gltf["images"][src["source"]]["uri"]
    image = bpy.data.images.load(os.path.join(folder, uri.replace("%20", " ")))
    image.colorspace_settings.name = "Non-Color"
    px = np.array(image.pixels[:], dtype=np.float32).reshape(-1, 4)
    lo, hi, to_lo, to_hi, clamp = remap
    g = to_lo + (px[:, 1] - lo) / max(hi - lo, 1e-6) * (to_hi - to_lo)
    px[:, 1] = np.clip(g, min(to_lo, to_hi), max(to_lo, to_hi)) if clamp else np.clip(g, 0.0, 1.0)
    name = os.path.splitext(uri)[0] + "_" + "".join(c if c.isalnum() else "_" for c in m["name"]) + "_roughness.png"
    remapped = bpy.data.images.new(name, image.size[0], image.size[1], alpha=True)
    remapped.colorspace_settings.name = "Non-Color"
    remapped.pixels.foreach_set(px.ravel())
    remapped.filepath_raw = os.path.join(folder, name)
    remapped.file_format = "PNG"
    remapped.save()
    gltf["images"].append({"uri": name.replace(" ", "%20"), "mimeType": "image/png"})
    gltf["textures"].append({k: v for k, v in src.items() if k != "source"} | {"source": len(gltf["images"]) - 1})
    view["index"] = len(gltf["textures"]) - 1
    print(f"falcon export: {m['name']}: roughness remapped to {to_lo:g}..{to_hi:g} ({name})")

suns = [o.data for o in bpy.context.scene.objects if o.type == "LIGHT" and o.data.type == "SUN" and o.visible_get()]
for light in gltf.get("extensions", {}).get("KHR_lights_punctual", {}).get("lights", []):
    if light.get("type") == "directional" and suns:
        light["intensity"] = suns[0].energy
with open(out, "w", encoding="utf-8") as f:
    json.dump(gltf, f, separators=(",", ":"))
print(f"falcon export: {out} ({len(masked)} cut-out materials)")
