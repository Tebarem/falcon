#include "anim/anim_events.h"

#include "anim/anim_format.h"
#include "core/json.h"

#include <algorithm>
#include <stdexcept>

namespace fx::anim {

bool parseMarkup(std::string_view text, Markup& out, std::string& error)
{
    out.clear();
    Json doc;
    try {
        doc = Json::parse(text);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    const Json* clips = doc.get("clips");
    if (!clips || !clips->isObject()) {
        error = "no \"clips\"";
        return false;
    }
    const auto percent = [](const Json& j, const char* key) { return std::clamp(float(j.getNumber(key, 0.0)), 0.0f, 1.0f); };
    for (const auto& [name, c] : clips->object) {
        ClipMarkup m;
        if (const Json* sync = c.get("sync"))
            for (const Json& e : sync->array)
                m.sync.push_back({fmt::hashId(e.getString("id").c_str()), std::min(percent(e, "at"), 0.9999f)});
        if (const Json* events = c.get("events"))
            for (const Json& e : events->array) {
                ClipEvent ev;
                ev.id = fmt::hashId(e.getString("id").c_str());
                ev.start = percent(e, "at");
                ev.duration = std::clamp(float(e.getNumber("duration", 0.0)), 0.0f, 1.0f);
                ev.kind = e.getString("type", "marker") == "foot" ? ClipEvent::Foot : ClipEvent::Marker;
                const std::string foot = e.getString("foot");
                ev.foot = foot == "left" ? ClipEvent::Left : foot == "right" ? ClipEvent::Right : ClipEvent::None;
                m.events.push_back(ev);
            }
        std::stable_sort(m.sync.begin(), m.sync.end(), [](const ClipSyncEvent& a, const ClipSyncEvent& b) { return a.start < b.start; });
        out[name] = std::move(m);
    }
    return true;
}

std::string markupPathFor(const std::string& model)
{
    const size_t slash = model.find_last_of('/'), dot = model.find_last_of('.');
    return (dot != std::string::npos && (slash == std::string::npos || dot > slash) ? model.substr(0, dot) : model) + ".fevents";
}

}  // namespace fx::anim
