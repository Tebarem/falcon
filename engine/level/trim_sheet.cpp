#include "level/trim_sheet.h"

#include "core/json.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace fx::level {

std::string trimSheetJson(const TrimSheet& sheet)
{
    Json root = Json::makeObject();
    root.set("format", Json::makeNumber(1));
    root.set("material", Json::makeString(sheet.material));
    root.set("aspect", Json::makeNumber(sheet.aspect));
    Json strips;
    strips.type = Json::Type::Array;
    for (const TrimStrip& s : sheet.strips) {
        Json o = Json::makeObject();
        o.set("name", Json::makeString(s.name));
        o.set("v0", Json::makeNumber(s.v0));
        o.set("v1", Json::makeNumber(s.v1));
        strips.array.push_back(std::move(o));
    }
    root.set("strips", std::move(strips));
    return root.dump(2) + "\n";
}

bool parseTrimSheet(const std::string& json, TrimSheet& out, std::string& error)
{
    try {
        const Json root = Json::parse(json);
        if (!root.isObject())
            return error = "a trim sheet is a JSON object", false;
        out = {};
        out.material = root.getString("material");
        out.aspect = float(root.getNumber("aspect", 1.0));
        if (!(out.aspect > 0.0f))
            out.aspect = 1.0f;
        if (const Json* strips = root.get("strips"))
            for (const Json& s : strips->array) {
                TrimStrip t;
                t.name = s.getString("name");
                t.v0 = float(s.getNumber("v0", 0.0));
                t.v1 = float(s.getNumber("v1", 1.0));
                out.strips.push_back(t);
            }
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool readTrimSheet(const std::string& path, TrimSheet& out, std::string& error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return error = "Can't read " + path, false;
    std::stringstream text;
    text << f.rdbuf();
    if (!parseTrimSheet(text.str(), out, error))
        return error = path + ": " + error, false;
    return true;
}

bool writeTrimSheet(const std::string& path, const TrimSheet& sheet, std::string& error)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        return error = "Can't write " + path, false;
    f << trimSheetJson(sheet);
    return bool(f) || (error = "Can't write " + path, false);
}

}  // namespace fx::level
