# content/library from the Poly Haven downloads: <Group>/<Name>.fmat, its textures in
# <Group>/Textures/<Name>/{color,normal,arm,height}.jpg (paths "@falcon/..."), its real-world size,
# its parallax depth (the height map's relief), metallic only for bare metal (the arm map's blue
# says where), and acoustic and footstep presets; License.txt lists them.
import json, pathlib, shutil, sys

src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
# (acoustic preset, surface preset) by id; the group's default otherwise.
GROUP = {"Wood": ("wood", "wood"), "Concrete": ("concrete", "concrete"), "Plaster": ("plaster", "concrete"),
         "Brick": ("brick", "brick"), "Metal": ("metal", "metal"), "Tiles": ("ceramic", "ceramic"),
         "Ground": ("dirt", "dirt"), "Roofing": ("rock", "rock")}
ID = {"checkered_pavement_tiles": ("concrete", "concrete"), "asphalt_02": ("concrete", "concrete"),
      "cobblestone_floor_04": ("rock", "rock"), "ganges_river_pebbles": ("gravel", "gravel"),
      "rock_01": ("rock", "rock"), "coast_sand_01": ("sand", "sand"), "forrest_ground_01": ("dirt", "dirt"),
      "clay_roof_tiles_02": ("ceramic", "ceramic"), "sandstone_blocks_05": ("rock", "rock"),
      "medieval_blocks_03": ("rock", "rock")}
MAPS = {"diff": "color", "nor_gl": "normal", "arm": "arm", "disp": "height"}
# Bare metal (its arm map's blue: where); painted and rusty plates have none.
METALLIC = {"corrugated_iron", "metal_plate"}
# How deep the height map's black lies under the surface (metres, parallax occlusion), by group, then by id.
PARALLAX = {"Wood": 0.004, "Concrete": 0.006, "Plaster": 0.004, "Brick": 0.025, "Metal": 0.003, "Tiles": 0.006,
            "Ground": 0.03, "Roofing": 0.03}
PARALLAX_ID = {"coast_sand_01": 0.008, "asphalt_02": 0.006, "medieval_blocks_03": 0.04, "ganges_river_pebbles": 0.03,
               "rock_01": 0.05, "cobblestone_floor_04": 0.04, "corrugated_iron": 0.01, "laminate_floor_02": 0.002,
               "oak_veneer_01": 0.002, "plywood": 0.002}

if dst.exists():
    shutil.rmtree(dst)
lines = []
for group_dir in sorted(p for p in src.iterdir() if p.is_dir()):
    group = group_dir.name
    for d in sorted(p for p in group_dir.iterdir() if p.is_dir()):
        info = json.loads((d / "info.json").read_text(encoding="utf-8"))
        name = info["name"].strip()
        tex = dst / group / "Textures" / name
        tex.mkdir(parents=True, exist_ok=True)
        for short, out in MAPS.items():
            shutil.copyfile(d / f"{short}.jpg", tex / f"{out}.jpg")
        base = f"@falcon/{group}/Textures/{name}/"
        w, h = (info.get("dimensions") or [1000, 1000])[:2]
        acoustic, surface = ID.get(info["id"], GROUP[group])
        fmat = {
            "format": 1, "name": name, "baseColor": [1, 1, 1, 1], "metallic": 1 if info["id"] in METALLIC else 0, "roughness": 1,
            "emissive": [0, 0, 0], "emissiveStrength": 1, "normalScale": 1, "occlusionStrength": 1,
            "alpha": "opaque", "alphaCutoff": 0.5, "doubleSided": False,
            "size": [round(w / 1000.0, 3), round(h / 1000.0, 3)],
            "parallax": PARALLAX_ID.get(info["id"], PARALLAX[group]),
            "textures": {"baseColor": base + "color.jpg", "normal": base + "normal.jpg",
                         "metallicRoughness": base + "arm.jpg", "occlusion": base + "arm.jpg", "height": base + "height.jpg"},
            "acoustic": {"preset": acoustic}, "surface": {"preset": surface},
        }
        (dst / group / f"{name}.fmat").write_bytes((json.dumps(fmat, indent=2) + "\n").encode("utf-8"))
        authors = ", ".join(sorted((info.get("authors") or {}).keys()))
        lines.append(f"  {group}/{name}  (polyhaven.com/a/{info['id']}; {authors}; {w / 1000:g} x {h / 1000:g} m)")
        print(f"{group}/{name}.fmat", flush=True)

license = """Falcon's material library: textures from Poly Haven (https://polyhaven.com), CC0 1.0 Universal
(public domain dedication, https://creativecommons.org/publicdomain/zero/1.0/). They may be used,
changed and shipped in any project, commercial or not, without attribution (credit is appreciated).

Each material: its colour, OpenGL normal, arm (ambient occlusion, roughness, metalness) and
displacement (height) maps at 2K, as Poly Haven publishes them; "size" in its .fmat is the
real-world width and height in metres one repeat of the texture covers, "parallax" how deep its
height map's relief is drawn.

""" + "\n".join(lines) + "\n"
(dst / "License.txt").write_bytes(license.encode("utf-8"))
print(f"{len(lines)} materials", flush=True)
