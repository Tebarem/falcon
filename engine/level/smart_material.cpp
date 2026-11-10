#include "level/smart_material.h"

#include "core/json.h"
#include "level/mesh_ops.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fx::level {

bool parseSmartMaterial(const std::string& json, SmartMaterial& out, std::string& error)
{
    try {
        const Json j = Json::parse(json);
        if (!j.isObject())
            return error = "a smart material is a JSON object", false;
        out = {};
        out.name = j.getString("name");
        out.base = j.getString("base");
        if (const Json* layers = j.get("layers"))
            for (size_t k = 0; k < 3 && k < layers->array.size(); ++k)
                out.layers[k] = layers->array[k].str;
        out.blend = float(j.getNumber("blend", 0.15));
        if (const Json* w = j.get("wear"); w && w->isObject())
            out.wear = wearFromJson(*w, WearKind::Edges);
        if (const Json* g = j.get("grime"); g && g->isObject())
            out.grime = wearFromJson(*g, WearKind::Grime);
        return true;
    } catch (const std::exception& e) {
        return error = e.what(), false;
    }
}

bool readSmartMaterial(const std::string& path, SmartMaterial& out, std::string& error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return error = "Can't read " + path, false;
    std::stringstream text;
    text << f.rdbuf();
    return parseSmartMaterial(text.str(), out, error) || (error = path + ": " + error, false);
}

bool writeSmartMaterial(const std::string& path, const SmartMaterial& s, std::string& error)
{
    JsonWriter w;
    w.beginObject().value("format", 1u).value("name", s.name).value("base", s.base).beginArray("layers");
    for (const std::string& l : s.layers)
        w.value(nullptr, l);
    w.endArray().value("blend", double(s.blend));
    if (s.wear)
        writeWearJson(w, "wear", *s.wear);
    if (s.grime)
        writeWearJson(w, "grime", *s.grime);
    w.endObject();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        return error = "Can't write " + path, false;
    f << Json::parse(w.str()).dump(2) << "\n";
    return true;
}

bool applySmartMaterial(const SmartMaterial& s, PolyMesh& mesh, std::vector<uint8_t>& mask, std::string& error,
                        std::vector<WearSettings>* live)
{
    // Every face's material, then the layers (mask UVs made when there are none).
    Json args = Json::makeObject();
    OpResult r;
    if (!s.base.empty()) {
        args.set("path", Json::makeString(s.base));
        if (!runOp("material", mesh, args, r))
            return error = r.error, false;
        mesh = std::move(r.mesh);
    }
    Json layers = Json::makeObject(), list;
    list.type = Json::Type::Array;
    for (const std::string& l : s.layers)
        list.array.push_back(Json::makeString(l));
    layers.set("materials", std::move(list));
    layers.set("blend", Json::makeNumber(s.blend));
    if (!runOp("layers", mesh, layers, r))
        return error = r.error, false;
    mesh = std::move(r.mesh);
    // The wear and grime made again from nothing (the preset's look, not what was painted before).
    mask.assign(0, 0);
    if (live) {
        live->clear();
        if (s.wear)
            live->push_back(*s.wear);
        if (s.grime)
            live->push_back(*s.grime);
        return true;
    }
    if (s.wear && !generateWear(mesh, *s.wear, mask))
        return error = "no mask UVs for the wear", false;
    if (s.grime && !generateWear(mesh, *s.grime, mask))
        return error = "no mask UVs for the grime", false;
    return true;
}

}  // namespace fx::level
