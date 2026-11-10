#include "script/script_entry.h"

#include "core/json.h"

namespace fx {

void readScriptsJson(const Json& entity, std::vector<ScriptEntry>& scripts)
{
    scripts.clear();
    const Json* list = entity.get("scripts");
    if (!list || list->type != Json::Type::Array)
        return;
    for (const Json& s : list->array) {
        if (!s.isObject() || s.getString("class").empty())
            continue;
        ScriptEntry e;
        e.className = s.getString("class");
        e.enabled = s.getBool("enabled", true);
        if (const Json* p = s.get("properties"); p && p->isObject())
            e.properties = p->dump(0);
        scripts.push_back(std::move(e));
    }
}

void writeScriptsJson(JsonWriter& w, const std::vector<ScriptEntry>& scripts)
{
    if (scripts.empty())
        return;
    w.beginArray("scripts");
    for (const ScriptEntry& e : scripts) {
        w.beginObject().value("class", e.className);
        if (!e.enabled)
            w.value("enabled", false);
        if (e.properties != "{}" && !e.properties.empty())
            w.raw("properties", e.properties);
        w.endObject();
    }
    w.endArray();
}

}  // namespace fx
