#pragma once

// (Scripts S3) The script components on an entity, as the scene edits and prefabs save them and
// the editor edits them: each a C# class (Falcon.Component) by its full name, whether it is
// enabled, and its [Property] values as a JSON object's text (only those set; the class's own
// defaults for the rest). Entity properties are the scene's keys for them (SceneEdits keys).
//
//   "scripts": [{"class": "MyGame.Spinner", "properties": {"Speed": 180}}, {"class": ..., "enabled": false}]

#include <string>
#include <vector>

namespace fx {

struct Json;
class JsonWriter;

struct ScriptEntry {
    std::string className;
    std::string properties = "{}";  // a JSON object
    bool enabled = true;

    bool operator==(const ScriptEntry&) const = default;
};

// The entity object's "scripts" array (nothing when it has none).
void readScriptsJson(const Json& entity, std::vector<ScriptEntry>& scripts);
// Written only when there are any.
void writeScriptsJson(JsonWriter& w, const std::vector<ScriptEntry>& scripts);

}  // namespace fx
