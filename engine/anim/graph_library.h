#pragma once

// A project's animation graphs (.fgraph files, anim/anim_graph_compiler.h), compiled against the
// scene's skeletons and clips when first used and again when their file changes, so editing a
// graph never needs a cook. The editor's Animator compiles unsaved text with compile().

#include "anim/anim_graph.h"
#include "anim/anim_graph_compiler.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fx {

class GraphLibrary {
public:
    // `projectDir`: where graph paths are relative to ("" outside a project: paths as given).
    void init(const anim::AnimData& data, std::string projectDir);

    // The graph at `path`; nullptr (logged once per change) when it can't be read or has errors.
    std::shared_ptr<const anim::GraphDefinition> get(const std::string& path);
    // The errors of the last compile of `path` (empty: none, or never compiled).
    const std::vector<anim::CompileMessage>& errors(const std::string& path) const;
    // Compiles `text` as `path` (unsaved edits); when it has no errors, get() returns it until the
    // file changes (with errors, the last good one stays). Returns the compile (errors included).
    std::shared_ptr<const anim::CompiledGraph> compile(const std::string& path, std::string_view text);
    const anim::AnimData* data() const { return data_; }
    const std::string& projectDir() const { return projectDir_; }

private:
    struct Entry {
        std::shared_ptr<const anim::GraphDefinition> definition;
        std::vector<anim::CompileMessage> errors;
        std::filesystem::file_time_type stamp{};
        bool read = false, missing = false;
    };
    std::filesystem::path resolve(const std::string& path) const;

    const anim::AnimData* data_ = nullptr;
    std::string projectDir_;
    std::map<std::string, Entry> entries_;
};

}  // namespace fx
