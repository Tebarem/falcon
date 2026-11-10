// Animation graphs: compiled from JSON (anim/anim_graph_compiler.h documents the format)
// against the skeletons and clips of the imported scene.

#include "cook.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace fx::cook {

namespace {

class SceneSource : public anim::CompileSource {
public:
    explicit SceneSource(const SrcScene& scene) : scene_(scene) {}
    uint32_t skeletonCount() const override { return uint32_t(scene_.skeletons.size()); }
    std::string skeletonName(uint32_t skeleton) const override { return scene_.skeletons[skeleton].name; }
    int32_t clip(const std::string& name, uint32_t skeleton) const override
    {
        for (uint32_t c = 0; c < scene_.clips.size(); ++c)
            if (scene_.clips[c].name == name && scene_.clips[c].skeleton == skeleton)
                return int32_t(c);
        return -1;
    }
    void bones(uint32_t skeleton, std::vector<std::string>& names, std::vector<int32_t>& parents) const override
    {
        names = scene_.skeletons[skeleton].boneNames;
        parents = scene_.skeletons[skeleton].parents;
    }

private:
    const SrcScene& scene_;
};

}  // namespace

bool compileGraph(const SrcScene& scene, const std::string& path, CompiledGraph& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "falcon cook: cannot read graph %s\n", path.c_str());
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    if (anim::compileGraph(text.str(), SceneSource(scene), out))
        return true;
    for (const anim::CompileMessage& m : out.errors)
        std::fprintf(stderr, "falcon cook: graph %s: %s%s%s\n", path.c_str(), m.node.c_str(), m.node.empty() ? "" : ": ",
                     m.text.c_str());
    return false;
}

}  // namespace fx::cook
