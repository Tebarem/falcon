# Downloads Poly Haven textures (CC0) at 2K JPG: diffuse, OpenGL normal, arm (AO, roughness,
# metal) and displacement (height) for each id, plus its info (name, real-world size in mm) into
# <out>/<group>/<id>/. Files already there are kept (run again to add a map).
import json, pathlib, sys, time, urllib.request

SET = {
    "Wood": ["plywood", "oak_veneer_01", "dark_wood", "laminate_floor_02", "brown_planks_03", "old_planks_02"],
    "Concrete": ["concrete_floor_worn_001", "concrete_floor_02", "concrete_wall_003", "concrete_wall_008"],
    "Plaster": ["beige_wall_001", "grey_plaster", "plastered_wall"],
    "Brick": ["red_brick_03", "medieval_blocks_03", "sandstone_blocks_05"],
    "Metal": ["metal_plate", "blue_metal_plate", "corrugated_iron", "rusty_metal_02"],
    "Tiles": ["interior_tiles", "checkered_pavement_tiles"],
    "Ground": ["asphalt_02", "cobblestone_floor_04", "forrest_ground_01", "ganges_river_pebbles", "rock_01", "coast_sand_01"],
    "Roofing": ["roof_slates_03", "clay_roof_tiles_02"],
}
MAPS = {"Diffuse": "diff", "nor_gl": "nor_gl", "arm": "arm", "Displacement": "disp"}
UA = {"User-Agent": "FalconEngine-texture-fetch/1.0"}


def get(url):
    for attempt in range(4):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120) as r:
                return r.read()
        except Exception as e:
            if attempt == 3:
                raise
            print(f"  retry {url}: {e}", flush=True)
            time.sleep(2 + attempt * 3)


out = pathlib.Path(sys.argv[1])
total = 0
for group, ids in SET.items():
    for id in ids:
        d = out / group / id
        d.mkdir(parents=True, exist_ok=True)
        info = json.loads(get(f"https://api.polyhaven.com/info/{id}"))
        files = json.loads(get(f"https://api.polyhaven.com/files/{id}"))
        meta = {"id": id, "name": info.get("name", id), "dimensions": info.get("dimensions"),
                "authors": info.get("authors"), "categories": info.get("categories"), "maps": {}}
        for key, short in MAPS.items():
            entry = files.get(key, {}).get("2k", {}).get("jpg")
            if not entry:
                print(f"{id}: no 2k jpg {key}", flush=True)
                continue
            name = f"{short}.jpg"
            meta["maps"][short] = name
            if (d / name).exists():
                continue
            data = get(entry["url"])
            (d / name).write_bytes(data)
            total += len(data)
        (d / "info.json").write_bytes(json.dumps(meta, indent=1).encode("utf-8"))
        print(f"{group}/{id}: {sorted(meta['maps'])} {meta['dimensions']}", flush=True)
print(f"done: {total / 1e6:.1f} MB", flush=True)
