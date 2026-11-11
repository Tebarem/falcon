"""The forest's plants as a scatter library: a glTF holding only their meshes, under names the
terrain's scatter layers use (samples/forest_scatter.json), for `falcon cook --empty --library`.

    python scripts/forest_plants_library.py samples/_src/blender/forest/forest.gltf

reads the glTF that scripts/blender_open_world.py (--forest) + blender_export.py wrote, and writes
plants.gltf beside it (sharing its .bin and textures). Meshes are picked by their Blender names,
which come from the Proximity scatter assets and the trees the build script makes. Then:

    falcon cook --empty samples/cooked/forest.fpak --library samples/_src/blender/forest/plants.gltf
    falcon --scene samples/cooked/forest.fpak --terrain 4 --terrain-hills 0 --terrain-layers grass,dirt
           --terrain-scatter "$(cat samples/forest_scatter.json)" --upscaler fsr --upscale-quality native
           --camera -40,1.8,20,20,3,60

(the README's forest screenshot).
"""
import json
import os
import sys

NAMES = {
    "Plane.013": "grass_a", "Plane.012": "grass_b",
    "Plane.087": "flower_a", "tree.006": "flower_b", "Circle.003": "flower_c", "Icosphere": "flower_d",
    "Circle.008": "flower_e", "Icosphere.004": "flower_f", "Circle": "flower_g", "Circle.007": "flower_h",
    "tree": "bush_a", "Icosphere.002": "bush_b", "Plane.047": "bush_c", "Icosphere.003": "bush_d", "Plane.018": "bush_e",
    "tree.017": "sapling_a", "tree.001": "sapling_b",
    "mesh.012": "rock_a", "mesh.009": "rock_b", "mesh.001": "rock_c",
    "log A": "log_a", "log B": "log_b", "log C": "log_c",
    "fir A": "fir_a", "fir B": "fir_b", "fir C": "fir_c",
    "oak A": "oak_a", "oak B": "oak_b", "oak C": "oak_c",
    "birch A": "birch_a", "birch B": "birch_b",
}

src = sys.argv[1]
g = json.load(open(src, encoding="utf-8"))
found = {m["name"]: i for i, m in enumerate(g["meshes"]) if m["name"] in NAMES}
missing = sorted(set(NAMES) - set(found))
if missing:
    sys.exit(f"missing meshes: {missing}")
# One node per plant at the origin (a library's nodes place nothing; they name the meshes).
meshes, nodes = [], []
for old, new in NAMES.items():
    m = dict(g["meshes"][found[old]])
    m["name"] = new
    nodes.append({"name": new, "mesh": len(meshes)})
    meshes.append(m)
g["meshes"] = meshes
g["nodes"] = nodes
g["scenes"] = [{"name": "plants", "nodes": list(range(len(nodes)))}]
g["scene"] = 0
for key in ("cameras", "extensionsUsed", "extensions"):
    if key == "extensions":
        g.get(key, {}).pop("KHR_lights_punctual", None)
    elif key == "cameras":
        g.pop(key, None)
if "extensionsUsed" in g:
    g["extensionsUsed"] = [e for e in g["extensionsUsed"] if e != "KHR_lights_punctual"]
out = os.path.join(os.path.dirname(src), "plants.gltf")
json.dump(g, open(out, "w", encoding="utf-8", newline="\n"))
print(f"{out}: {len(meshes)} meshes")
