#include "anim/graph_library.h"

#include "core/log.h"

#include <fstream>
#include <sstream>

namespace fx {

void GraphLibrary::init(const anim::AnimData& data, std::string projectDir)
{
    data_ = &data;
    projectDir_ = std::move(projectDir);
    entries_.clear();
}

std::filesystem::path GraphLibrary::resolve(const std::string& path) const
{
    return projectDir_.empty() ? std::filesystem::path(path) : std::filesystem::path(projectDir_) / path;
}

std::shared_ptr<const anim::GraphDefinition> GraphLibrary::get(const std::string& path)
{
    if (!data_ || path.empty())
        return nullptr;
    Entry& e = entries_[path];
    std::error_code ec;
    const std::filesystem::path file = resolve(path);
    const auto stamp = std::filesystem::last_write_time(file, ec);
    if (ec) {
        if (!e.missing)
            FX_WARN("animation graph %s: cannot read it", path.c_str());
        e = {};
        e.read = e.missing = true;
        e.errors.push_back({"", "cannot read the file"});
        return nullptr;
    }
    if (!e.read || stamp != e.stamp) {
        std::ifstream in(file, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        e.stamp = stamp;
        e.read = true;
        e.missing = false;
        e.definition = nullptr;  // (a file with errors has none)
        compile(path, text.str());
    }
    return e.definition;
}

const std::vector<anim::CompileMessage>& GraphLibrary::errors(const std::string& path) const
{
    static const std::vector<anim::CompileMessage> none;
    const auto it = entries_.find(path);
    return it == entries_.end() ? none : it->second.errors;
}

std::shared_ptr<const anim::CompiledGraph> GraphLibrary::compile(const std::string& path, std::string_view text)
{
    auto graph = std::make_shared<anim::CompiledGraph>();
    if (!data_)
        return graph;
    anim::compileGraph(text, anim::AnimDataSource(*data_), *graph);
    Entry& e = entries_[path];
    e.errors = graph->errors;
    if (graph->ok()) {
        e.definition = anim::GraphDefinition::fromCompiled(graph);
    } else {  // (unsaved edits with errors: the last good one stays)
        for (const anim::CompileMessage& m : graph->errors)
            FX_WARN("animation graph %s: %s%s%s", path.c_str(), m.node.c_str(), m.node.empty() ? "" : ": ", m.text.c_str());
    }
    return graph;
}

}  // namespace fx
